#include <faiss/IndexHNSW.h>
#include <faiss/IndexIVFFlat.h>
#include <faiss/index_io.h>
#include <faiss/invlists/InvertedLists.h>

#include <omp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {
constexpr std::size_t D = 42;
constexpr std::size_t SHARDS = 16;
constexpr int HNSW_M = 16;
constexpr int EF_CONSTRUCTION = 200;
constexpr int EF_SEARCH = 256;
constexpr int TRAIN_SEED = 42;
constexpr int TRAIN_ITERATIONS = 20;

void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
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

std::vector<std::size_t> shard_boundaries(
        const std::vector<std::int64_t>& ends) {
    const std::size_t rows = ends.size();
    require(rows >= SHARDS, "too few rows for sixteen shards");
    require(std::is_sorted(ends.begin(), ends.end()),
            "end times are not sorted");
    std::vector<std::size_t> bounds(SHARDS + 1);
    bounds.front() = 0;
    bounds.back() = rows;
    std::size_t previous = 0;
    for (std::size_t shard = 1; shard < SHARDS; ++shard) {
        const std::size_t remaining = SHARDS - shard;
        std::size_t ideal = (rows * shard + SHARDS / 2) / SHARDS;
        ideal = std::max(ideal, previous + 1);
        ideal = std::min(ideal, rows - remaining);

        std::size_t right = ideal;
        while (right < rows && ends[right - 1] == ends[right]) ++right;
        std::size_t left = ideal;
        while (left > previous && ends[left - 1] == ends[ideal]) --left;

        const bool right_ok = right > previous && right <= rows - remaining;
        const bool left_ok = left > previous && left <= rows - remaining;
        require(right_ok || left_ok,
                "cannot split equal-row shards without separating tied ends");
        std::size_t cut = 0;
        if (right_ok && left_ok) {
            cut = (right - ideal <= ideal - left) ? right : left;
        } else {
            cut = right_ok ? right : left;
        }
        require(cut > previous && ends[cut - 1] < ends[cut],
                "shard cut separates equal end times");
        bounds[shard] = cut;
        previous = cut;
    }
    for (std::size_t shard = 0; shard < SHARDS; ++shard)
        require(bounds[shard] < bounds[shard + 1], "empty shard");
    return bounds;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 5,
                "usage: build_shards TIME_ROUTE_F32 TIMES_I64 TOTAL_NLIST OUTPUT_DIR");
        omp_set_dynamic(0);
        omp_set_num_threads(1);

        const fs::path vector_path(argv[1]);
        const fs::path time_path(argv[2]);
        const std::size_t total_nlist = std::stoull(argv[3]);
        const fs::path output(argv[4]);
        require(total_nlist % SHARDS == 0, "total nlist is not divisible by 16");
        const std::size_t local_nlist = total_nlist / SHARDS;
        require(local_nlist == 8 || local_nlist == 16,
                "expected 128 or 256 total lists");
        require(!fs::exists(output), "output directory already exists");

        auto vectors = read_file<float>(vector_path);
        auto ends = read_file<std::int64_t>(time_path);
        require(vectors.size() % D == 0 && vectors.size() / D == ends.size(),
                "42D vectors and end-time rows differ");
        const std::size_t rows = ends.size();
        require(rows <= std::numeric_limits<std::uint32_t>::max(),
                "row count does not fit uint32");
        const auto bounds = shard_boundaries(ends);

        fs::create_directories(output);
        std::ofstream manifest(output / "manifest.tsv");
        require(bool(manifest), "cannot create manifest");
        manifest << "shard\trank_begin\trank_end\tmin_end\tmax_end\tlocal_nlist\tindex_file\n";
        std::vector<std::uint32_t> membership(rows,
                std::numeric_limits<std::uint32_t>::max());
        std::size_t verified = 0;
        const auto started = Clock::now();

        for (std::size_t shard = 0; shard < SHARDS; ++shard) {
            const std::size_t begin = bounds[shard];
            const std::size_t end = bounds[shard + 1];
            const std::size_t count = end - begin;
            const std::size_t min_training = 39 * local_nlist;
            require(count >= min_training,
                    "shard is below FAISS min-points-per-centroid requirement");

            auto quantizer = std::make_unique<faiss::IndexHNSWFlat>(
                    D, HNSW_M, faiss::METRIC_INNER_PRODUCT);
            quantizer->hnsw.efConstruction = EF_CONSTRUCTION;
            quantizer->hnsw.efSearch = EF_SEARCH;
            faiss::IndexIVFFlat index(
                    quantizer.get(), D, local_nlist,
                    faiss::METRIC_INNER_PRODUCT);
            index.cp.seed = TRAIN_SEED;
            index.cp.niter = TRAIN_ITERATIONS;
            index.nprobe = std::min<std::size_t>(16, local_nlist);
            index.train(static_cast<faiss::idx_t>(count),
                        vectors.data() + begin * D);

            std::vector<faiss::idx_t> ids(count);
            std::iota(ids.begin(), ids.end(), static_cast<faiss::idx_t>(begin));
            index.add_with_ids(static_cast<faiss::idx_t>(count),
                               vectors.data() + begin * D, ids.data());
            require(index.ntotal == static_cast<faiss::idx_t>(count),
                    "shard index row count differs");

            std::size_t shard_verified = 0;
            for (std::size_t list = 0; list < local_nlist; ++list) {
                const std::size_t list_size = index.invlists->list_size(list);
                faiss::InvertedLists::ScopedIds list_ids(index.invlists, list);
                faiss::InvertedLists::ScopedCodes codes(index.invlists, list);
                for (std::size_t offset = 0; offset < list_size; ++offset) {
                    const faiss::idx_t raw_rank = list_ids[offset];
                    require(raw_rank >= static_cast<faiss::idx_t>(begin) &&
                                    raw_rank < static_cast<faiss::idx_t>(end),
                            "posting ID leaves shard rank interval");
                    if (offset)
                        require(list_ids[offset - 1] < raw_rank,
                                "posting IDs are not sorted by time rank");
                    const std::size_t rank = static_cast<std::size_t>(raw_rank);
                    require(membership[rank] ==
                                    std::numeric_limits<std::uint32_t>::max(),
                            "duplicate posting ID");
                    membership[rank] = static_cast<std::uint32_t>(
                            shard * local_nlist + list);
                    require(std::memcmp(
                                    codes.get() + offset * D * sizeof(float),
                                    vectors.data() + rank * D,
                                    D * sizeof(float)) == 0,
                            "stored vector differs from frozen 42D FP32 bits");
                    ++shard_verified;
                }
            }
            require(shard_verified == count,
                    "shard postings do not cover all rows");
            verified += shard_verified;

            std::ostringstream filename;
            filename << "shard_" << std::setw(2) << std::setfill('0')
                     << shard << ".faiss";
            const fs::path index_path = output / filename.str();
            faiss::write_index(&index, index_path.c_str());
            manifest << shard << '\t' << begin << '\t' << end << '\t'
                     << ends[begin] << '\t' << ends[end - 1] << '\t'
                     << local_nlist << '\t' << filename.str() << '\n';
        }
        manifest.close();
        require(bool(manifest), "cannot finish manifest");
        require(verified == rows, "posting verification total differs");
        for (const auto value : membership)
            require(value != std::numeric_limits<std::uint32_t>::max(),
                    "unassigned row in membership array");
        std::ofstream members(output / "list_members.u32", std::ios::binary);
        members.write(reinterpret_cast<const char*>(membership.data()),
                      static_cast<std::streamsize>(membership.size() * sizeof(std::uint32_t)));
        members.close();
        require(bool(members), "cannot finish list membership array");

        const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
        std::cout << std::setprecision(12)
                  << "{\"status\":\"completed\",\"rows\":" << rows
                  << ",\"dimension\":" << D << ",\"shards\":" << SHARDS
                  << ",\"total_nlist\":" << total_nlist
                  << ",\"local_nlist\":" << local_nlist
                  << ",\"cp_seed\":" << TRAIN_SEED
                  << ",\"cp_niter\":" << TRAIN_ITERATIONS
                  << ",\"efConstruction\":" << EF_CONSTRUCTION
                  << ",\"efSearch\":" << EF_SEARCH
                  << ",\"verified_rows\":" << verified
                  << ",\"seconds\":" << seconds << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "build_shards: " << error.what() << '\n';
        return 1;
    }
}
