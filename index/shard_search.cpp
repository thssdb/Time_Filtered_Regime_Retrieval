#include <faiss/IndexHNSW.h>
#include <faiss/IndexFlat.h>
#include <faiss/IndexIVFFlat.h>
#include <faiss/impl/HNSW.h>
#include <faiss/index_io.h>
#include <faiss/invlists/InvertedLists.h>
#include <faiss/utils/utils.h>

#include <omp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {
constexpr std::size_t D = 42;
constexpr std::size_t MAX_SHARDS = 16;
constexpr std::size_t NPROBE = 16;
constexpr std::size_t COARSE_K = 100;
constexpr std::size_t FINAL_K = 10;
constexpr int HNSW_M = 16;
constexpr int EF_CONSTRUCTION = 200;
constexpr int EF_SEARCH = 256;
constexpr std::uint32_t RANK_FENCE = 128;

void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}

bool env_enabled(const char* name) {
    const char* value = std::getenv(name);
    return value && std::string_view(value) == "1";
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

struct ManifestRow {
    std::size_t shard = 0;
    std::size_t begin = 0;
    std::size_t end = 0;
    std::int64_t min_end = 0;
    std::int64_t max_end = 0;
    std::size_t nlist = 0;
    std::string file;
};

std::vector<ManifestRow> read_manifest(const fs::path& path) {
    std::ifstream in(path);
    require(bool(in), "cannot read shard manifest");
    std::string line;
    require(bool(std::getline(in, line)) &&
                    line == "shard\trank_begin\trank_end\tmin_end\tmax_end\tlocal_nlist\tindex_file",
            "unexpected manifest header");
    std::vector<ManifestRow> rows;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::replace(line.begin(), line.end(), '\t', ' ');
        std::istringstream row(line);
        ManifestRow value;
        require(bool(row >> value.shard >> value.begin >> value.end >>
                         value.min_end >> value.max_end >> value.nlist >> value.file),
                "invalid manifest row");
        rows.push_back(std::move(value));
    }
    require(rows.size() == 4 || rows.size() == 8 || rows.size() == 16,
            "manifest shard count must be 4, 8, or 16");
    return rows;
}

using Weights = std::array<double, D>;

double inverse_norm(const float* values, const Weights& weights) {
    double squared = 0.0;
    for (std::size_t j = 0; j < D; ++j)
        squared += weights[j] * double(values[j]) * double(values[j]);
    require(squared > 0.0 && std::isfinite(squared), "zero or invalid weighted norm");
    return 1.0 / std::sqrt(squared);
}

double weighted_l2(const float* query, const float* vector,
                   const Weights& weights) {
    double value = 0.0;
    for (std::size_t j = 0; j < D; ++j) {
        const double delta = double(query[j]) - double(vector[j]);
        value += weights[j] * delta * delta;
    }
    return std::sqrt(value);
}

struct RouteCandidate {
    float score = 0.0f;
    std::uint32_t global_list = 0;
    std::uint16_t shard = 0;
    std::uint16_t local_list = 0;
};

bool route_better(const RouteCandidate& a, const RouteCandidate& b) {
    return a.score != b.score ? a.score > b.score
                              : a.global_list < b.global_list;
}

struct CosineCandidate {
    double score = 0.0;
    std::uint32_t rank = 0;
    std::uint32_t id = 0;
    const float* vector = nullptr;
};

bool cosine_better(const CosineCandidate& a, const CosineCandidate& b) {
    return a.score != b.score ? a.score > b.score : a.id < b.id;
}

struct BetterIsLowerPriority {
    bool operator()(const CosineCandidate& a, const CosineCandidate& b) const {
        return cosine_better(a, b);
    }
};

