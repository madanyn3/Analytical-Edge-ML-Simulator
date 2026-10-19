# Phase K design: tiled multi-core with banked shared L2, on-chip network and memory

Revision 2, 16 October 2026. **Design for review; no code yet.** This revision replaces
revision 1's single arbiter. The user specified:

- tiles in a mesh or torus, each holding a core, a private L1 and one L2 bank;
- line-interleaved bank mapping;
- one memory controller (MC);
- link contention modeled from the start;
- 2×2 (4 cores) first, then 3×3 and 4×4.

Everything here is a **synthetic model**. The NUCLEO-L4R5ZI is single-core, so nothing in
this phase is a hardware claim. Version-1 configurations, the single-stream `Hierarchy`,
and their tests keep their current behavior.

## 1. Architecture

```text
  k×k grid (k = 2, 3, 4), tile i = (x, y), i = y·k + x

  ┌──────────── tile i ────────────┐
  │ core i ─► L1 i (private)       │
  │ L2 bank i (shared slice)       │◄──► router i ◄──► links to ±X, ±Y neighbors
  │ [MC, on exactly one tile m]    │     (torus: wraparound links)
  └────────────────────────────────┘
```

| Component | Model |
|---|---|
| Core | Blocking request source, one outstanding read (§2) |
| L1 | Private per core, identical configuration, existing LRU/line rules |
| L2 | `B = k²` banks, one per tile; per-bank geometry `S` sets × `W` ways, line `L₂` |
| Bank mapping | `line = ⌊A/L₂⌋`, `bank = line mod B`, `set = ⌊line/B⌋ mod S`, `tag = ⌊line/(B·S)⌋`. Bank and set fields don't overlap, so every set of every bank is usable |
| Network | Directed links between neighboring routers; dimension-order XY routing; packets of flits; link contention (§4) |
| Memory | Existing `MemoryService` (FIFO, first-beat latency, beats, `K` credits) attached to tile `m` |

The L1 line size must divide `L₂`, as in the version-1 nesting rule.

### Evaluation strategy

The exact evaluator is **event-driven**. It advances from one event (injection, link
grant, delivery, bank or memory grant, completion) to the next. It never ticks idle
cycles, and it is deterministic under the tie rules in §5. An independent tick-stepped
oracle exists only in tests (§8).

**The evaluator is the exact reference, not the analytical contribution.** The analytical
results are in §6. Each one is either proven or measured against this evaluator.

## 2. Core model

Core `p` runs segments `(c_k, A_k, n_k)`: `c_k` cycles of compute, then a read of `n_k`
bytes at address `A_k`. With at most one outstanding read:

```text
a_(p,0)   = start_p + c_0
a_(p,k+1) = d_(p,k) + c_(k+1)
```

Here `a` is the time the core issues the read and `d` is when it completes.

- **Generated program:** `c_k = compute_cycles`, `n_k = bytes`, and
  `A_k = address + (k mod address_period)·stride`, for `count` segments.
- **Explicit program:** a CSV trace (§7.3).
- **Feedback:** this closed loop is the feedback research-plan §20.4 requires. A slower
  network or memory delays the cores' next requests, and the evaluator reproduces that
  exactly.
- **Deferred:** more than one outstanding read per core, and segment streams produced by
  the Phase D instruction pipeline.

## 3. Transaction flow

A read is split into L1 line references using the existing rules. An **L1 hit** completes
locally after `lookup_cycles`. An **L1 miss** on line ℓ by core `c`, with home bank
`h = bank(ℓ)`, goes through these steps:

```text
1. REQ   c → h        1 flit                         (skipped if h = c)
2. bank h: arbitration, lookup_cycles
   hit:  go to 5
   miss: 3. MREQ  h → m     1 flit                    (skipped if h = m)
         4. memory service (one full L₂ line), MRESP m → h,
            1 + ⌈L₂/flit_bytes⌉ flits (skipped if h = m); then L2 fill_cycles
5. RESP  h → c        1 + ⌈L₁/flit_bytes⌉ flits      (skipped if h = c)
6. L1 fill_cycles; the line is installed in L1 at MRU; the read continues
```

- **BK1: banks are blocking.** A bank is busy from its grant until it injects the
  response in step 5. On an L2 miss, the bank stays busy during the whole memory round
  trip. This matches version 1's blocking caches. Non-blocking banks with miss status
  holding registers (MSHRs) are later work.
