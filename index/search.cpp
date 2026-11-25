#include <faiss/IndexHNSW.h>
#include <faiss/IndexIVFFlat.h>
#include <faiss/index_io.h>
#include <faiss/invlists/InvertedLists.h>
#include <faiss/utils/distances.h>

#include "route.hpp"


#include <omp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {
constexpr std::size_t D = 42;
constexpr std::size_t K = 10;
constexpr std::size_t NPROBE = 16;
constexpr int HNSW_M = 16;
constexpr int EF_CONSTRUCTION = 200;
constexpr int EF_SEARCH = 256;

#ifndef COLLECT_STATS
#define COLLECT_STATS 0
#endif

void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}

double elapsed_ms(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

template <class T>
std::vector<T> read_file(const fs::path& path) {
    const auto bytes = fs::file_size(path);
    require(bytes > 0 && bytes % sizeof(T) == 0, "invalid input extent");
    std::vector<T> values(bytes / sizeof(T));
    std::ifstream in(path, std::ios::binary);
    in.read(reinterpret_cast<char*>(values.data()),
            static_cast<std::streamsize>(bytes));
    require(bool(in), "cannot read input file");
    return values;
}

template <class T>
void print_array(const std::vector<T>& values) {
    std::cout << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) std::cout << ',';
        std::cout << values[i];
    }
    std::cout << ']';
}

struct Hit {
    float score = 0;
    std::uint32_t time_rank = 0;
    std::uint32_t original_id = 0;
};

bool better(const Hit& left, const Hit& right) {
    if (left.score != right.score) return left.score > right.score;
    return left.original_id < right.original_id;
}

// std::priority_queue puts the element for which this predicate is false on
// top. Treating a better hit as lower priority therefore leaves the current
// worst Top-10 member at top.
struct BetterIsLowerPriority {
    bool operator()(const Hit& left, const Hit& right) const {
        return better(left, right);
    }
};

struct Counters {
    std::uint64_t selected_nonempty_lists = 0;
    std::uint64_t empty_selected_lists = 0;
    std::uint64_t time_pruned_nonempty_lists = 0;
    std::uint64_t scanned_nonempty_lists = 0;
    std::uint64_t selected_rows = 0;
    std::uint64_t time_pruned_rows = 0;
    std::uint64_t scanned_rows = 0;
    std::uint64_t range_comparisons = 0;
    std::uint64_t slice_binary_searches = 0;
    std::uint64_t time_slice_candidate_rows = 0;
    std::uint64_t rows_eliminated_by_slice = 0;
    std::uint64_t timestamp_checks = 0;
    std::uint64_t time_filtered_rows = 0;
    std::uint64_t distance_computations = 0;
    std::uint64_t heap_updates = 0;
    std::uint64_t route_range_checks = 0;
    std::uint64_t route_nodes_pruned = 0;
    std::uint64_t route_edges_considered = 0;
    std::uint64_t route_edges_pruned = 0;
    std::uint64_t route_nodes_expanded = 0;
};

struct ScanPlan {
    std::size_t list = 0;
    std::size_t begin = 0;
    std::size_t end = 0;
};

#include "score.hpp"

const char* required_env(const char* name) {
    const char* value = std::getenv(name);
    if (!value || !*value) throw std::runtime_error(std::string("missing ") + name);
    return value;
}