struct ShardIndex {
    ManifestRow meta;
    std::unique_ptr<faiss::Index> owner;
    faiss::IndexIVFFlat* ivf = nullptr;
    faiss::IndexHNSW* coarse = nullptr;
    std::unique_ptr<faiss::IndexHNSWFlat> weighted_coarse;
    struct ListView {
        const faiss::idx_t* ids = nullptr;
        const std::uint8_t* codes = nullptr;
        std::size_t count = 0;
        std::uint32_t first_rank = 0;
        std::uint32_t last_rank = 0;
        std::size_t fence_begin = 0;
        std::size_t fence_count = 0;
    };
    struct PackedFencePair {
        std::uint32_t start = 0;
        std::uint32_t end = 0;
    };
    std::vector<ListView> lists;
    // Legacy list-major fence offsets are kept for the frozen skip_binary arm.
    std::vector<std::uint32_t> fences;
    // New locator: [rank block][local list] -> exact ID-slice boundaries.
    std::vector<PackedFencePair> packed_fences;
    std::size_t rank_blocks = 0;
};
static_assert(sizeof(ShardIndex::PackedFencePair) ==
              2 * sizeof(std::uint32_t));

struct PackedLocatorCounters {
    std::uint64_t slice_size_0;
    std::uint64_t slice_size_1;
    std::uint64_t slice_size_2;
    std::uint64_t slice_size_3;
    std::uint64_t ordinary_binary;
    std::uint64_t pair_reads;
};

struct PackedQueryFences {
    const ShardIndex::PackedFencePair* lower = nullptr;
    const ShardIndex::PackedFencePair* upper = nullptr;
};

struct Plan {
    std::size_t shard = 0;
    std::size_t list = 0;
    std::size_t begin = 0;
    std::size_t end = 0;
    bool check_time = false;
};