- **BK2: local traffic doesn't use the network.** When source and destination are the same
  tile, there is zero network latency; local injection and ejection ports are not
  contended resources.
- **BK3: L2 is noninclusive with respect to L1.** An L2 eviction does not invalidate any
  L1 copy. This keeps each core's L1 state independent of other cores.
- **BK4: read-only, no coherence.**

## 4. Network model (exact, with link contention)

### 4.1 Topology and routing

- **Mesh:** links between horizontally and vertically adjacent tiles.
- **Torus:** a mesh plus wraparound links; requires `k ≥ 3`. With `k = 2`, the wraparound
  link would duplicate the existing neighbor link.
- **Routing:** dimension-order XY. Correct X first, then Y.
- **Torus direction:** in each dimension, take the shorter direction around the ring. When
  both directions are equally long (distance `k/2`), take the positive direction.
- **Deadlock:** routes are deterministic, and buffers are unbounded (NW2), so deadlock
  can't occur in this abstraction.

**Hop count `H(s, t)`:**
- mesh: `|x_s − x_t| + |y_s − y_t|`;
- torus: `ring(x_s, x_t) + ring(y_s, y_t)`, where `ring(a, b) = min(|a − b|, k − |a − b|)`.

### 4.2 Packet timing

Parameters: `t_r` = router cycles per hop, `t_l` = link cycles per flit (`t_l ≥ 1`),
and `F` = flits per packet.

A packet's path is a sequence of directed links `e₁ … e_H`. Let `s_j` be the time its head
is granted link `e_j`.

```text
ready_1     = inject + t_r                  (source router)
s_j         = grant time at e_j  ≥  ready_j  (§4.3)
link e_j busy on [s_j, s_j + F·t_l)
ready_(j+1) = s_j + t_l + t_r
delivered   = s_H + F·t_l                   (whole packet at destination)
```

**Zero-load latency** (no contention): `T₀(H, F) = H·(t_r + t_l) + (F − 1)·t_l` for
`H ≥ 1`, and `0` for `H = 0`.

- **NW1: virtual cut-through flow control.** The head may enter the next link before the
  tail has left the current one. Flits stream at one flit per `t_l`.
- **NW2: unbounded router buffers.** A packet that is blocked waits entirely buffered.
  There is no backpressure upstream. Finite buffers and wormhole backpressure would need
  credit state per link and are later work.
- **NW3: one virtual channel; links are full-duplex.** Each direction is a separate link.

### 4.3 Link arbitration

At each directed link `e`:

```text
t = max(free_e, min over waiting heads of ready)
E = { packets waiting at e with ready ≤ t }
winner = POLICY(E);  s = t;  free_e = t + F_winner · t_l
```

| `link_arbitration` | Winner |
|---|---|
| `fifo` | Smallest `ready`. Ties go by input port in the fixed order `local, −X, +X, −Y, +Y`, then by packet class |
| `round_robin` | Per-link pointer over input ports in that same order. The first port, cyclically from the pointer, with an eligible packet wins; within a port, FIFO. Pointer ← winner's port + 1 |

**Packet-class tie rule** (added after the oracle sweep found an id-dependent tie):
- **Order:** memory-controller responses, then bank responses, then bank memory
  requests, then core requests.
- **Why only the local port needs it:** packets on a non-local port can't arrive at the
  same tick, because their upstream link served them one at a time.
- **Why the order is complete:** each tile injects at most one packet per class per tick.
  A core has one outstanding read, a bank serves one request at a time, and the MC's
  completions are distinct because every request is a same-size, aligned L2 line.
- **Consequence:** packet ids never decide arbitration. The test oracle breaks any
  remaining tie by reverse id, and it still agrees with the evaluator.

### 4.4 Bank and memory-controller arbitration

- **Banks:** each bank is a blocking server. Its grant time is
  `t = max(bank_free, earliest waiting delivered request)`, and among eligible requests
  the policy (`fifo`, `round_robin` by source tile, or `fixed_priority` by tile index)
  picks the winner. This is revision 1's rule, applied per bank.
- **Memory controller:** the existing earliest-admission rule:
  `f = next_issue` if fewer than `K` credits are outstanding; otherwise
  `f = max(next_issue, earliest pending completion)`. FIFO by delivery time, ties broken
  by bank index. Requests are submitted at the nondecreasing grant time `t`, so the
  service's arrival-order check still holds. The true delivery time is kept for
  statistics.