class Worker {
public:
    Worker(const fs::path& time_store, const fs::path& index_path)
        : index_(faiss::read_index(index_path.c_str())),
          starts_(read_file<std::int64_t>(time_store / "start_times.i64")),
          ends_(read_file<std::int64_t>(time_store / "times.i64")),
          original_ids_(read_file<std::uint32_t>(time_store / "time_ids.u32")) {
        ivf_ = dynamic_cast<faiss::IndexIVFFlat*>(index_.get());
        coarse_ = ivf_ ? dynamic_cast<faiss::IndexHNSW*>(ivf_->quantizer) : nullptr;
        require(ivf_ && coarse_,
                "expected IndexIVFFlat with an IndexHNSW quantizer");
        require(ivf_->d == D &&
                        ivf_->metric_type == faiss::METRIC_INNER_PRODUCT &&
                        coarse_->d == D &&
                        coarse_->metric_type == faiss::METRIC_INNER_PRODUCT,
                "index is not 42D FP32 inner product");
        require(!ivf_->by_residual && ivf_->code_size == D * sizeof(float),
                "expected non-residual IVFFlat codes");
        require(!ivf_->invlists->use_iterator,
                "iterator-only inverted lists are unsupported");
        require(static_cast<std::size_t>(ivf_->nlist) >= NPROBE,
                "index has fewer than 16 lists");
        require(coarse_->hnsw.nb_neighbors(1) == HNSW_M,
                "coarse HNSW M differs from 16");
        require(coarse_->hnsw.efConstruction == EF_CONSTRUCTION,
                "coarse HNSW efConstruction differs from 200");
        coarse_->hnsw.efSearch = EF_SEARCH;
        ivf_->nprobe = NPROBE;

        rows_ = static_cast<std::size_t>(ivf_->ntotal);
        nlist_ = static_cast<std::size_t>(ivf_->nlist);
        require(rows_ >= K && rows_ <= std::numeric_limits<std::uint32_t>::max(),
                "unsupported row count");
        require(starts_.size() == rows_ && ends_.size() == rows_ &&
                        original_ids_.size() == rows_,
                "time-store and index row counts differ");
        require(std::is_sorted(ends_.begin(), ends_.end()),
                "time-store end times are not chronological");
        std::vector<std::uint8_t> id_seen(rows_);
        for (std::size_t row = 0; row < rows_; ++row) {
            require(starts_[row] <= ends_[row],
                    "event start exceeds event end");
            require(original_ids_[row] < rows_ &&
                            !id_seen[original_ids_[row]]++,
                    "time_ids.u32 is not a permutation");
        }

        // The route-code copy is an optional offline startup audit.  It is
        // not required by the deployed worker and is never included in
        // persistent storage or query timing.
        std::vector<float> route_reference;
        const char* verify_route = std::getenv("NANHANG_VERIFY_ROUTE_CODES");
        if (verify_route && std::string_view(verify_route) == "1") {
            route_reference = read_file<float>(time_store / "time_route.f32");
            require(route_reference.size() == rows_ * D,
                    "time_route.f32 row count differs");
        }
        list_min_start_.assign(nlist_, std::numeric_limits<std::int64_t>::max());
        list_max_end_.assign(nlist_, std::numeric_limits<std::int64_t>::min());
        const auto scales=read_file<double>(required_env("SCALER_FILE"));
        require(scales.size()==6,"expected three channel scalers");
        std::copy(scales.begin(),scales.end(),scalers_.begin());
        for(size_t c=0;c<3;++c) require(std::isfinite(scalers_[2*c]) && scalers_[2*c+1]>0,"invalid scaler");
        auto saved_weights=read_file<double>(required_env("WEIGHT_FILE"));
        require(saved_weights.size()==D,"wrong weight count");
        std::copy(saved_weights.begin(),saved_weights.end(),weights_.begin());
        vectors_.resize(rows_);
        inv_norms_.resize(rows_);
        std::vector<uint32_t> list_ids(rows_);
        list_rows_.assign(nlist_, 0);
        list_max_norm_.assign(nlist_, 0.0);
        std::vector<std::uint8_t> row_seen(rows_);
        for (std::size_t list = 0; list < nlist_; ++list) {
            const std::size_t count = ivf_->invlists->list_size(list);
            list_rows_[list] = count;
            if (count == 0) {
                ++empty_lists_;
                continue;
            }
            faiss::InvertedLists::ScopedIds ids(ivf_->invlists, list);
            faiss::InvertedLists::ScopedCodes codes(ivf_->invlists, list);
            for (std::size_t offset = 0; offset < count; ++offset) {
                const auto raw_rank = ids[offset];
                require(raw_rank >= 0 &&
                                static_cast<std::size_t>(raw_rank) < rows_,
                        "inverted-list ID is outside time-rank domain");
                const std::size_t rank = static_cast<std::size_t>(raw_rank);
                if (offset != 0)
                    require(ids[offset - 1] < raw_rank,
                            "inverted-list time ranks are not strictly increasing");
                require(!row_seen[rank]++, "duplicate inverted-list ID");
                if (!route_reference.empty()) {
                    require(std::memcmp(
                                    codes.get() + offset * D * sizeof(float),
                                    route_reference.data() + rank * D,
                                    D * sizeof(float)) == 0,
                            "IVFFlat vector bits differ from frozen standardized 42D store");
                    ++bitwise_verified_rows_;
                }
                const auto* member = reinterpret_cast<const float*>(
                        codes.get() + offset * D * sizeof(float));
                vectors_[rank]=member;
                list_ids[rank]=list;
                inv_norms_[rank]=inverse_norm(member,weights_);
                double squared_norm = 0.0;
                for (std::size_t dimension = 0; dimension < D; ++dimension) {
                    const double value = member[dimension];
                    squared_norm += value * value;
                }
                list_max_norm_[list] = std::max(
                        list_max_norm_[list], std::sqrt(squared_norm));
                list_min_start_[list] =
                        std::min(list_min_start_[list], starts_[rank]);
                list_max_end_[list] =
                        std::max(list_max_end_[list], ends_[rank]);
                ++verified_rows_;
            }
        }
        if(const char* file=std::getenv("LIST_MAP")){
            std::ofstream out(file,std::ios::binary);
            out.write(reinterpret_cast<const char*>(list_ids.data()),rows_*sizeof(uint32_t));
            require(bool(out),"cannot write list membership");
        }
        require(verified_rows_ == rows_,
                "inverted lists do not cover every time rank exactly once");

        // Preserve HNSW topology. Normalize each centroid with the weighted
        // norm so an inner product with the weighted query ranks by cosine.
        std::vector<float> centroids(nlist_ * D);
        coarse_->reconstruct_n(0, static_cast<faiss::idx_t>(nlist_),
                               centroids.data());
        std::vector<float> weighted_centroids(nlist_ * D);
        for (std::size_t list = 0; list < nlist_; ++list) {
            const float* c=centroids.data()+list*D;
            const auto inv=list_rows_[list]?inverse_norm(c,weights_):0.0;
            for(size_t j=0;j<D;++j)
                weighted_centroids[list*D+j]=float(double(c[j])*inv);
        }
        weighted_coarse_ = std::make_unique<faiss::IndexHNSWFlat>(
                D, HNSW_M, faiss::METRIC_INNER_PRODUCT);
        weighted_coarse_->storage->add(
                static_cast<faiss::idx_t>(nlist_), weighted_centroids.data());
        weighted_coarse_->ntotal = static_cast<faiss::idx_t>(nlist_);
        weighted_coarse_->is_trained = true;
        weighted_coarse_->hnsw = coarse_->hnsw;
        weighted_coarse_->hnsw.efSearch = EF_SEARCH;
        hop_metadata_ = std::make_unique<nanhang::ivf_hop_route::Metadata>(
                coarse_->hnsw, list_min_start_, list_max_end_, list_rows_);
    }

