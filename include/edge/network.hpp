#pragma once
// Exact link-contention model for the tiled cluster (docs/cluster-design.md §4.2–4.3, §5).
// Each directed link is a resource held for F*t_l cycles per packet. Packets wait in
// unbounded router buffers (virtual cut-through, NW1–NW3). The class is event driven:
// it never ticks idle cycles; a caller advances it between event times it reports.
#include "edge/noc.hpp"
#include <map>
#include <optional>

namespace edge::noc {
struct Packet {
    U id;       // Caller-unique; final FIFO tie-break after ready time and input port.
    U source, target, flits;
};
struct Delivery { U id, time; };
struct LinkStats {
    U packets{}, flits{}, busy_cycles{}, wait_cycles{}; // wait = grant - ready, summed.
    bool operator==(const LinkStats&) const = default;
};
class Network {
public:
    Network(const TopologyConfig&);
    // Register a packet entering its source router at `time`. Local packets
    // (source == target) are delivered at `time` with no network cost (BK2).
    // Times must not precede the last processed event time.
    void inject(const Packet&, U time);
    // Earliest pending delivery or possible link grant, if any work remains.
    std::optional<U> next_time() const;
    // Phase 1 at tick t: remove and return deliveries at exactly t, ordered by id.
    std::vector<Delivery> deliveries(U t);
    // Phase 3 at tick t: every link whose grant time is exactly t grants one packet.
    // Grants at t only create events at >= t + t_l > t, so link order within t is free.
    void grants(U t);
    const LinkStats& link_stats(Link l) const { return stats_[topology_.link_index(l)]; }
    const Topology& topology() const { return topology_; }
    bool idle() const;
private:
    struct Waiting { U ready; Port in; U id; std::size_t hop; };
    struct InFlight { Packet packet; std::vector<Link> path; };
    struct LinkState { std::vector<Waiting> queue; U free{}; std::uint8_t pointer{}; };
    Topology topology_;
    U router_, link_;
    bool round_robin_;
    U now_{};
    std::vector<LinkState> links_;
    std::vector<LinkStats> stats_;
    std::map<U, InFlight> flights_;            // Packets still crossing links, by id.
    std::vector<Delivery> pending_;            // Known at the final link's grant.
    // Link grant rule: t = max(free, earliest waiting ready). Empty queue -> none.
    std::optional<U> grant_time(const LinkState&) const;
};
}