## 5. Event semantics and exactness

**Simultaneous events.** Events at the same tick `t` are processed in fixed phases:

1. Completions and deliveries: memory completions release credits, delivered packets
   become waiting requests, L1 fills finish.
2. Core issues and L1 lookups that produce injections.
3. Grants, in fixed resource order: memory controller, banks by index, then links by
   (router index, output port).

Every grant schedules its consequences strictly later, because `t_l ≥ 1`,
`lookup_cycles ≥ 1` and `first_beat_cycles ≥ 1`. So the phase order defines a unique
deterministic execution. The tick oracle implements the same phases independently.

**Why the evaluator is exact without ticking.** Between consecutive event times, no
resource changes state. The next event time is the minimum over pending deliveries,
completions, compute ends, and earliest grant times. Each of these is known from current
state, because every granted operation's end time is computed at its grant.

**Exact acceleration (periodic state).** At each event time, form a **normalized key**
containing:
- all queues and residual busy times relative to `t`;
- every cache's contents in MRU-to-LRU order;
- round-robin pointers;
- memory readiness and credits;
- each core's program position modulo its address period.

If a key repeats after `c` grants with elapsed time `Δ`, determinism and translation
invariance prove exact repetition. The evaluator then skips whole periods and finishes
the remainder explicitly. This is the version-1 `stable_blocks` proof generalized. If no
repeat is found within budget, the run fails explicitly.

## 6. Analytical results to prove and verify

The **must-have** items are the ones the December snapshot needs.

### N1. Zero-load latency closed forms (must-have)

- **L1 hit:** `lookup_L1`.
- **L2 hit, core `c` to bank `h`:**
  `lookup_L1 + T₀(H(c,h), 1) + lookup_L2 + T₀(H(h,c), F_L1) + fill_L1`.
- **L2 miss:** the hit path, plus `T₀(H(h,m), 1)`, the memory latency `D`,
  `T₀(H(m,h), F_L2)` and `fill_L2`.

**Average under uniform line distribution.** Line interleaving sends a uniformly random
line to each bank with probability `1/B`. Averaged over all cores and banks, mean hop
counts are:

| Topology | Mean `H` (k×k, source and destination uniform, self included) | k=2 | k=3 | k=4 |
|---|---|---|---|---|
| Mesh | `2(k² − 1)/(3k)` | 1 | 16/9 | 5/2 |
| Torus (k ≥ 3) | `k/2` (even k), `(k² − 1)/(2k)` (odd k) | — | 4/3 | 2 |

These were checked by exact enumeration for k = 2–8. Per-core averages differ by
position: corner tiles are farther from the average bank. Report them per core, and
prove the per-tile formula `(1/k)·Σ_x' |x − x'|` per dimension.

**Strided access streams.** For a strided access stream, the sequence of banks visited is
`(ℓ₀ + i·σ) mod B` for line stride `σ`. It touches `B / gcd(σ, B)` distinct banks. This
is a resonance condition analogous to Phase A's set conflicts: for example, with
`σ = B`, every access goes to the same bank. State it and prove it.

**Checked so far (N1).** `analytic::zero_load_read` and `analytic::l2_hit_sum_over_banks`
(`src/cluster_analytic.cpp`) are pure arithmetic on the configuration. In
`tests/cluster_theorem_tests.cpp` they match the evaluator on every read of 324 random
single-core runs (25,476 reads; 2×2–4×4 meshes, 3×3/4×4 tori, every core tile). A lone
blocking core never meets contention. The per-tile hop sum `k(S(x) + S(y))` is checked
for k = 2–8, with `S(a) = a(a+1)/2 + (k−1−a)(k−a)/2` on a mesh and `⌊k²/4⌋` on a torus.
The bank-sum formula has a `(B−1)(F_L1−1)t_l` term rather than `B(...)`, because T₀ is
not affine in H: a local bank costs no network time at all.

### N2. Resource-demand bounds for the closed system (must-have)

Each request class (L1 hit, L2 hit at bank `h`, L2 miss) places a known demand on each
resource: links on its XY path (`F·t_l` per packet), its bank (lookup, or the whole miss
round trip under BK1), and memory (issue spacing `I`). Given each core's per-request mix,
let `Dmax` be the largest total demand on any single resource per round of requests, and
`R₀` the zero-load round-trip time. Then:

