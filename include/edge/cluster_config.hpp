#pragma once
// Configuration version 2: a synthetic k×k tiled multi-core (docs/cluster-design.md).
// Each tile holds a core slot, a private L1, one line-interleaved L2 bank and a router;
// one memory controller attaches to tile mc_tile. Every value is an explicit input with
// provenance; nothing here describes the single-core L4R5 board.
#include "edge/config.hpp"

namespace edge {
struct TopologyConfig {
    std::string kind;              // "mesh" or "torus" (torus needs k >= 3).
    std::uint64_t k{};             // Grid side; tiles = k*k, tile i = (i mod k, i div k).
    std::uint64_t mc_tile{};       // Tile hosting the memory controller.
    std::uint64_t router_cycles{}; // t_r: per-router delay before contending for a link.
    std::uint64_t link_cycles{};   // t_l >= 1: link occupancy per flit.
    std::uint64_t flit_bytes{};    // Payload bytes per flit; packets add one header flit.
    std::string link_arbitration;  // "fifo" or "round_robin" (pointers start at port 0).
    std::string provenance;
    bool operator==(const TopologyConfig&) const = default;
};
struct LevelConfig {
    CacheGeometry geometry{};      // For L2: per bank.
    std::uint64_t lookup_cycles{}, fill_cycles{};
    std::string arbitration;       // L2 banks only: fifo | round_robin | fixed_priority.
    std::string provenance;
    bool operator==(const LevelConfig& o) const {
        return geometry.line_bytes==o.geometry.line_bytes && geometry.sets==o.geometry.sets &&
               geometry.ways==o.geometry.ways && lookup_cycles==o.lookup_cycles &&
               fill_cycles==o.fill_cycles && arbitration==o.arbitration && provenance==o.provenance;
    }
};
struct CoreConfig {
    std::string name;
    std::uint64_t tile{}, start_cycles{}, compute_cycles{}, count{};
    // Read k is at address + (k mod address_period)*stride, bytes wide.
    std::uint64_t address{}, bytes{}, stride{}, address_period{};
    bool operator==(const CoreConfig&) const = default;
};
struct ClusterStimulus {
    std::string engine;            // "finite" or "periodic_state".
    std::uint64_t max_requests{}, max_transactions{}, max_events{};
    bool operator==(const ClusterStimulus&) const = default;
};
struct ClusterConfig {
    std::uint64_t clock_hz{};
    TopologyConfig topology;
    LevelConfig l1, l2;            // L1 replicated per core; L2 replicated per bank.
    ServiceConfig memory;
    std::vector<CoreConfig> cores; // Declaration order.
    ClusterStimulus stimulus;
    bool operator==(const ClusterConfig& o) const {
        return clock_hz==o.clock_hz && topology==o.topology && l1==o.l1 && l2==o.l2 &&
               memory.first_beat_cycles==o.memory.first_beat_cycles &&
               memory.beat_bytes==o.memory.beat_bytes && memory.beat_cycles==o.memory.beat_cycles &&
               memory.issue_cycles==o.memory.issue_cycles && memory.outstanding==o.memory.outstanding &&
               memory.provenance==o.memory.provenance && cores==o.cores && stimulus==o.stimulus;
    }
};
// Reads only [model] version, after full lexical validation. Returns 1 or 2.
std::uint64_t config_version(std::string_view text, const std::string& source = "<config>");
std::uint64_t config_file_version(const std::string& path);
ClusterConfig parse_cluster_config(std::string_view text, const std::string& source = "<config>");
ClusterConfig load_cluster_config(const std::string& path);
void validate_cluster_config(const ClusterConfig&);
// Deterministic effective configuration; parse_cluster_config(describe(c)) == c.
std::string describe_cluster_config(const ClusterConfig&);
}
