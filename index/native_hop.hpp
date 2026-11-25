#pragma once
#include <faiss/impl/HNSW.h>
#include <cstdint>

namespace nh_hop {
struct Counts {
    std::uint64_t neighbor_checks = 0, neighbor_pruned = 0;
    std::uint64_t expansion_checks = 0, expansion_pruned = 0, entry_bridges = 0;
};

// The frozen four centroid graphs all have max_level == 1. L1 is unrestricted.
struct Params : faiss::SearchParametersHNSW {
    const std::int64_t* min_start = nullptr;
    const std::int64_t* max_end = nullptr;
    std::int64_t lo = 0, hi = 0;
    Counts* counts = nullptr;
    mutable bool reached = false;

    bool overlaps(faiss::idx_t id) const {
        return max_end[id] >= lo && min_start[id] < hi;
    }
};
}