```text
X ≤ min( P / (Z + R₀),  1 / Dmax )
```

This is a direct generalization of the asymptotic bounds.

**Channel load under XY routing on a k×k mesh.** The maximum channel load for uniform
traffic is a known closed form; for even k the bisection channels are the bottleneck.
Derive it for this request/response packet mix, then check the bound and where it is
tight against the evaluator.

**Checked so far (N2).** The check uses the finite-run form of the bound. Makespan T is at
least the largest of:
- each core's `start + Σ(Z + R₀)`;
- each link's `flits · t_l`;
- each bank's minimum busy time (lookup per hit, the whole zero-load miss round trip
  per miss under BK1);
- memory's `(N−1)I + D` and `⌈N/K⌉D`.

The test streams are built so that each read's class (L1 hit, L2 hit or L2 miss) follows
from the construction for any interleaving. Under that construction the predicted
per-link packet and flit counts, bank references and misses, memory requests and L1
counts equal the evaluator's exactly in 150 random clusters. Every bound holds.

The all-to-all mesh channel load `k⌊k²/4⌋` (bisection links) is checked by enumeration
for k = 2–8.

Observed tightness (makespan / bound, by binding term) in the current sweep:

| Binding term | Clusters | Mean makespan / bound |
|---|---|---|
| Core zero-load | 86 | 1.24 |
| Link | 12 | 1.60 |
| Bank (stride σ = B resonance onto one bank) | 46 | 1.02 |
| Memory | 6 | 1.28 |

These are observations from a random sweep, not theorems. A plausible reading, not yet
analysed: the resonance bank is one blocking resource that serializes every request, so
its bound is nearly tight. The busiest link is idle whenever its traffic is held up at
banks, other links or compute, so its bound is loose.

### N3. Memory theorem K3a (must-have; unchanged from revision 1)

**Setting.**
- P sources with no network attached directly to a `MemoryService` (`D`, `I`, `K`).
- Each source has constant compute `Z`.
- Policy FIFO, or round-robin with pointer 0.

**Result.** Admissions satisfy

```text
s_n = max(a_n, s_(n−1) + I, s_(n−K) + D)
a_n = Z                      (n < P)
a_n = s_(n−P) + D + Z        (n ≥ P)
```

with eventual slope `λ = max(I, D/K, (Z+D)/P)`. So `X = min(1/I, K/D, P/(Z+D))` is
attained exactly. The proof sketch is in revision 1's argument: cyclic order is
preserved, completions are FIFO, and the max-plus cycle mean is `max_j w_j/j`.

**Checked so far.** `tests/cluster_theorem_tests.cpp` drives the production
`MemoryService` with P closed-loop sources and compares every admission with the
recurrence (`analytic::k3a_admissions`). It covers 3,000 random configurations
(P ≤ 8, Z ≤ 80) of 600 admissions each. Every admission matches, and the sources stay
in cyclic order. On the last 300 admissions some period q satisfies
`s_(n+q) − s_n = qλ` with λ from `analytic::k3a_cycle_time`. The mean response time
over one period is exactly `Pλ − Z`. Each of the three terms of λ binds in some
cases. One worked example: `P=4, Z=10, D=12, I=2, K=2` gives `λ = 6`, mean latency 14,
and admissions `10, 12, 22, 24, 34, 36, …` (period 2, gaps 2 and 10). These are
checks of a stated theorem, not a proof; the proof goes in the theorem write-up.

**Open item: transient length.** The longest transient observed is 30 admissions
(P ≤ 8). Bounding the transient is a research item. Periodic-state acceleration
doesn't depend on it.

**How it is tested.** Directly, with several closed-loop sources on `MemoryService`, as a
component-level directed test. It also applies to a whole-tile system whenever every
read misses in L2 and the network and banks are not on the critical cycle. Which resource
dominates is decided by checking N2's `Dmax` against memory's own bound, not assumed.

### N4. Approximation study (stretch)

Compare against the exact evaluator:

- per-link queueing approximations (M/D/1 per channel, and the priority-aware
  decomposition of Mandal et al., research-plan §20.2);
- MVA, with banks, the most-loaded links and memory as queueing stations and compute as
  a delay station.

