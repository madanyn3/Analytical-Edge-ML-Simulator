#pragma once
// Closed-form results for the synthetic tiled cluster (docs/cluster-design.md §6).
// Nothing here runs the event evaluator: every quantity is arithmetic on the
// configuration, so the tests can compare it against run_cluster as an independent
// reference. Distances are recomputed per dimension instead of walking routes.
#include "edge/cluster_config.hpp"

namespace edge::analytic {
using U = std::uint64_t;

// --- N1: zero-load latencies ------------------------------------------------------
// Distance along one dimension between coordinates a and b: |a - b| on a mesh,
// min(d, k - d) on a torus ring. XY routing makes H the sum over both dimensions.
U axis_distance(const TopologyConfig&, U a, U b);
U hops(const TopologyConfig&, U from_tile, U to_tile);
// S(a) = sum over a' in [0, k) of axis_distance(a, a'), in closed form:
//   mesh:  a(a+1)/2 + (k-1-a)(k-a)/2       (distances to the left plus to the right)
//   torus: floor(k^2/4)                    (independent of a; ring symmetry)
U axis_distance_sum(const TopologyConfig&, U a);
// Sum over all k^2 tiles h of H(tile, h) = k*(S(x) + S(y)): each dimension's sum is
// repeated once per value of the other coordinate.
U hop_sum_from(const TopologyConfig&, U tile);
// T0(H, F) = H(t_r + t_l) + (F - 1) t_l for H >= 1, 0 for H = 0, with
// F = 1 + ceil(payload / flit_bytes).
U packet_time(const TopologyConfig&, U hops, U payload_bytes);
// Memory latency of one aligned L2 line: D = first_beat + (b - 1) beat_cycles,
// b = ceil(L2 line / beat_bytes).
U memory_line_cycles(const ClusterConfig&);

struct ZeroLoadRead {
    U l1_hit, l2_hit, l2_miss;
    bool operator==(const ZeroLoadRead&) const = default;
};
// Latency of one read by the core on core_tile whose line lives in `bank`, with every
// bank, link and the memory controller idle (design §6 N1):
//   L1 hit:  lookup_L1
//   L2 hit:  lookup_L1 + T0(H(c,h), 0 B) + lookup_L2 + T0(H(h,c), L1 line) + fill_L1
//   L2 miss: L2 hit + T0(H(h,m), 0 B) + D + T0(H(m,h), L2 line) + fill_L2
ZeroLoadRead zero_load_read(const ClusterConfig&, U core_tile, U bank);
// Sum of the zero-load L2-hit latency over all B = k^2 banks, in closed form:
//   B(lookup_L1 + lookup_L2 + fill_L1) + 2(t_r + t_l) k (S(x) + S(y))
//     + (B - 1)(F_L1 - 1) t_l.
// The last term counts only non-local banks: T0 is not affine in H because H = 0
// costs nothing. Divided by B it is the mean over a uniformly distributed line.
U l2_hit_sum_over_banks(const ClusterConfig&, U core_tile);

// --- N3 / K3a: closed-loop sources on the memory service ------------------------------
// P sources, each with constant compute Z, attached directly to a FIFO MemoryService
// (no network) with per-request latency D, issue spacing I and K credits. Request n
// comes from source n mod P (cyclic order is preserved), and admissions satisfy
//   s_n = max(a_n, s_(n-1) + I, s_(n-K) + D),   a_n = Z (n < P),  a_n = s_(n-P) + D + Z,
// with terms whose index is negative dropped. Returns s_0 .. s_(count-1).
std::vector<U> k3a_admissions(U sources, U compute, U latency, U spacing, U credits, U count);
// Exact rational num/den in lowest terms.
struct Rational {
    U num, den;
    bool operator==(const Rational&) const = default;
};
// Cycle time lambda = max(I, D/K, (Z + D)/P): the maximum cycle mean of the max-plus
// recurrence above. Throughput X = 1/lambda = min(1/I, K/D, P/(Z + D)) is attained.
Rational k3a_cycle_time(U sources, U compute, U latency, U spacing, U credits);

// --- N2: resource demand and bounds --------------------------------------------------
// Directed links on the XY route, as link indices tile*5 + port (ports: local 0, -x 1,
// +x 2, -y 3, +y 4). Stepped per dimension here, independently of noc::Topology::route.
std::vector<U> xy_links(const TopologyConfig&, U from_tile, U to_tile);
enum class ReadClass { l1_hit, l2_hit, l2_miss };
struct ClassifiedRead {
    std::size_t core;   // Index into ClusterConfig::cores.
    U bank;             // Home bank of the read's line (unused for L1 hits).
    ReadClass kind;
};
// Work each read places on each resource, independent of timing:
//   L1 hit:  nothing outside the core.
//   L2 hit:  REQ (1 flit) core->bank, RESP (F_L1 flits) bank->core; bank busy lookup_L2.
//   L2 miss: the L2-hit packets plus MREQ (1 flit) bank->MC and MRESP (F_L2) MC->bank;
//            bank busy >= lookup_L2 + T0(MREQ) + D + T0(MRESP) + fill_L2 (blocking bank,
//            BK1); one memory request.
// Packet and flit counts are exact; bank busy and core windows are lower bounds,
// reached only when nothing waits.
struct Demand {
    std::vector<U> link_packets, link_flits;                 // By link index.
    std::vector<U> bank_references, bank_misses, bank_min_busy;
    U memory_requests{};
    std::vector<U> core_l1_hits, core_l1_misses;
    std::vector<U> core_min_finish;  // start + sum over reads of (compute + zero-load latency).
};
Demand resource_demand(const ClusterConfig&, const std::vector<ClassifiedRead>&);
// Makespan T (last completion) is at least each of:
//   core:   core_min_finish_p                       (a blocking core never beats zero load)
//   link:   flits_l * t_l                           (one flit per t_l per link)
//   bank:   bank_min_busy_b                         (one request at a time)
//   memory: (N-1) I + D  and  ceil(N/K) D           (issue spacing; K credits)
// This is the finite-run form of X <= min(P/(Z + R0), 1/Dmax).
struct MakespanBound {
    U value;            // Max of the terms below.
    U core, link, bank, memory;
};
MakespanBound makespan_lower_bound(const ClusterConfig&, const Demand&);
// Uniform all-to-all on a k x k mesh (every tile sends one packet to every tile) under
// XY routing. The +x link from column x in row y carries every packet whose source is
// in row y at column <= x and whose destination column is > x (any of k rows), since X
// moves happen in the source row: k (x+1)(k-1-x) packets. Y moves happen in the
// destination column, giving k (y+1)(k-1-y) on the +y link from row y. -x/-y mirror
// these. The maximum is k floor(k^2/4) (k^3/4 even, k(k^2-1)/4 odd), at the bisection.
U mesh_all_to_all_max_channel_load(U k);
}
