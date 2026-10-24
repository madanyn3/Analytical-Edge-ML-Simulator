#pragma once
#include "edge/memory.hpp"

namespace edge {
// Approximate cache models for the A4 comparison (docs/fc-cache-theorem.md §5). These are
// labelled approximations, not oracles: they drop the set mapping that the exact analyzers
// (analyze_reuse, analyze_fc_flash) keep.
//
// Reuse distance of a line reference: the number of distinct other lines referenced since
// the previous reference to the same line; first references have no distance.
inline constexpr std::uint64_t no_reuse = ~std::uint64_t{0};
std::vector<std::uint64_t> reuse_distances(std::uint64_t line_bytes, std::span<const Event>);
// P(hit | D) under random independent set mapping (research-plan §7.4): each of the D
// intervening lines maps to the reused line's set with probability 1/S independently,
// and the reference hits iff fewer than W of them do: P(Binomial(D, 1/S) < W).
double random_set_hit_probability(std::uint64_t distance, std::uint64_t sets, std::uint64_t ways);
struct ApproxCounts {
    std::uint64_t references{};
    double fully_associative_misses{};  // LRU with S*W lines and no sets: hit iff D < S*W.
    double random_set_misses{};         // Expected misses under random set mapping.
};
// Call `call_index` (0 = cold, 1 = first warm call) of a stream repeated from an empty cache.
// Warm distances come from the concatenation of two calls (first references then reach
// back cyclically into the previous call).
ApproxCounts approximate_misses(CacheGeometry, std::span<const Event>, std::uint64_t call_index,
                                std::uint64_t limit = 1000000);
}
