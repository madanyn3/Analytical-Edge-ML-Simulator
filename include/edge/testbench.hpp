#pragma once
#include "edge/hierarchy.hpp"

namespace edge {
struct BenchResult {
    std::uint64_t requests{}, requested_bytes{}, first_arrival{}, last_done{}, min_latency{}, max_latency{};
    std::uint64_t evaluated_requests{}, skipped_requests{};
    long double latency_sum{};
    HierarchyStats stats;
    std::vector<CacheState> cache_state;
    ServiceState memory_state;
    std::vector<Completion> evaluated; // Only evaluated rows, never invent skipped timestamps.
};
// No CPU/frontend dependency. Generated stimulus or explicit timestamped reads.
BenchResult run_testbench(const ModelConfig&, const std::vector<Request>* explicit_reads = nullptr);
// Exact CSV header: arrival_cycles,address,bytes. Decimal or 0x unsigned values.
// No sorting: equal arrivals retain file order; decreasing arrivals are rejected.
std::vector<Request> parse_stimulus(std::string_view,const std::string& source,std::uint64_t limit);
std::string report_testbench(const ModelConfig&,const BenchResult&);
}
