#pragma once
#include "edge/fc.hpp"
#include <optional>
#include <string>

namespace edge {
struct CacheGeometry { std::uint64_t line_bytes, sets, ways; };
// Full line numbers, ordered MRU to LRU within each set; {} means empty.
using CacheState = std::vector<std::vector<std::uint64_t>>;
struct CacheCounts {
    std::uint64_t references{}, hits{}, misses{}, evictions{}, fill_bytes{};
    CacheState final_state;
};
std::uint64_t contiguous_lines(std::uint64_t address, std::uint64_t bytes,
                               std::uint64_t line_bytes);
// Explicit read-only finite analysis. Rejects writes and excessive expansion.
CacheCounts analyze_reuse(CacheGeometry, std::span<const Event>,
                         CacheState initial = {}, std::uint64_t limit = 10000);
// Symbolic special case: cold ascending consecutive line sweep repeated R times.
// Counts do not enumerate N*R references. Final state costs O(sets*ways).
CacheCounts analyze_cold_sweep(CacheGeometry, std::uint64_t first_line,
                              std::uint64_t distinct_lines, std::uint64_t repeats);
struct BandwidthDemand {
    std::string boundary;
    std::uint64_t bytes;
    std::optional<double> bytes_per_second;
    std::string provenance;
};
struct RooflineBound {
    double known_lower_bound_seconds{};
    bool complete_roofline{};
    std::vector<std::string> unknowns;
};
// Conditional necessary bounds, never predicted execution time.
// dependency_seconds is an independently derived critical-path lower bound.
RooflineBound roofline_bound(std::uint64_t macs, std::optional<double> macs_per_second,
    const std::string& compute_provenance, std::span<const BandwidthDemand>,
    std::optional<double> dependency_seconds = std::nullopt);
}