class Worker {
public:
    Worker(const fs::path& time_store, const fs::path& index_root)
        : ends_(read_file<std::int64_t>(time_store / "times.i64")),
          ids_(read_file<std::uint32_t>(time_store / "time_ids.u32")),
          manifest_(read_manifest(index_root / "manifest.tsv")) {
        require(ends_.size() == ids_.size() && !ends_.empty(),
                "time metadata row counts differ");
        require(std::is_sorted(ends_.begin(), ends_.end()),
                "end times are not sorted");
        rows_ = ends_.size();
        require(std::getenv("SCALER_FILE") && std::getenv("WEIGHT_FILE"), "set SCALER_FILE and WEIGHT_FILE");
        const auto scalers = read_file<double>(std::getenv("SCALER_FILE"));
        require(scalers.size() == 6, "expected three channel scalers");
        std::copy(scalers.begin(), scalers.end(), scalers_.begin());
        const auto weights = read_file<double>(std::getenv("WEIGHT_FILE"));
        require(weights.size() == D, "wrong weight count");
        std::copy(weights.begin(), weights.end(), weights_.begin());
        diagnostic_ = env_enabled("SHARD_COLLECT_STATS");
        verify_packed_ = env_enabled("SHARD_VERIFY_PACKED");
        faiss::set_search_stats_enabled(diagnostic_);
        shard_count_ = manifest_.size();

        std::size_t expected_begin = 0;
        std::vector<std::uint8_t> seen(rows_);
        inv_norms_.assign(rows_, 0.0);
        shards_.reserve(shard_count_);
        for (std::size_t shard_no = 0; shard_no < shard_count_; ++shard_no) {
            const auto& meta = manifest_[shard_no];
            require(meta.shard == shard_no && meta.begin == expected_begin &&
                            meta.end > meta.begin && meta.end <= rows_,
                    "manifest shard ranks are not contiguous");
            require(meta.min_end == ends_[meta.begin] &&
                            meta.max_end == ends_[meta.end - 1],
                    "manifest time bounds differ from frozen metadata");
            if (shard_no)
                require(manifest_[shard_no - 1].max_end < meta.min_end,
                        "equal end times cross a shard boundary");
            expected_begin = meta.end;

            ShardIndex shard;
            shard.meta = meta;
            shard.owner.reset(faiss::read_index((index_root / meta.file).c_str()));
            shard.ivf = dynamic_cast<faiss::IndexIVFFlat*>(shard.owner.get());
            shard.coarse = shard.ivf
                    ? dynamic_cast<faiss::IndexHNSW*>(shard.ivf->quantizer)
                    : nullptr;
            require(shard.ivf && shard.coarse,
                    "expected IVFFlat with HNSW quantizer");
            require(shard.ivf->d == D && shard.ivf->nlist == meta.nlist &&
                            shard.ivf->metric_type == faiss::METRIC_INNER_PRODUCT &&
                            !shard.ivf->by_residual &&
                            shard.ivf->code_size == D * sizeof(float),
                    "unexpected shard index layout");
            require(shard.coarse->hnsw.nb_neighbors(1) == HNSW_M &&
                            shard.coarse->hnsw.efConstruction == EF_CONSTRUCTION,
                    "unexpected HNSW construction settings");
            shard.coarse->hnsw.efSearch = EF_SEARCH;
            auto* array_lists = dynamic_cast<faiss::ArrayInvertedLists*>(
                    shard.ivf->invlists);
            require(array_lists, "expected resident ArrayInvertedLists");

            std::vector<float> centroids(meta.nlist * D);
            shard.coarse->reconstruct_n(0, meta.nlist, centroids.data());
            std::vector<float> weighted(meta.nlist * D);
            for (std::size_t list = 0; list < meta.nlist; ++list) {
                const double inv = inverse_norm(centroids.data() + list * D, weights_);
                for (std::size_t j = 0; j < D; ++j)
                    weighted[list * D + j] = float(double(centroids[list * D + j]) * inv);
            }
            shard.weighted_coarse = std::make_unique<faiss::IndexHNSWFlat>(
                    D, HNSW_M, faiss::METRIC_INNER_PRODUCT);
            shard.weighted_coarse->storage->add(meta.nlist, weighted.data());
            shard.weighted_coarse->ntotal = meta.nlist;
            shard.weighted_coarse->is_trained = true;
            shard.weighted_coarse->hnsw = shard.coarse->hnsw;
            shard.weighted_coarse->hnsw.efSearch = EF_SEARCH;

            std::size_t shard_rows = 0;
            shard.lists.resize(meta.nlist);
            const std::size_t rank_blocks =
                    (meta.end - meta.begin + RANK_FENCE - 1) / RANK_FENCE;
            shard.rank_blocks = rank_blocks;
            shard.fences.reserve(meta.nlist * (rank_blocks + 1));
            shard.packed_fences.resize(rank_blocks * meta.nlist);
            for (std::size_t list = 0; list < meta.nlist; ++list) {
                const std::size_t count = shard.ivf->invlists->list_size(list);
                shard_rows += count;
                const auto* posting_ids = array_lists->get_ids(list);
                const auto* codes = array_lists->get_codes(list);
                require(count > 0 && posting_ids && codes,
                        "empty or inaccessible posting list");
                auto& view = shard.lists[list];
                view.ids = posting_ids;
                view.codes = codes;
                view.count = count;
                view.first_rank = static_cast<std::uint32_t>(posting_ids[0]);
                view.last_rank = static_cast<std::uint32_t>(posting_ids[count - 1]);
                view.fence_begin = shard.fences.size();
                view.fence_count = rank_blocks + 1;
                std::size_t posting_offset = 0;
                for (std::size_t block = 0; block <= rank_blocks; ++block) {
                    const std::size_t boundary = std::min<std::size_t>(
                            meta.end, meta.begin + block * RANK_FENCE);
                    while (posting_offset < count &&
                           posting_ids[posting_offset] <
                                   static_cast<faiss::idx_t>(boundary))
                        ++posting_offset;
                    shard.fences.push_back(
                            static_cast<std::uint32_t>(posting_offset));
                }
                for (std::size_t offset = 0; offset < count; ++offset) {
                    const auto raw_rank = posting_ids[offset];
                    require(raw_rank >= static_cast<faiss::idx_t>(meta.begin) &&
                                    raw_rank < static_cast<faiss::idx_t>(meta.end),
                            "posting leaves shard rank interval");
                    if (offset)
                        require(posting_ids[offset - 1] < raw_rank,
                                "posting IDs are not time-rank sorted");
                    const std::size_t rank = static_cast<std::size_t>(raw_rank);
                    require(!seen[rank]++, "duplicate posting ID");
                    const auto* vector = reinterpret_cast<const float*>(
                            codes + offset * D * sizeof(float));
                    inv_norms_[rank] = inverse_norm(vector, weights_);
                }
                require(shard.fences[view.fence_begin] == 0 &&
                                shard.fences.back() == count,
                        "rank fence does not cover list");
                for (std::size_t block = 0; block < rank_blocks; ++block) {
                    const auto start =
                            shard.fences[view.fence_begin + block];
                    const auto end =
                            shard.fences[view.fence_begin + block + 1];
                    require(start <= end && end <= count,
                            "invalid packed rank-fence pair");
                    shard.packed_fences[block * meta.nlist + list] =
                            ShardIndex::PackedFencePair{start, end};
                }
            }
            require(shard_rows == meta.end - meta.begin,
                    "shard posting count differs from manifest");
            legacy_cache_bytes_ +=
                    shard.lists.size() * sizeof(ShardIndex::ListView) +
                    shard.fences.size() * sizeof(std::uint32_t);
            packed_cache_bytes_ += shard.packed_fences.size() *
                    sizeof(ShardIndex::PackedFencePair);
            if (verify_packed_) verify_packed_locator(shard);
            shards_.push_back(std::move(shard));
        }
        require(expected_begin == rows_ &&
                        std::all_of(seen.begin(), seen.end(), [](auto v) { return v == 1; }),
                "shards do not cover frozen rows exactly once");
        incremental_cache_bytes_ = legacy_cache_bytes_ + packed_cache_bytes_;
    }

