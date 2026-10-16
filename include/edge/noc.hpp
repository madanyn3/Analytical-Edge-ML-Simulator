#pragma once
// Network geometry for the tiled cluster (docs/cluster-design.md sections 2 and 4):
// coordinates, directed links, dimension-order XY routing, hop counts, zero-load packet
// latency and the line-interleaved bank mapping. No contention or state lives here.
#include "edge/cluster_config.hpp"
#include <array>

namespace edge::noc {
using U = std::uint64_t;
// Router ports in the FIXED order used for FIFO ties and round-robin pointers.
enum class Port : std::uint8_t { local = 0, minus_x = 1, plus_x = 2, minus_y = 3, plus_y = 4 };
inline constexpr std::size_t port_count = 5;
Port opposite(Port);
struct Coord { U x, y; bool operator==(const Coord&) const = default; };
// A directed link is named by the router it leaves and the output port it uses.
struct Link { U from; Port out; bool operator==(const Link&) const = default; };

class Topology {
public:
    explicit Topology(const TopologyConfig&);
    U k() const { return k_; }
    U tiles() const { return k_ * k_; }
    bool torus() const { return torus_; }
    Coord coord(U tile) const;            // tile = y*k + x
    U tile(Coord) const;
    U neighbor(U tile, Port out) const;   // Throws if the link does not exist (mesh edge).
    bool has_link(U tile, Port out) const;
    // Packet enters the downstream router on the port facing back along the link.
    Port input_port(Link l) const { return opposite(l.out); }
    U link_index(Link l) const { return l.from * port_count + static_cast<U>(l.out); }
    // XY dimension order: all X moves, then all Y moves. On a torus each dimension takes
    // the shorter ring direction; an exact tie (distance k/2) takes the + direction.
    std::vector<Link> route(U source, U target) const;
    U hops(U source, U target) const;     // Closed form; equals route(...).size().
private:
    U k_;
    bool torus_;
    // Signed step (+1, -1 or 0) and length of the chosen move along one dimension.
    std::pair<int, U> axis_move(U from, U to) const;
};
// Flits for a packet with `payload_bytes` of data: one header flit plus ceil(payload/flit).
U flits(U payload_bytes, U flit_bytes);
// Zero-load delivery time of an F-flit packet over H hops:
//   T0(H,F) = H*(t_r + t_l) + (F-1)*t_l  for H >= 1,  and 0 for H = 0 (local, no network).
U zero_load(U hops, U flits, U router_cycles, U link_cycles);

// Line-interleaved shared L2: line = floor(A/L2), bank = line mod B,
// set = floor(line/B) mod S, tag = floor(line/(B*S)). Bank and set fields do not overlap.
struct BankMap {
    U line_bytes, banks, sets;
    U line(U address) const { return address / line_bytes; }
    U bank(U address) const { return line(address) % banks; }
    U set(U address) const { return (line(address) / banks) % sets; }
    U tag(U address) const { return line(address) / banks / sets; }
};
}