    void dump_envelopes(const fs::path& output) const {
        require(!fs::exists(output), "envelope output already exists");
        if (!output.parent_path().empty())
            fs::create_directories(output.parent_path());
        std::ofstream out(output);
        require(bool(out), "cannot create envelope output");
        out << "list_id\trows\tempty\tmin_member_start\tmax_member_end\n";
        for (std::size_t list = 0; list < nlist_; ++list) {
            const bool empty = list_rows_[list] == 0;
            out << list << '\t' << list_rows_[list] << '\t' << empty << '\t';
            if (empty) out << "null\tnull\n";
            else out << list_min_start_[list] << '\t' << list_max_end_[list] << '\n';
        }
        out.close();
        require(bool(out), "cannot finish envelope output");
    }

    void serve(const fs::path& index_path, bool envelope_persisted) {
        std::cout<<"{\"status\":\"ready\"}\n"<<std::flush;
        std::string arm;
        std::int64_t lo = 0;
        std::int64_t hi = 0;
        std::array<float, D> query{};
        while (std::cin >> arm >> lo >> hi) {
            require(arm == "exact" || arm == "native_row" || arm == "native_slice" ||
                            arm == "weighted_row" || arm == "weighted_slice",
                    "invalid benchmark arm");
            require(lo < hi, "invalid half-open time window");
            for (float& value : query)
                require(bool(std::cin >> value) && std::isfinite(value),
                        "invalid raw42 query vector");
            run_query(arm, lo, hi, query.data());
        }
        require(std::cin.eof(), "malformed request stream");
    }

private:
    bool intersects(std::size_t list, std::int64_t lo, std::int64_t hi) const {
        return list_max_end_[list] >= lo && list_min_start_[list] < hi;
    }