Report the error against utilization and k. The expected but unverified outcome is that
the approximations hold at low load and break near saturation and under strided bank
resonance.

### N5. Fixed-priority fairness (experiment)

Measure starvation and the per-core throughput ratio under `fixed_priority` banks. No
closed form is claimed.

## 7. Configuration version 2

Version-1 files are unchanged. Version 2 uses the same strict lexical rules: required
keys, no hidden defaults, and provenance on every physical section.

### 7.1 Sections

| Section | Keys |
|---|---|
| `[model]` | `version=2`, `clock_hz`, `access=read_only`, `prefetch=none`, `cache_concurrency=blocking`, `coherence=none` |
| `[topology]` | `kind=mesh\|torus`, `k` (2–8; torus requires ≥ 3), `mc_tile`, `router_cycles`, `link_cycles`, `flit_bytes`, `link_arbitration=fifo\|round_robin`, `provenance` |
| `[l1]` | `line_bytes`, `sets`, `ways`, `lookup_cycles`, `fill_cycles`, `initial_lines=empty`, `provenance` (replicated per core) |
| `[l2]` | per-bank `line_bytes`, `sets`, `ways`, `lookup_cycles`, `fill_cycles`, `bank_arbitration=fifo\|round_robin\|fixed_priority`, `initial_lines=empty`, `provenance` |
| `[memory]` | as version 1 (`policy=fifo`, `first_beat_cycles`, `beat_bytes`, `beat_cycles`, `issue_cycles`, `outstanding`, `provenance`) |
| `[core.NAME]` | `tile`, `start_cycles`, `compute_cycles`, `count`, `address`, `bytes`, `stride`, `address_period` |
| `[stimulus]` | `engine=finite\|periodic_state`, `max_requests`, `max_transactions`, `max_events` |

**Constraints.**
- One core per tile at most. Tiles without a core are idle but still host a bank and a
  router.
- `l1.line_bytes` divides `l2.line_bytes`.
- Every round-robin pointer starts at index 0 (port `local`, or tile 0). This is a
  stated rule, not a hidden default. Pointers are included in the reported and compared
  state.
- Explicit `initial_lines` for L1 and L2 are deferred: v2 requires `empty`.
- **Rejected:** a torus with `k = 2`; `mc_tile` outside the grid; duplicate tiles; packet
  flit counts or timings that overflow.

### 7.2 First example: 4 cores on a 2×2 mesh

```ini
# Synthetic 2x2 tiled CMP. No value is a measured hardware parameter.
[model]
version=2
clock_hz=1000000000
access=read_only
prefetch=none
cache_concurrency=blocking
coherence=none

[topology]
kind=mesh
k=2
mc_tile=0
router_cycles=1
link_cycles=1
flit_bytes=16
link_arbitration=round_robin
provenance=synthetic teaching network

[l1]
line_bytes=32
sets=4
ways=2
lookup_cycles=1
fill_cycles=1
initial_lines=empty
provenance=synthetic

[l2]
line_bytes=64
sets=16
ways=4
lookup_cycles=4
fill_cycles=2
bank_arbitration=round_robin
initial_lines=empty
provenance=synthetic

[memory]
policy=fifo
first_beat_cycles=40
beat_bytes=16
beat_cycles=2
issue_cycles=2
outstanding=4
provenance=synthetic

[core.c0]
tile=0
start_cycles=0
compute_cycles=10
count=1000
address=0x0
bytes=4
stride=64
address_period=64
# c1..c3 on tiles 1..3

[stimulus]
engine=periodic_state
max_requests=100000
max_transactions=1000000
max_events=10000000
```

**Hand-checkable numbers.**
- Response flits: L1 response `1 + 32/16 = 3`; memory response `1 + 64/16 = 5`.
- Core 0 to bank 3 (diagonal, `H = 2`), zero load:
  - REQ: `T₀(2, 1) = 2·(1 + 1) = 4` cycles;
  - RESP: `T₀(2, 3) = 4 + 2 = 6` cycles;
  - L2 hit latency: `1 + 4 + 4 + 6 + 1 = 16` cycles.
- These become golden tests.

### 7.3 Explicit programs

`--trace` with header `core,compute_cycles,address,bytes`. Rows for each named core are
in program order; rows for different cores may interleave in the file. When a trace is
given, it defines every core's program.

### 7.4 Reports

