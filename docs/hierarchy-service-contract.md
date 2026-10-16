# Configurable cache hierarchy and memory service: contract v1

Implemented following the user's 29 September 2026 request for configurable CH and
memory models, a frontend-free directed testbench, careful research interpretation,
and explanatory code comments. These are **synthetic, unvalidated hardware models**.
The previous FC source/binary evidence boundary remains unchanged.

The [SVG visual study guide](visual-guide.md) walks through the implementation in
nine diagrams, including [cache request flow](diagrams/02-cache-hierarchy.svg),
[memory admission](diagrams/04-memory-service.svg),
[worked timing](diagrams/05-timing-examples.svg) and
[analytical acceleration](diagrams/06-analytical-acceleration.svg).

## Research selection: precision comes from matching assumptions

There is no single most detailed paper that simultaneously supplies set-associative
replacement, cache hierarchy composition, MCU flash service and CPU dependencies.
The selection below uses exact finite-system mathematics where the contract is
known. Greater paper complexity would not identify undocumented target constants.

| Work inspected | What it establishes | What this implementation uses / does not claim |
|---|---|---|
| [HayStack, PLDI 2019](https://htor.inf.ethz.ch/publications/img/gysi-haystack.pdf), sections 2–3 | Symbolic reuse-distance and miss-threshold counting; fully associative LRU abstraction | Retain exact reuse reasoning. Existing set-aware module extends the mapping contract. No import of its integer-set solver, write policy or timing model |
| [Falcon, PLDI 2024](https://grosser.science/static/aae77ad842c423d9b3fbe5e7337ae38d/pitchanathan-2024-falcon-a-scalable-analytical-cache-model.pdf), sections 2–4 | Affine reuse-source/threshold decomposition; fully associative hierarchies; conflict misses excluded | Research direction for future general affine FC counting. This code is **not** a Falcon reproduction or proof of its published performance |
| [Le Boudec and Thiran, Network Calculus](https://leboudec.github.io/netcal/latex/netCalBook.pdf), section 3.2 and chapter 4 | Max-plus algebra and deterministic system composition | Mathematical framework for explicit resource-readiness recurrences. The credit/beat model and closed form below are project derivations, not equations attributed to this book |
| [Mandal et al., priority-aware NoC model](https://arxiv.org/abs/1908.02408) | Priority-aware models derived from microarchitecture and input traffic | Do not apply a stochastic priority/NoC formula to our single FIFO service path. No NoC artifact was reproduced |
| [Roofline](https://www2.eecs.berkeley.edu/Pubs/TechRpts/2008/EECS-2008-134.pdf) | Compute/traffic performance ceilings | Existing conditional bound API remains separate from request-completion timing |

This is deliberately an exact, explicit **subset** of possible hierarchy behavior.
The finite hierarchy path is a labeled access-level fallback, justified because
ordered downstream misses and inclusion invalidations require state not present in
a miss histogram. Analytical acceleration is used only on supported regular streams
or proven repeated-state blocks. No default per-cycle simulator or latency regression
is introduced. The independent test oracle does step cycles for tiny cases.

## Interfaces and source organization

- `config.hpp` / `config.cpp`: typed configurations, strict text parser, semantic
  validation and deterministic effective-configuration/provenance serialization.
- `service.hpp` / `service.cpp`: request-level FIFO service recurrence and cold
  periodic closed form, including outgoing issue/credit state.
- `hierarchy.hpp` / `hierarchy.cpp`: ordered miss routing, read-only cache state,
  inclusion invalidation, per-level counters and translated boundary state.
- `testbench.hpp` / `testbench.cpp`: direct injection at any configured cache or
  memory; generated/CSV stimulus; counters, latency, throughput and final states.
- `examples/edge_bench.cpp`: small command-line adapter; no instruction frontend.
- `tests/hierarchy_tests.cpp`: independent timestamp-based cache residency and
  discrete-tick memory oracle, golden cases, parser and acceleration checks.

All addresses, widths, counters and ticks use unsigned 64-bit integers. Arithmetic
overflow is an error, not wraparound. Reported means/bytes-per-second use floating
arithmetic; exact counts and completion ticks do not. Time conversion uses the one
explicit `clock_hz`. Multiple clock domains are unsupported.

## Cache hierarchy mathematics

Geometry remains `line=floor(address/L)`, `set=line mod S`, W ways per set, LRU.
Every hit moves a line to MRU. Every miss fetches a complete line and installs it
at MRU after completion; replacement removes LRU only when no invalid slot exists.
Crossing reads are split into ascending line references and processed serially.
Levels are listed upper to lower. Lower line sizes must be integer multiples of
upper sizes; a full upper line then fits in one lower line. Lower fill traffic uses
the lower line's size, not the upper request's width.

For one line at level l, after any top-level admission wait:

```
lookup_done = arrival + lookup_cycles[l]
hit:  completion = lookup_done
miss: completion = lower(full_line, lookup_done) + fill_cycles[l]
```

For top-level requests, `start_i=max(offered_arrival_i, previous_completion)`.
All requests to a cache path are blocking, including hits. `fill_cycles` is an
explicit post-return installation/upper-return cost; there is no extra implicit
bus bandwidth, hit transfer or miss penalty. It must be supplied consistently for
the modeled request widths. It is not inferred from the MCU's five flash wait states.

`inclusive`: lower eviction invalidates every upper line in its address span,
preserving the order of survivors. These count as invalidations separately from
local LRU evictions. Initial upper contents must exist in every lower level.
Upper hits do not refresh the lower LRU. Direct injection at L2 may invalidate L1.
Invalidations are atomic at the lower replacement point and have no additional
charged latency in this contract; a snoop/invalidation network would need its own
service resources.

`noninclusive`: allocation still populates the requested path, but lower eviction
does not invalidate upper copies. This means neither inclusion nor exclusion is
enforced; it does not mean exclusive/victim-cache placement.

No write data, dirty state, coherence, writeback, prefetch, critical-word-first
return, MSHR merging, hit-under-miss or cache port contention is modeled. The strict
schema rejects alternative policy values. A single injection entry is allowed per
run; mixed bypass/cache traffic would need a specified arbiter.

## Memory mathematics: latency, throughput and credits

The model is a single FIFO admission stream with fixed first-beat latency, serial
beat occupancy and K in-flight credits. Waiting offered requests have an unbounded
external queue. Outstanding credits are not queue depth; queue overflow/drop/backoff
is not implemented. Requests are reads and complete atomically at their last beat.

For request i at address A_i, width n_i, offered at a_i, let B be `beat_bytes`,
tau be `beat_cycles`, L be `first_beat_cycles`, and J0 be `issue_cycles`:

```
b_i = ceil(((A_i mod B) + n_i)/B)
D_i = L + (b_i-1)*tau                  # admission to last beat, cycles
I_i = max(J0, b_i*tau)                 # next admission spacing, cycles
wire_bytes_i = b_i*B
```

Partially used boundary beats still consume full beats. The memory API separates
requested bytes from wire bytes. Internal command/address bus bytes and DRAM/flash
bank commands are not counted. Fixed L separates response latency from initiation
occupancy; issue readiness may remain after completion when I_i>D_i.

Admission rule: start at `max(a_i, next_issue)`, release all completions at or before
that tick; if all K credits remain occupied, wait for the earliest completion.
Then `d_i=start_i+D_i` and `next_issue=start_i+I_i`. Completion at tick t releases
its credit before admission at t. Equal offered arrivals retain input order.
The response transfers cannot overlap: the previous request reserves at least its
beat count times tau before the next admission, and L is common to all requests.

This yields exact times under these specified rules. It is not a statistical mean
delay formula. Independent memory traffic can pipeline; the blocking cache path
usually exposes less parallelism. Changing K can therefore affect the direct-memory
loaded test without changing a serial cache-miss stream.

### Closed form for a periodic identical request

For cold state, `a_i=a0+i*P`, equal address/width, N requests, zero-based i, constant
D and I, FIFO completion order follows from equal D:

```
s_i = max(a_i, s_(i-1)+I, s_(i-K)+D)  # omit unavailable predecessors
J = max(P,I)
H = max(K*J,D)
s_i = a0 + floor(i/K)*H + (i mod K)*J
d_i = s_i+D
```

Proof by induction: inside a group of K requests, the preceding admission supplies
the J spacing unless the offered-arrival term is equal/larger. At the next group,
the earliest reused credit requires D since that group's first admission, while
port/arrival spacing requires K*J. Their maximum is H. The same two constraints
recur each group. This also proves monotone completion order. The implementation
tests every intermediate start/completion indirectly through the independent oracle
and compares aggregate timing and outgoing state with finite evaluation.

The first response latency is D. Last latency is `d_(N-1)-a_(N-1)`. To calculate the
mean without replay, write `N=q*K+r`. Then:

```
sum floor(i/K) = K*q*(q-1)/2 + r*q
sum (i mod K) = q*K*(K-1)/2 + r*(r-1)/2
mean_latency = D + [sum_floor*(H-K*P) + sum_rem*(J-P)]/N
```

Zero requests have zero summary counts. The outgoing state retains next issue time
and only completions still pending at the last admission, plus the last offered
arrival for FIFO order checks on continuation. Constructing it costs
O(min(N,K)); counts and timing use O(1) arithmetic. No billion-element vector exists.

In a saturated stream the long-run useful-byte rate is
`request_bytes * min(1/I, K/D)` bytes/cycle. Finite reported bandwidth instead uses
`total_requested_bytes / (last_completion-first_arrival)` and therefore includes
startup/drain. Overloaded open-loop traffic has growing queueing latency; it is not
reported as a stable steady-state latency. Closed-loop stimulus uses the previous
completion plus configured think time to derive the next offered arrival.

## Exact repeated-block acceleration for the hierarchy

For `stable_blocks`, repeat the address block generated by `address_period` in
closed-loop mode. At boundaries compare:

1. Every resident line and its MRU-to-LRU order at every set and level.
2. Memory issue readiness relative to the next offered arrival, clamped at zero
   when already ready. No outstanding completion may outlive a drained boundary.
3. The same immutable configuration and subsequent stimulus block.

If one complete block returns to exactly the same normalized state, determinism
and time-translation invariance prove that every subsequent identical block has
the same counter increment, per-request latency and duration. Multiply these deltas
and translate readiness instead of replaying. Cold and transition blocks remain
explicit. Misses need not be zero: a conflict-thrashing block can also be periodic.
Multi-block cycles that do not return after one block are not accelerated.

This is a project-specific fixed-point proof, not Falcon's symbolic affine solver.
If the proof fails before the configured evaluation budget, the test fails explicitly.
Open-loop queue growth, multiple concurrent sources and arbitrary timestamp traces
are not eligible. Final readiness after a skip is canonicalized at the boundary;
historical ready times in the past are equivalent to zero residual readiness.

## Assumption and provenance ledger

| ID | Assumption / source | Why / affected outputs | Alternatives and status |
|---|---|---|---|
| CH1 | Modulo, per-set LRU; accepted synthetic contract | Deterministic conflicts and exact state | Hashed indexing, FIFO, pseudo-LRU unsupported; not verified ART |
| CH2 | Full-line, demand-only reads | Defines lower traffic and allocation | Sub-line valid bits/prefetch/writes unsupported |
| CH3 | Blocking cache path; project restriction | Makes completion composition explicit | Nonblocking MSHRs require a separate dependency/state model |
| CH4 | Inclusive or noninclusive, nested line sizes | Defines eviction effects and routing | Exclusive/victim, arbitrary size ratios unsupported |
| CH5 | Serial lookups and caller-specified install cost | Defines hit/miss response boundaries | Overlapped lookup, return bus arbitration not included |
| M1 | One FIFO, fixed first-beat latency | Exact finite admission recurrence | Banks, row hits, refresh, priorities, arbitration unsupported |
| M2 | Finite in-flight credits, unlimited offered queue | Separates pipelining from queueing | Finite queue rejection/backpressure would change arrivals |
| M3 | One clock, complete beat occupancy | Exact transfer/issue demand | Clock crossing, burst limits, interleaved return buses unsupported |
| A1 | Only proven normalized-state repeats are skipped | Exact supported state transition | No extrapolation from similar miss rates |
| P1 | All checked-in numbers are synthetic teaching inputs | Reproducible directed tests without a board | No STM32 service ceiling/latency claim; hardware validation pending |
| P2 | Whole-module provenance string required | Every timing/geometry choice has an explicit source label | For real profiles, cite each knob/value/condition within this field or a linked ledger |

The L4R5 flash-path ART caches are not a generic L1/L2/SRAM hierarchy. These synthetic
levels are reusable model/test infrastructure, not an assertion about the selected
board. Board-specific routing, separate instruction/data interactions, bank mode,
sub-line buffers, prefetch and shared flash arbitration still require derived rules
and sensitivity analysis. No neural-network timing coefficients have been fitted.

## Testbench and measurements under the abstraction

See [configuration reference](configuration.md) for syntax, knobs and runnable commands.
Reports include classification, evaluated/skipped requests, useful bytes, completion
ticks, min/mean/max latency, drain-inclusive bandwidth, per-level hit/miss/eviction/
invalidation/fill counters, requested/wire memory traffic, outgoing state and effective
configuration/provenance. CSV stimulus injects explicit reads without the CPU frontend.
Every printed request row was actually evaluated; skipped rows are not fabricated.

Golden synthetic examples:

- 32-byte aligned memory read: 4 beats, D=12+3*2=18 cycles, I=max(2,4*2)=8 cycles.
- Four such requests spaced 100 cycles apart each have 18-cycle latency.
- Saturated K=4 permits one request per 8 cycles after startup; N requests drain at
  `18+(N-1)*8`. K=1 instead serializes them at 18 cycles/request.
- Cold L1 read in the supplied hierarchy takes `1+3+18+2+1=25` cycles. Repeated
  L1 hit takes 1 cycle. A different L1 line in an already resident L2 line takes
  `1+3+1=5` cycles. These numbers follow the synthetic contract only.

The tests independently check complete states and traffic, not just elapsed time.
After an exception during hierarchy execution, discard that model instance: a
budget/overflow error may occur after earlier transactions changed its state.
Configuration parsing is completed before any stimulus executes.

## Verification record (29 September 2026)

GCC 13 in WSL Ubuntu, C++20: all seven CTest groups passed in Debug with AddressSanitizer
and UndefinedBehaviorSanitizer, and in optimized Release. The final hierarchy test
also passed in both builds after adding explicit prefix/continuation checks.

- 324 periodic parameter combinations, each checked at every prefix of 19 requests,
  with exact completion ticks, outgoing credit state and snapshot continuation.
- 120 variable-width/alignment memory streams against discrete-tick service.
- 120 hierarchy streams against independent timestamp-based LRU, checking timing,
  traffic, inclusion invalidations and final contents after each request.
- 24 accelerated/finite repeated-block comparisons, including memory readiness
  carried into a future request, plus billion-request examples.
- Golden cold/L1-hit/L2-hit and credit/bandwidth cases; strict config/CSV rejection,
  overflow, empty input, unsupported policy and finite-work-budget cases.
- Existing FC and memory algebra tests remain passing. No board execution occurred.

Example reports are reproducible with the commands in the config reference. The
local Release reports are `build-release/hierarchy-report.txt`, `latency-report.txt`
and `bandwidth-report.txt` (generated build artifacts, not committed source).
The hierarchy report gives 1,000,000,000 requests, 8 evaluated requests, 4 L1 misses,
2 L2 misses, 64 memory wire bytes and 1,000,000,056 completion cycles. The unloaded
memory profile gives 18 cycles for each request. The saturated billion-request
memory profile completes at 8,000,000,010 cycles and reports 399,999,999.5 requested
bytes/s at the hypothetical 100 MHz clock. Its queueing latency grows because all
requests were offered at tick zero; this is intentional, not steady-state latency.

Builds from the Windows-mounted checkout emitted its existing timestamp-skew
warnings. Changed targets were compiled and linked in each configuration, and
tests executed successfully. These checks establish the specified abstractions,
not hardware accuracy or the behavior of the complete published research artifacts.
