#include <faiss/IndexHNSW.h>
#include <faiss/IndexIVFFlat.h>
#include <faiss/index_io.h>

#include <omp.h>

#include <algorithm>
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
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {
constexpr std::size_t D = 42;
constexpr std::size_t NPROBE = 16;
constexpr std::size_t TRAIN_LIMIT = 50'000;
constexpr int HNSW_M = 16;
constexpr int EF_CONSTRUCTION = 200;
constexpr int EF_SEARCH = 256;
constexpr int TRAIN_SEED = 42;
constexpr int TRAIN_ITERATIONS = 20;

void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}

double seconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double>(end - begin).count();
}

std::vector<float> read_vectors(const fs::path& path) {
    const auto bytes = fs::file_size(path);
    require(bytes > 0 && bytes % (D * sizeof(float)) == 0,
            "time_route.f32 extent is not a nonempty 42D FP32 matrix");
    std::vector<float> values(bytes / sizeof(float));
    std::ifstream in(path, std::ios::binary);
    in.read(reinterpret_cast<char*>(values.data()),
            static_cast<std::streamsize>(bytes));
    require(bool(in), "cannot read time_route.f32");
    return values;
}

std::vector<faiss::idx_t> training_rows(std::size_t rows) {
    std::vector<faiss::idx_t> ids(rows);
    std::iota(ids.begin(), ids.end(), 0);
    if (rows <= TRAIN_LIMIT) return ids;
    std::mt19937 generator(TRAIN_SEED);
    std::shuffle(ids.begin(), ids.end(), generator);
    ids.resize(TRAIN_LIMIT);
    std::sort(ids.begin(), ids.end());
    return ids;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 4,
                "usage: build_index TIME_ROUTE_F32 NLIST OUTPUT_FAISS");
        omp_set_dynamic(0);

        const fs::path input(argv[1]);
        const std::size_t nlist = std::stoull(argv[2]);
        const fs::path output(argv[3]);
        require(nlist >= NPROBE, "NLIST must be at least 16");
        require(!fs::exists(output), "output index already exists");

        auto vectors = read_vectors(input);
        const std::size_t rows = vectors.size() / D;
        require(rows >= nlist &&
                        rows <= static_cast<std::size_t>(
                                    std::numeric_limits<faiss::idx_t>::max()),
                "row count is outside supported range");

        auto train_ids = training_rows(rows);
        std::vector<float> training(train_ids.size() * D);
        for (std::size_t i = 0; i < train_ids.size(); ++i) {
            std::memcpy(training.data() + i * D,
                        vectors.data() + static_cast<std::size_t>(train_ids[i]) * D,
                        D * sizeof(float));
        }

        auto quantizer = std::make_unique<faiss::IndexHNSWFlat>(
                D, HNSW_M, faiss::METRIC_INNER_PRODUCT);
        quantizer->hnsw.efConstruction = EF_CONSTRUCTION;
        quantizer->hnsw.efSearch = EF_SEARCH;
        faiss::IndexIVFFlat index(
                quantizer.get(), D, nlist, faiss::METRIC_INNER_PRODUCT);
        index.cp.seed = TRAIN_SEED;
        index.cp.niter = TRAIN_ITERATIONS;
        index.nprobe = NPROBE;
        const std::size_t minimum_training =
                static_cast<std::size_t>(index.cp.min_points_per_centroid) * nlist;
        require(train_ids.size() >= minimum_training,
                "training sample is below FAISS min_points_per_centroid requirement");

        const auto train_begin = Clock::now();
        index.train(static_cast<faiss::idx_t>(train_ids.size()), training.data());
        const auto train_end = Clock::now();
        index.add(static_cast<faiss::idx_t>(rows), vectors.data());
        const auto add_end = Clock::now();
        require(index.ntotal == static_cast<faiss::idx_t>(rows),
                "IVF row count differs after add");

        std::size_t list_total = 0;
        std::size_t empty_lists = 0;
        std::size_t list_min = rows;
        std::size_t list_max = 0;
        std::size_t bitwise_verified = 0;
        std::vector<std::uint8_t> seen(rows);
        for (std::size_t list = 0; list < nlist; ++list) {
            const std::size_t count = index.invlists->list_size(list);
            list_total += count;
            empty_lists += count == 0;
            list_min = std::min(list_min, count);
            list_max = std::max(list_max, count);
            faiss::InvertedLists::ScopedIds ids(index.invlists, list);
            faiss::InvertedLists::ScopedCodes codes(index.invlists, list);
            for (std::size_t offset = 0; offset < count; ++offset) {
                const auto raw_id = ids[offset];
                require(raw_id >= 0 &&
                                static_cast<std::size_t>(raw_id) < rows,
                        "inverted-list ID is outside the time-rank domain");
                const std::size_t id = static_cast<std::size_t>(raw_id);
                require(!seen[id]++, "duplicate inverted-list ID");
                require(std::memcmp(codes.get() + offset * D * sizeof(float),
                                    vectors.data() + id * D,
                                    D * sizeof(float)) == 0,
                        "IVFFlat code differs from standardized 42D FP32 bits");
                ++bitwise_verified;
            }
        }
        require(list_total == rows && bitwise_verified == rows,
                "inverted lists do not cover every row exactly once");

        if (!output.parent_path().empty())
            fs::create_directories(output.parent_path());
        faiss::write_index(&index, output.c_str());
        const auto write_end = Clock::now();

        std::cout << std::setprecision(12)
                  << "{\"status\":\"completed\",\"rows\":" << rows
                  << ",\"dimension\":" << D << ",\"metric\":\"inner_product\""
                  << ",\"nlist\":" << nlist << ",\"nprobe\":" << NPROBE
                  << ",\"quantizer\":\"IndexHNSWFlat\",\"hnsw_M\":" << HNSW_M
                  << ",\"efConstruction\":" << EF_CONSTRUCTION
                  << ",\"efSearch\":" << EF_SEARCH
                  << ",\"cp_seed\":" << TRAIN_SEED
                  << ",\"cp_niter\":" << TRAIN_ITERATIONS
                  << ",\"cp_min_points_per_centroid\":"
                  << index.cp.min_points_per_centroid
                  << ",\"train_limit\":" << TRAIN_LIMIT
                  << ",\"train_vectors\":" << train_ids.size()
                  << ",\"train_sampling\":\"all rows if <=50000; otherwise sorted std::mt19937 seed-42 sample without replacement\""
                  << ",\"list_rows\":" << list_total
                  << ",\"empty_lists\":" << empty_lists
                  << ",\"list_min_rows\":" << list_min
                  << ",\"list_max_rows\":" << list_max
                  << ",\"ivf_code_rows_bitwise_verified\":" << bitwise_verified
                  << ",\"train_seconds\":" << seconds(train_begin, train_end)
                  << ",\"add_seconds\":" << seconds(train_end, add_end)
                  << ",\"write_seconds\":" << seconds(add_end, write_end)
                  << ",\"index_bytes\":" << fs::file_size(output)
                  << ",\"omp_threads\":" << omp_get_max_threads() << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "build_index: " << error.what() << '\n';
        return 1;
    }
}
