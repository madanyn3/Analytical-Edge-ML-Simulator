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
}