    void run_query(const std::string& arm, std::int64_t lo, std::int64_t hi,
                   const float* raw_query) {
        const bool weighted = arm == "weighted_row" || arm == "weighted_slice";
        // Both *_slice arms combine the native L0 one-hop time gate with
        // list-local binary location of the half-open global time-rank range.
        const bool hop = arm == "native_slice" || arm == "weighted_slice";
        const bool slice = hop;
        Counters counters;
        std::array<float, NPROBE> coarse_scores{};
        std::array<faiss::idx_t, NPROBE> coarse_labels{};
        const auto begin = Clock::now();
        std::array<float,D> z;
        for(std::size_t j=0;j<D;++j){
            const auto c=j<6?0:(j<24?1:2);
            z[j]=static_cast<float>((double(raw_query[j])-scalers_[2*c])/scalers_[2*c+1]);
        }
        const float* query=z.data();
        Weights wq;
        std::array<float,D> route_q;
        const auto invq=inverse_norm(query,weights_);
        for(size_t j=0;j<D;++j) {
            wq[j]=weights_[j]*double(query[j]);
            route_q[j]=float(wq[j]*invq);
        }
        if (arm=="exact") { coarse_labels.fill(-1); } else if (hop && coarse_->hnsw.max_level>0) {
            nh_hop::Counts route_counts;
            auto params = hop_metadata_->parameters(lo, hi, &route_counts);
            const faiss::IndexHNSW* router =
                    weighted ? weighted_coarse_.get() : coarse_;
            router->search(1, route_q.data(), NPROBE,
                           coarse_scores.data(), coarse_labels.data(), &params);
            require(params.reached, "native HNSW one-hop hook was not reached");
            if constexpr (COLLECT_STATS != 0) {
                counters.route_range_checks =
                        route_counts.neighbor_checks +
                        route_counts.expansion_checks;
                counters.route_nodes_pruned =
                        route_counts.neighbor_pruned +
                        route_counts.expansion_pruned;
                counters.route_edges_considered = route_counts.neighbor_checks;
                counters.route_edges_pruned = route_counts.neighbor_pruned;
                counters.route_nodes_expanded =
                        route_counts.expansion_checks +
                        route_counts.entry_bridges;
            }
        } else {
            const faiss::IndexHNSW* router =
                    weighted ? weighted_coarse_.get() : coarse_;
            router->search(1, route_q.data(), NPROBE,
                           coarse_scores.data(), coarse_labels.data());
        }
        const auto routed = Clock::now();

        std::vector<std::uint8_t> selected_seen(nlist_);
        faiss::idx_t global_begin = 0;
        faiss::idx_t global_end = static_cast<faiss::idx_t>(rows_);
        if (slice) {
            global_begin = static_cast<faiss::idx_t>(
                    std::lower_bound(ends_.begin(), ends_.end(), lo) - ends_.begin());
            global_end = static_cast<faiss::idx_t>(
                    std::lower_bound(ends_.begin(), ends_.end(), hi) - ends_.begin());
        }
        std::vector<ScanPlan> plans;
        plans.reserve(NPROBE);
        std::vector<std::int64_t> selected_list_ids;
        selected_list_ids.reserve(NPROBE);
        for (const auto raw_list : coarse_labels) {
            if (raw_list < 0) break;
            require(raw_list >= 0 &&
                            static_cast<std::size_t>(raw_list) < nlist_,
                    "coarse HNSW returned an invalid list");
            const std::size_t list = static_cast<std::size_t>(raw_list);
            require(!selected_seen[list]++,
                    "coarse HNSW returned a duplicate list");
            selected_list_ids.push_back(raw_list);
            const std::size_t count = list_rows_[list];
            if (count == 0) {
                ++counters.empty_selected_lists;
                continue;
            }
            ++counters.selected_nonempty_lists;
            counters.selected_rows += count;
            std::size_t begin_offset = 0;
            std::size_t end_offset = count;
            if (slice) {
                faiss::InvertedLists::ScopedIds ids(ivf_->invlists, list);
                begin_offset = static_cast<std::size_t>(
                        std::lower_bound(ids.get(), ids.get() + count, global_begin) - ids.get());
                end_offset = static_cast<std::size_t>(
                        std::lower_bound(ids.get() + begin_offset, ids.get() + count, global_end) - ids.get());
                counters.slice_binary_searches += 2;
                counters.time_slice_candidate_rows += end_offset - begin_offset;
                counters.rows_eliminated_by_slice += count - (end_offset - begin_offset);
            }
            plans.push_back(ScanPlan{list, begin_offset, end_offset});
            ++counters.scanned_nonempty_lists;
            counters.scanned_rows += end_offset - begin_offset;
        }
        const auto gated = Clock::now();

        std::priority_queue<CosineCandidate> coarse_pool;
        auto consider=[&](uint32_t rank,const float* vector) {
            const double cosine=cosine_score(wq,invq,vector,inv_norms_[rank]);
            CosineCandidate c{cosine,rank,original_ids_[rank],vector};
            if(coarse_pool.size()<100) coarse_pool.push(c);
            else if(c<coarse_pool.top()){ coarse_pool.pop();coarse_pool.push(c); }
        };
        size_t count_scored=0;
        if(arm=="exact"){
            auto a=std::lower_bound(ends_.begin(),ends_.end(),lo)-ends_.begin();
            auto b=std::lower_bound(ends_.begin()+a,ends_.end(),hi)-ends_.begin();
            for(auto r=a;r<b;++r)consider(r,vectors_[r]);
            count_scored=b-a; counters.scanned_rows=b-a;
        }
        for (const ScanPlan& plan : plans) {
            const std::size_t list = plan.list;
            const std::size_t count = list_rows_[list];
            faiss::InvertedLists::ScopedIds ids(ivf_->invlists, list);
            faiss::InvertedLists::ScopedCodes codes(ivf_->invlists, list);
            for (std::size_t offset = plan.begin; offset < plan.end; ++offset) {
                const auto raw_rank = ids[offset];
                require(raw_rank >= 0 &&
                                static_cast<std::size_t>(raw_rank) < rows_,
                        "inverted-list ID changed after startup audit");
                const std::size_t rank = static_cast<std::size_t>(raw_rank);
                if (!slice) {
                    if constexpr (COLLECT_STATS != 0) ++counters.timestamp_checks;
                    if (ends_[rank] < lo || ends_[rank] >= hi) {
                        if constexpr (COLLECT_STATS != 0)
                            ++counters.time_filtered_rows;
                        continue;
                    }
                }
                const auto* vector = reinterpret_cast<const float*>(
                        codes.get() + offset * D * sizeof(float));
                consider(rank,vector);++count_scored;
            }
        }
        const auto scanned = Clock::now();

        struct Refined { double distance; uint32_t id, rank; };
        std::vector<Refined> refined;
        refined.reserve(coarse_pool.size());
        while(!coarse_pool.empty()){
            auto c=coarse_pool.top();coarse_pool.pop();
            refined.push_back({weighted_l2(query,c.vector,weights_),c.id,c.rank});
        }
        const auto keep=std::min<size_t>(K,refined.size());
        std::partial_sort(refined.begin(),refined.begin()+keep,refined.end(),
            [](const Refined& a,const Refined& b){
                return a.distance!=b.distance ? a.distance<b.distance : a.id<b.id;
            });
        std::vector<Hit> hits;hits.reserve(keep);
        for(size_t i=0;i<keep;++i)
            hits.push_back({float(-refined[i].distance),refined[i].rank,refined[i].id});
        const auto ranked = Clock::now();

        std::vector<uint32_t> ranks; for(const auto& hit:hits)ranks.push_back(hit.time_rank);
        std::cout<<std::setprecision(14)<<"{\"r\":";print_array(ranks);
        std::cout<<",\"lists\":";print_array(selected_list_ids);
        std::cout<<",\"visited\":"<<counters.scanned_rows<<",\"scored\":"<<count_scored
                 <<",\"search_us\":"<<elapsed_ms(begin,ranked)*1000<<"}\n"<<std::flush;
    }
    std::unique_ptr<faiss::Index> index_;
    faiss::IndexIVFFlat* ivf_ = nullptr;
    faiss::IndexHNSW* coarse_ = nullptr;
    std::size_t rows_ = 0;
    std::size_t nlist_ = 0;
    std::size_t empty_lists_ = 0;
    std::size_t verified_rows_ = 0;
    std::size_t bitwise_verified_rows_ = 0;
    std::vector<std::int64_t> starts_;
    std::vector<std::int64_t> ends_;
    std::vector<std::uint32_t> original_ids_;
    std::vector<const float*> vectors_;
    std::vector<std::int64_t> list_min_start_;
    std::vector<std::int64_t> list_max_end_;
    std::vector<std::size_t> list_rows_;
    std::array<double,6> scalers_;
    Weights weights_;
    std::vector<double> inv_norms_;
    std::vector<double> list_max_norm_;
    std::unique_ptr<faiss::IndexHNSWFlat> weighted_coarse_;
    std::unique_ptr<nanhang::ivf_hop_route::Metadata> hop_metadata_;
};
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 3 || argc == 4, "usage: search TIME_STORE INDEX_FAISS [ENVELOPES_TSV]");
        omp_set_dynamic(0);
        omp_set_num_threads(1);
        Worker worker(argv[1], argv[2]);
        if (argc == 4) worker.dump_envelopes(argv[3]);
        worker.serve(argv[2], argc == 4);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "worker: " << error.what() << '\n';
        return 1;
    }
}
