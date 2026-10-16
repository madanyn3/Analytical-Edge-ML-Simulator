#pragma once
#include "edge/service.hpp"

namespace edge {
struct LevelStats {
    std::uint64_t references{}, hits{}, misses{}, evictions{}, invalidations{}, fill_bytes{};
    bool operator==(const LevelStats&) const = default;
};
struct HierarchyStats {
    std::vector<LevelStats> levels;
    std::uint64_t memory_requests{}, memory_requested_bytes{}, memory_wire_bytes{};
};
// Finite stateful fallback for ordered miss routing. One top-level request completes
// before the next enters any cache. MemoryService can also be tested independently.
// This is not the symbolic Falcon algorithm and is not an ART implementation.
class Hierarchy {
public:
    explicit Hierarchy(ModelConfig);
    Completion submit(const std::string& entry, Request);
    const HierarchyStats& stats() const { return stats_; }
    const std::vector<CacheState>& cache_state() const { return state_; }
    const ServiceState& memory_state() const { return memory_.state(); }
    // Complete boundary state modulo time translation. Used to PROVE a fixed point
    // before skipping repeated closed-loop stimulus blocks, not to guess warm hits.
    std::vector<std::uint64_t> boundary_key(std::uint64_t now) const;
    void skip_fixed_blocks(std::uint64_t boundary, std::uint64_t elapsed,
                           const HierarchyStats& before, std::uint64_t blocks);
private:
    ModelConfig config_;
    MemoryService memory_;
    std::vector<CacheState> state_;
    HierarchyStats stats_;
    std::uint64_t ready_{}, last_arrival_{}, transactions_{};
    std::uint64_t access(std::size_t level, std::uint64_t address, std::uint64_t bytes, std::uint64_t arrival);
    void invalidate_above(std::size_t level, std::uint64_t line);
};
}
