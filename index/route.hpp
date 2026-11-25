#pragma once

#include "native_hop.hpp"

#include <faiss/impl/HNSW.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace nanhang::ivf_hop_route {

// Constructs the L0 self-plus-outgoing-neighbor interval required by the
// FAISS hook. Layers above L0 retain unrestricted navigation.
class Metadata {
public:
    Metadata(
            const faiss::HNSW& graph,
            const std::vector<std::int64_t>& list_min_start,
            const std::vector<std::int64_t>& list_max_end,
            const std::vector<std::size_t>& list_rows)
            : graph_(graph),
              list_min_start_(list_min_start),
              list_max_end_(list_max_end),
              list_rows_(list_rows) {
        const std::size_t nlist = list_rows_.size();
        require(graph_.levels.size() == nlist &&
                        list_min_start_.size() == nlist &&
                        list_max_end_.size() == nlist,
                "centroid/list metadata extents differ");
        minimum_.resize(nlist);
        maximum_.resize(nlist);
        build();
    }

    const std::int64_t* minimum_data() const { return minimum_.data(); }
    const std::int64_t* maximum_data() const { return maximum_.data(); }
    std::size_t size() const { return minimum_.size(); }
    std::size_t bytes() const {
        return (minimum_.size() + maximum_.size()) * sizeof(std::int64_t);
    }
    bool overlaps(std::size_t centroid, std::int64_t lo, std::int64_t hi) const {
        require(centroid < size(), "centroid outside metadata");
        return maximum_[centroid] >= lo && minimum_[centroid] < hi;
    }

    nh_hop::Params parameters(
            std::int64_t lo,
            std::int64_t hi,
            nh_hop::Counts* counts) const {
        require(lo < hi, "invalid time interval");
        nh_hop::Params params;
        params.efSearch = 256;
        params.bounded_queue = graph_.search_bounded_queue;
        params.check_relative_distance = graph_.check_relative_distance;
        params.min_start = minimum_data();
        params.max_end = maximum_data();
        params.lo = lo;
        params.hi = hi;
        params.counts = counts;
        return params;
    }

private:
    static void require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }
    void include(
            std::size_t list,
            std::int64_t& minimum,
            std::int64_t& maximum) const {
        if (list_rows_[list] == 0) return;
        minimum = std::min(minimum, list_min_start_[list]);
        maximum = std::max(maximum, list_max_end_[list]);
    }
    void build() {
        for (std::size_t centroid = 0; centroid < size(); ++centroid) {
            std::int64_t minimum = std::numeric_limits<std::int64_t>::max();
            std::int64_t maximum = std::numeric_limits<std::int64_t>::min();
            include(centroid, minimum, maximum);
            std::size_t begin = 0, end = 0;
            graph_.neighbor_range(
                    static_cast<faiss::HNSW::storage_idx_t>(centroid),
                    0, &begin, &end);
            for (std::size_t position = begin; position < end; ++position) {
                const auto neighbor = graph_.neighbors[position];
                if (neighbor < 0) break;
                require(static_cast<std::size_t>(neighbor) < size(),
                        "invalid neighbor in centroid graph");
                include(static_cast<std::size_t>(neighbor), minimum, maximum);
            }
            minimum_[centroid] = minimum;
            maximum_[centroid] = maximum;
        }
    }

    const faiss::HNSW& graph_;
    const std::vector<std::int64_t>& list_min_start_;
    const std::vector<std::int64_t>& list_max_end_;
    const std::vector<std::size_t>& list_rows_;
    std::vector<std::int64_t> minimum_;
    std::vector<std::int64_t> maximum_;
};

} // namespace nanhang::ivf_hop_route