    void serve() {
        std::cout << "{\"status\":\"ready\",\"arms\":[\"all_scan\",\"skip_scan\",\"all_binary_packed\",\"skip_binary_packed\"],\"shards\":"
                  << shard_count_ << ",\"cache_bytes\":"
                  << incremental_cache_bytes_
                  << ",\"legacy_cache_bytes\":" << legacy_cache_bytes_
                  << ",\"packed_cache_bytes\":" << packed_cache_bytes_
                  << ",\"packed_verified\":"
                  << (verify_packed_ ? "true" : "false")
                  << ",\"packed_verify_targets\":" << packed_verify_targets_
                  << ",\"packed_verify_mismatches\":" << packed_verify_mismatches_
                  << "}\n"
                  << std::flush;
        std::string arm;
        std::int64_t lo = 0;
        std::int64_t hi = 0;
        std::array<float, D> query{};
        while (std::cin >> arm >> lo >> hi) {
            require(arm == "all_scan" || arm == "skip_scan" ||
                            arm == "all_binary_packed" ||
                            arm == "skip_binary_packed",
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
    static inline std::size_t exact_lower_bound_slice(
            const faiss::idx_t* ids, std::size_t begin, std::size_t end,
            faiss::idx_t target, PackedLocatorCounters* counters) {
        require(begin <= end, "invalid packed lower-bound slice");
        const std::size_t size = end - begin;
        if (counters) {
            switch (size) {
                case 0: ++counters->slice_size_0; break;
                case 1: ++counters->slice_size_1; break;
                case 2: ++counters->slice_size_2; break;
                case 3: ++counters->slice_size_3; break;
                default: ++counters->ordinary_binary; break;
            }
        }
        switch (size) {
            case 0:
                return begin;
            case 1:
                return begin + (ids[begin] < target);
            case 2:
                if (ids[begin] >= target) return begin;
                return begin + 1 + (ids[begin + 1] < target);
            case 3:
                if (ids[begin] >= target) return begin;
                if (ids[begin + 1] >= target) return begin + 1;
                return begin + 2 + (ids[begin + 2] < target);
            default:
                return static_cast<std::size_t>(std::lower_bound(
                        ids + begin, ids + end, target) - ids);
        }
    }

    std::size_t packed_lower_bound(
            const ShardIndex& shard, const ShardIndex::ListView& view,
            std::size_t list, faiss::idx_t target, std::size_t hint,
            const ShardIndex::PackedFencePair* block_pairs,
            PackedLocatorCounters* counters) const {
        if (target <= static_cast<faiss::idx_t>(shard.meta.begin)) return hint;
        if (target >= static_cast<faiss::idx_t>(shard.meta.end)) return view.count;
        require(block_pairs && list < shard.meta.nlist,
                "missing packed rank-block directory");
        const auto pair = block_pairs[list];
        if (counters) ++counters->pair_reads;
        const std::size_t begin = std::max<std::size_t>(pair.start, hint);
        const std::size_t end = std::max<std::size_t>(pair.end, begin);
        require(end <= view.count, "packed rank-fence pair leaves list");
        return exact_lower_bound_slice(view.ids, begin, end, target, counters);
    }

    void verify_packed_locator(const ShardIndex& shard) {
        for (std::size_t list = 0; list < shard.meta.nlist; ++list) {
            const auto& view = shard.lists[list];
            for (std::size_t raw_target = shard.meta.begin;
                 raw_target <= shard.meta.end; ++raw_target) {
                const auto target = static_cast<faiss::idx_t>(raw_target);
                const ShardIndex::PackedFencePair* block_pairs = nullptr;
                if (raw_target < shard.meta.end) {
                    const std::size_t block =
                            (raw_target - shard.meta.begin) / RANK_FENCE;
                    require(block < shard.rank_blocks,
                            "verification target leaves packed directory");
                    block_pairs = shard.packed_fences.data() +
                            block * shard.meta.nlist;
                }
                const std::size_t observed = packed_lower_bound(
                        shard, view, list, target, 0, block_pairs, nullptr);
                std::uint64_t ignored_directory_reads = 0;
                const std::size_t legacy = fenced_lower_bound(
                        shard, view, target, 0, ignored_directory_reads);
                const std::size_t expected = static_cast<std::size_t>(
                        std::lower_bound(view.ids, view.ids + view.count, target) -
                        view.ids);
                ++packed_verify_targets_;
                if (observed != legacy || observed != expected) {
                    ++packed_verify_mismatches_;
                    throw std::runtime_error(
                            "packed lower_bound differs from legacy or exact list search");
                }
            }
        }
    }

    std::size_t fenced_lower_bound(
            const ShardIndex& shard, const ShardIndex::ListView& view,
            faiss::idx_t target, std::size_t hint,
            std::uint64_t& directory_reads) const {
        if (target <= static_cast<faiss::idx_t>(shard.meta.begin)) return hint;
        if (target >= static_cast<faiss::idx_t>(shard.meta.end)) return view.count;
        const std::size_t block =
                (static_cast<std::size_t>(target) - shard.meta.begin) /
                RANK_FENCE;
        require(block + 1 < view.fence_count,
                "rank target leaves fence directory");
        std::size_t begin = shard.fences[view.fence_begin + block];
        std::size_t end = shard.fences[view.fence_begin + block + 1];
        if (diagnostic_) directory_reads += 2;
        begin = std::max(begin, hint);
        end = std::max(end, begin);
        return static_cast<std::size_t>(std::lower_bound(
                view.ids + begin, view.ids + end, target) - view.ids);
    }

    void run_query(const std::string& arm, std::int64_t lo, std::int64_t hi,
                   const float* raw_query) {
        const auto started = Clock::now();
        const bool all_graphs =
                arm == "all_scan" || arm == "all_binary_packed";
        const bool packed =
                arm == "all_binary_packed" || arm == "skip_binary_packed";
        std::array<bool, MAX_SHARDS> active{};
        std::array<bool, MAX_SHARDS> full{};
        std::size_t active_count = 0;
        for (std::size_t s = 0; s < shard_count_; ++s) {
            active[s] = manifest_[s].max_end >= lo && manifest_[s].min_end < hi;
            full[s] = active[s] && manifest_[s].min_end >= lo &&
                      manifest_[s].max_end < hi;
            active_count += active[s];
        }
        require(active_count > 0, "query window intersects no shard");

        std::array<float, D> query{};
        for (std::size_t j = 0; j < D; ++j) {
            const std::size_t channel = j < 6 ? 0 : (j < 24 ? 1 : 2);
            query[j] = float((double(raw_query[j]) - scalers_[2 * channel]) /
                             scalers_[2 * channel + 1]);
        }
        const double query_inv = inverse_norm(query.data(), weights_);
        std::array<double, D> weighted_query{};
        std::array<float, D> route_query{};
        for (std::size_t j = 0; j < D; ++j) {
            weighted_query[j] = weights_[j] * double(query[j]);
            route_query[j] = float(weighted_query[j] * query_inv);
        }

        if (diagnostic_) faiss::hnsw_stats.reset();
        std::vector<RouteCandidate> route;
        route.reserve(shard_count_ * NPROBE);
        for (std::size_t s = 0; s < shard_count_; ++s) {
            if (!all_graphs && !active[s]) continue;
            const auto& shard = shards_[s];
            const std::size_t local_k = std::min<std::size_t>(
                    NPROBE, shard.meta.nlist);
            std::array<float, NPROBE> scores{};
            std::array<faiss::idx_t, NPROBE> labels{};
            shard.weighted_coarse->search(
                    1, route_query.data(), local_k,
                    scores.data(), labels.data());
            if (!active[s]) continue;
            for (std::size_t i = 0; i < local_k; ++i) {
                require(labels[i] >= 0 &&
                                static_cast<std::size_t>(labels[i]) < shard.meta.nlist,
                        "local HNSW returned invalid list");
                const std::uint32_t global = static_cast<std::uint32_t>(
                        s * shard.meta.nlist + static_cast<std::size_t>(labels[i]));
                route.push_back(RouteCandidate{
                        scores[i], global, static_cast<std::uint16_t>(s),
                        static_cast<std::uint16_t>(labels[i])});
            }
        }
        require(!route.empty(), "no active-shard coarse candidates");
        std::sort(route.begin(), route.end(), route_better);
        const std::size_t available_routes = route.size();
        if (route.size() > NPROBE) route.resize(NPROBE);
        require(route.size() == std::min<std::size_t>(NPROBE, available_routes),
                "invalid global nprobe result");
        for (std::size_t i = 1; i < route.size(); ++i)
            require(route[i - 1].global_list != route[i].global_list,
                    "duplicate global list candidate");

        faiss::idx_t global_begin = 0;
        faiss::idx_t global_end = static_cast<faiss::idx_t>(rows_);
        std::uint64_t global_rank_bound_searches = 0;
        std::uint64_t packed_query_fence_shards = 0;
        if (packed) {
            global_begin = std::lower_bound(
                    ends_.begin(), ends_.end(), lo) - ends_.begin();
            global_end = std::lower_bound(
                    ends_.begin() + global_begin, ends_.end(), hi) - ends_.begin();
            if (diagnostic_) global_rank_bound_searches = 2;
        }
        // Deliberately uninitialized for scan arms.  Every element is filled
        // before use by a packed arm, so scan pays no packed-state zeroing.
        std::array<PackedQueryFences, MAX_SHARDS> packed_query_fences;
        if (packed) {
            for (std::size_t s = 0; s < shard_count_; ++s) {
                const auto& shard = shards_[s];
                auto block_base = [&](faiss::idx_t target) {
                    if (target <= static_cast<faiss::idx_t>(shard.meta.begin) ||
                        target >= static_cast<faiss::idx_t>(shard.meta.end))
                        return static_cast<const ShardIndex::PackedFencePair*>(nullptr);
                    const std::size_t block =
                            (static_cast<std::size_t>(target) - shard.meta.begin) /
                            RANK_FENCE;
                    require(block < shard.rank_blocks,
                            "query target leaves packed directory");
                    return shard.packed_fences.data() +
                            block * shard.meta.nlist;
                };
                packed_query_fences[s].lower = block_base(global_begin);
                packed_query_fences[s].upper = block_base(global_end);
                if (diagnostic_) ++packed_query_fence_shards;
            }
        }
        std::vector<Plan> plans;
        plans.reserve(route.size());
        std::uint64_t visited = 0;
        std::uint64_t outside_lists = 0;
        std::uint64_t empty_after_clip = 0;
        std::uint64_t full_lists = 0;
        std::uint64_t clipped_lists = 0;
        std::uint64_t scan_locator_rows = 0;
        std::uint64_t binary_searches = 0;
        std::uint64_t directory_reads = 0;
        std::uint64_t full_scan_rows = 0;
        PackedLocatorCounters packed_counters;
        PackedLocatorCounters* active_packed_counters = nullptr;
        if (packed && diagnostic_) {
            packed_counters = PackedLocatorCounters{};
            active_packed_counters = &packed_counters;
        }
        for (const auto& candidate : route) {
            const auto& shard = shards_[candidate.shard];
            const std::size_t list = candidate.local_list;
            const auto& view = shard.lists[list];
            const std::size_t count = view.count;
            std::size_t begin = 0;
            std::size_t end = count;
            // The two scan arms intentionally reproduce the original Fig. 11
            // full-posting path: no global-rank lower_bound and no legacy or
            // packed locator access.  Only fully covered shards skip the
            // per-posting timestamp predicate.
            if (!packed) {
                const bool check_time = !full[candidate.shard];
                plans.push_back(Plan{
                        candidate.shard, list, begin, end, check_time});
                visited += count;
                if (diagnostic_ && check_time) full_scan_rows += count;
                continue;
            }
            if (view.last_rank < static_cast<std::uint32_t>(global_begin) ||
                view.first_rank >= static_cast<std::uint32_t>(global_end)) {
                if (diagnostic_) ++outside_lists;
                continue;
            }
            const bool lower_cross =
                    view.first_rank < static_cast<std::uint32_t>(global_begin);
            const bool upper_cross =
                    view.last_rank >= static_cast<std::uint32_t>(global_end);
            if (!lower_cross && !upper_cross) {
                if (diagnostic_) ++full_lists;
            } else {
                if (diagnostic_) ++clipped_lists;
                const auto& query_fences =
                        packed_query_fences[candidate.shard];
                if (lower_cross) {
                    begin = packed_lower_bound(
                            shard, view, list, global_begin, 0,
                            query_fences.lower,
                            active_packed_counters);
                    if (diagnostic_) ++binary_searches;
                }
                if (upper_cross) {
                    end = packed_lower_bound(
                            shard, view, list, global_end, begin,
                            query_fences.upper,
                            active_packed_counters);
                    if (diagnostic_) ++binary_searches;
                }
            }
            require(begin <= end && end <= count,
                    "invalid exact list window");
            if (begin == end) {
                if (diagnostic_) ++empty_after_clip;
                continue;
            }
            plans.push_back(Plan{candidate.shard, list, begin, end, false});
            visited += end - begin;
        }

        std::priority_queue<CosineCandidate,
                            std::vector<CosineCandidate>,
                            BetterIsLowerPriority> coarse;
        std::uint64_t scored = 0;
        for (const auto& plan : plans) {
            const auto& shard = shards_[plan.shard];
            const auto& view = shard.lists[plan.list];
            for (std::size_t offset = plan.begin; offset < plan.end; ++offset) {
                const std::size_t rank = static_cast<std::size_t>(view.ids[offset]);
                if (plan.check_time && (ends_[rank] < lo || ends_[rank] >= hi))
                    continue;
                const auto* vector = reinterpret_cast<const float*>(
                        view.codes + offset * D * sizeof(float));
                double dot = 0.0;
                for (std::size_t j = 0; j < D; ++j)
                    dot += weighted_query[j] * double(vector[j]);
                CosineCandidate current{
                        (dot * inv_norms_[rank]) * query_inv,
                        static_cast<std::uint32_t>(rank), ids_[rank], vector};
                if (coarse.size() < COARSE_K) {
                    coarse.push(current);
                } else if (cosine_better(current, coarse.top())) {
                    coarse.pop();
                    coarse.push(current);
                }
                ++scored;
            }
        }

        struct Refined {
            double distance = 0.0;
            std::uint32_t id = 0;
            std::uint32_t rank = 0;
        };
        std::vector<Refined> refined;
        refined.reserve(coarse.size());
        while (!coarse.empty()) {
            const auto value = coarse.top();
            coarse.pop();
            refined.push_back(Refined{
                    weighted_l2(query.data(), value.vector, weights_),
                    value.id, value.rank});
        }
        const std::size_t keep = std::min<std::size_t>(FINAL_K, refined.size());
        std::partial_sort(refined.begin(), refined.begin() + keep, refined.end(),
                          [](const Refined& a, const Refined& b) {
                              return a.distance != b.distance
                                      ? a.distance < b.distance
                                      : a.id < b.id;
                          });
        refined.resize(keep);
        const auto ended = Clock::now();

        std::vector<std::uint32_t> ranks;
        std::vector<std::uint32_t> original_ids;
        std::vector<std::uint32_t> lists;
        ranks.reserve(keep);
        original_ids.reserve(keep);
        for (const auto& hit : refined) {
            ranks.push_back(hit.rank);
            original_ids.push_back(hit.id);
        }
        for (const auto& candidate : route) lists.push_back(candidate.global_list);
        const double search_us =
                std::chrono::duration<double, std::micro>(ended - started).count();
        std::cout << std::setprecision(14) << "{\"r\":";
        print_array(ranks);
        std::cout << ",\"ids\":";
        print_array(original_ids);
        std::cout << ",\"lists\":";
        print_array(lists);
        std::cout << ",\"visited\":" << visited
                  << ",\"scored\":" << scored
                  << ",\"active_shards\":" << active_count
                  << ",\"postings_visited\":" << visited
                  << ",\"search_us\":" << search_us;
        if (diagnostic_) {
            std::cout << ",\"outside_lists\":" << outside_lists
                      << ",\"empty_after_clip\":" << empty_after_clip
                      << ",\"full_lists\":" << full_lists
                      << ",\"clipped_lists\":" << clipped_lists
                      << ",\"scan_locator_rows\":" << scan_locator_rows
                      << ",\"binary_searches\":" << binary_searches
                      << ",\"directory_reads\":" << directory_reads
                      << ",\"full_scan_rows\":" << full_scan_rows
                      << ",\"global_rank_bound_searches\":"
                      << global_rank_bound_searches
                      << ",\"packed_query_fence_shards\":"
                      << packed_query_fence_shards
                      << ",\"packed_pair_reads\":"
                      << (packed ? packed_counters.pair_reads : 0)
                      << ",\"packed_slice_size_0\":"
                      << (packed ? packed_counters.slice_size_0 : 0)
                      << ",\"packed_slice_size_1\":"
                      << (packed ? packed_counters.slice_size_1 : 0)
                      << ",\"packed_slice_size_2\":"
                      << (packed ? packed_counters.slice_size_2 : 0)
                      << ",\"packed_slice_size_3\":"
                      << (packed ? packed_counters.slice_size_3 : 0)
                      << ",\"packed_ordinary_binary\":"
                      << (packed ? packed_counters.ordinary_binary : 0)
                      << ",\"cache_bytes\":" << incremental_cache_bytes_
                      << ",\"legacy_cache_bytes\":" << legacy_cache_bytes_
                      << ",\"packed_cache_bytes\":" << packed_cache_bytes_
                      << ",\"centroid_evals\":" << faiss::hnsw_stats.ndis
                      << ",\"hnsw_expansions\":"
                      << faiss::hnsw_stats.nhops;
        }
        std::cout << "}\n" << std::flush;
    }

    std::size_t rows_ = 0;
    std::size_t shard_count_ = 0;
    std::size_t incremental_cache_bytes_ = 0;
    std::size_t legacy_cache_bytes_ = 0;
    std::size_t packed_cache_bytes_ = 0;
    std::uint64_t packed_verify_targets_ = 0;
    std::uint64_t packed_verify_mismatches_ = 0;
    std::vector<std::int64_t> ends_;
    std::vector<std::uint32_t> ids_;
    std::vector<ManifestRow> manifest_;
    std::vector<ShardIndex> shards_;
    std::vector<double> inv_norms_;
    std::array<double, 6> scalers_{};
    Weights weights_{};
    bool diagnostic_ = false;
    bool verify_packed_ = false;
};
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 3,
                "usage: shard_search TIME_STORE INDEX_ROOT");
        omp_set_dynamic(0);
        omp_set_num_threads(1);
        Worker worker(argv[1], argv[2]);
        worker.serve();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "shard_search: " << error.what() << '\n';
        return 1;
    }
}
