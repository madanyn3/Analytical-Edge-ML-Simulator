#pragma once
// Exact event-driven evaluator for the synthetic tiled cluster (docs/cluster-design.md
// §2–§5). Cores are blocking request sources with private L1s; L1 misses travel as
// packets to the line-interleaved home L2 bank; L2 misses travel to the single memory
// controller. Banks, the controller and every directed link are arbitrated resources.
// Nothing here is a hardware claim: the selected L4R5 board is single-core.
#include "edge/cluster_config.hpp"
#include "edge/hierarchy.hpp"
#include "edge/network.hpp"

namespace edge {
struct ReadRecord {
    std::uint64_t core, index, issue, done; // done - issue = read latency.
    bool operator==(const ReadRecord&) const = default;
};
struct CoreStats {
    std::uint64_t reads{}, latency_sum{}, min_latency{}, max_latency{}, last_done{};
    LevelStats l1;        // references/hits/misses/evictions/fill_bytes (no invalidations).
    bool operator==(const CoreStats&) const = default;
};
struct BankStats {
    LevelStats l2;
    std::uint64_t grants{}, wait_cycles{}, busy_cycles{}; // wait = grant - delivery.
    bool operator==(const BankStats&) const = default;
};
struct ClusterResult {
    std::vector<CoreStats> cores;         // Declaration order.
    std::vector<BankStats> banks;         // Bank index = tile index.
    std::uint64_t memory_requests{}, memory_wire_bytes{}, memory_wait_cycles{};
    std::uint64_t events{}, transactions{};          // Evaluated work only (budgets).
    // periodic_state: reads, cycles and whole periods skipped by exact state repetition.
    // Counters above and per core/bank/link include skipped reads; `reads` does not.
    std::uint64_t skipped_reads{}, skipped_cycles{}, skipped_periods{};
    std::vector<ReadRecord> reads;        // Every evaluated read, in evaluation order.
    std::vector<CacheState> l1_state;     // Per core, L1 line numbers, MRU to LRU.
    std::vector<CacheState> l2_state;     // Per bank, L2 line numbers, MRU to LRU.
    ServiceState memory_state;
    std::vector<noc::LinkStats> links;    // Indexed by Topology::link_index.
};
// finite: evaluates every read exactly. periodic_state: the same evaluation, plus exact
// skipping of whole periods once a normalized state key repeats (design §5); results
// equal finite except that skipped reads are counted, not listed. Budgets apply to
// evaluated work and are enforced, never truncated.
ClusterResult run_cluster(const ClusterConfig&);
// Text report: per-core, per-bank and per-link counters, fairness, final states and the
// effective configuration. Utilizations are busy/makespan ratios of exact counts.
std::string report_cluster(const ClusterConfig&, const ClusterResult&);
}