- **Per core:** requests, L1 counters, latency (min/mean/max), and a breakdown into network,
  bank-wait, bank-service and memory time.
- **Per bank:** references, hits, misses, evictions, utilization and mean queue wait.
- **Per link:** flits, utilization and mean wait; the most-loaded links are listed
  explicitly.
- **Memory:** requests, wire bytes and utilization.
- **Cluster:** throughput, the fairness ratio, evaluated versus skipped events, the
  detected period, final states, and the effective configuration with provenance.

## 8. Verification plan

**Oracle.** A tick-stepped simulator in `tests/cluster_tests.cpp`, written from this
document's text. It has its own LRU, routing, link occupancy and arbitration code, and
shares nothing with the evaluator. Each tick runs the §5 phases.

**Golden cases on the 2×2 mesh, by hand.**
1. Zero-load L2 hit for each (core, bank) pair, including local hits (`H = 0`).
2. Two packets competing for one link in the same tick, under FIFO tie order and under
   round-robin.
3. Bank conflict: two cores hit the same bank at the same time.
4. L2 miss path to the MC on tile 0, including the case `h = m`.
5. Strided resonance: line stride `σ = B` concentrates every access on one bank, while
   `σ = 1` spreads them.
6. Credit-limited memory: the K3a example values.

**3×3 torus golden cases.** Wraparound routing, the tie rule at distance k/2 (k = 4),
and the mean-hop formula against the evaluator's zero-load average.

**Sweeps.**
- About 500 random small configurations (2×2, 3×3; mesh and torus; all policies; random
  programs), evaluator against oracle. Check every completion time, every counter, final
  cache, memory, pointer and link state, and continuation from a snapshot.
- N1 and N2 formulas against the evaluator; K3a at every prefix; `periodic_state` against
  `finite`.

**Rejections.** Every constraint in §7.1.

**Gate.** Zero mismatches against the oracle, N1 and K3a exact, and N2 bounds never
violated, with their tightness reported.

## 9. Code plan

| Piece | Change |
|---|---|
| `config.*` | Version-2 structs and parser branch; validation |
| `service.*` | `MemoryService::earliest_admission()` accessor |
| `noc.hpp/.cpp` (new) | Topology, XY routing, hop counts, link resources and arbitration |
| `cluster.hpp/.cpp` (new) | Cores, private L1s, banks, MC attachment, event loop, state key, period skip |
| `testbench.cpp`, `edge_bench.cpp` | Version dispatch; version-2 report |
| `tests/noc_tests.cpp`, `tests/cluster_tests.cpp` (new) | Routing/hop and zero-load units; oracle, golden cases, sweeps |
| `configs/cluster-2x2-mesh.ini`, `cluster-3x3-torus.ini`, `cluster-4x4-mesh.ini` | Directed examples |
| `docs/cluster-theorems.md` | N1–N3 proofs and figures |

## 10. Session plan (about one hour each)

1. Review this revision.
2. Version-2 config, parser and rejection tests.
3. `noc`: topology, XY/torus routing, hop counts, N1 mean-hop tests.
4. Link resource and arbitration; packet timing; zero-load golden tests.
5. Cluster event loop, part 1: cores, L1, bank arbitration (L2 hits only).
6. Cluster event loop, part 2: miss path, MC, credits.
7. Tick oracle (network + banks + memory).
8. Golden cases 1–6 on 2×2.
9. Random sweep against the oracle; snapshot continuation.
10. 3×3 torus and 4×4 mesh configurations; resonance and N2 bound checks.
11. Periodic-state acceleration.
12. Reports, CLI, a visual-guide figure.
13–14. Proof write-up: N1, N2, K3a (`cluster-theorems.md`).

This is about 14 sessions, so Phase K now runs until about 6 November. Phase A
(A1–A3, about 6 sessions) follows, then 4–5 sessions of write-up before the 27 November
freeze. The plan has little slack: if sessions slip, cut N4 and A4/A5 first, then 4×4.

## 11. Remaining questions

None are blocking. Defaults are chosen above, and each can be changed during review:

- **Flit size and packet format:** 1 header flit plus a data payload.
- **MC location:** tile 0 by default; configurable.
- **Blocking banks (BK1):** a common first model, but they make the L2-miss cost
  dominate bank occupancy. If you want non-blocking banks before December, that is the
  largest single scope addition.
