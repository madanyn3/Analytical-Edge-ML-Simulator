#pragma once
#include "edge/config.hpp"

namespace edge {
struct Request { std::uint64_t arrival, address, bytes; };
struct Completion {
    std::uint64_t arrival{}, start{}, done{}, wire_bytes{};
};
struct ServiceState {
    std::uint64_t next_issue{};
    // Outstanding completions as of the most recent admission, ascending order.
    std::vector<std::uint64_t> pending;
    std::uint64_t last_arrival{}; // Preserve FIFO input-order validation across snapshots.
    bool operator==(const ServiceState&) const = default;
};
// FIFO admissions, pipelined fixed-latency beat service, bounded in-flight credits.
// Externally offered arrivals may queue without bound; no requests are dropped.
class MemoryService {
public:
    explicit MemoryService(ServiceConfig, ServiceState initial = {});
    Completion submit(Request);
    // Earliest tick at which a request already waiting could be admitted: next_issue,
    // or the earliest outstanding completion if all credits are still busy then.
    // A request submitted with arrival >= this value starts exactly at its arrival.
    std::uint64_t earliest_admission() const;
    const ServiceState& state() const { return state_; }
private:
    ServiceConfig config_;
    ServiceState state_;
};
struct PeriodicResult {
    std::uint64_t requests{}, requested_bytes{}, wire_bytes{}, first_done{}, last_done{};
    std::uint64_t min_latency{}, max_latency{};
    long double mean_latency{};
    ServiceState final_state;
};
// Closed form for cold service, equally spaced, same-address/width requests.
// Runtime O(outstanding), not O(count); includes exact post-admission pending state.
PeriodicResult periodic_memory(const ServiceConfig&, Request first,
                               std::uint64_t count, std::uint64_t spacing);
}
