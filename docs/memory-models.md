# I2: cache mathematics and conditional memory bounds

Visual companions: [cache geometry/reuse/sweeps](diagrams/03-cache-mathematics.svg),
[roofline and evidence](diagrams/09-roofline-and-evidence.svg), and the
[full implementation atlas](visual-guide.md).

Implemented 29 September 2026, following authorization to continue without a board.
This is a synthetic read-only cache module and a bound calculator, not a validated
L4R5 timing model. The FC connection uses the existing **source tensor schedule**;
compiled dynamic accesses, instruction fetches and stack traffic remain outside it.

## Research direction

Mattson-style stack analysis is the foundation for LRU miss counting across cache
capacities. It does not automatically remove dependence on trace length. IBM's
[stack-distance paper summary](https://research.ibm.com/publications/efficient-stack-distance-computation-for-priority-replacement-policies)
identifies the original Mattson et al. (1970) method and its per-access work. The
original archived PDF could not be retrieved during this pass; this is not a claim
to have newly reviewed its full text.

[HayStack, Gysi et al., PLDI 2019](https://htor.inf.ethz.ch/publications/img/gysi-haystack.pdf)
goes further: derive reuse distances and count threshold crossings symbolically,
with partial enumeration where needed. Its abstraction is fully associative LRU;
our accepted set-associative contract needs same-set reuse distances.
[Falcon, Pitchanathan et al., PLDI 2024](https://grosser.science/static/aae77ad842c423d9b3fbe5e7337ae38d/pitchanathan-2024-falcon-a-scalable-analytical-cache-model.pdf)
is the next study reference for scalable decomposition. We have not imported or
reproduced either research artifact. A general affine solver is premature for this slice.

[Roofline, Williams, Waterman and Patterson](https://www2.eecs.berkeley.edu/Pubs/TechRpts/2008/EECS-2008-134.pdf)
connects work per transferred byte to bandwidth and compute ceilings. We adapt the
units to integer MACs: one multiply-accumulate counts as one MAC throughout. This
provides necessary performance bounds, not complete short-kernel execution times.

## 1. Geometry and set reuse

For byte address A, access width n, line size L, S sets and W ways:

```
line(A) = floor(A/L)
set(A) = line(A) mod S
lines(A,n) = floor((A+n-1)/L) - floor(A/L) + 1   (n>0)
lines(A,0) = 0
```

For affine addresses substitute `A(i)=A0+i*stride`; mapping is exact, but touched
lines alone do not determine misses. Split a crossing read into ascending line
references. D_set is the number of distinct other lines in the same set since the
previous reference; a reuse hits iff `D_set < W`. For example with S=2,W=2,
line sequence `0,2,1,0` hits on the last access: only line 2 competes with 0.
Sequence `0,2,4,0` misses on the last access even though another set is empty.

`analyze_reuse` retains an unlimited recency stack per set and tests the position
against W. It accepts explicit initial resident lines ordered MRU to LRU, or an
empty state. Missing prior history is unnecessary for hit/miss behavior: lines
outside the initial residents must miss until reloaded. Its final state is the
first W stack entries. It reports references, hits, misses, evictions and fill
bytes; one byte access can generate multiple line references.

This finite analyzer is O(R*(R+C)) in the worst case with vector stacks, where R
is the expanded line-reference count and C the initial resident count. It is a
transparent bounded bridge to the FC schedule, **not symbolic FC acceleration**.
Default expansion limit: 10,000 references; cache-state budget: 1,000,000 lines.
These are software safety limits, not hardware parameters. Writes are rejected;
no write-through/write-back behavior is silently assumed.

## 2. First non-enumerating formula

Consider N consecutive distinct line numbers starting at B, visited in increasing
order, repeated R times from empty cache. For set s define:

```
o_s = (s - (B mod S)) mod S              # least nonnegative residue
k_s = 0                                if o_s >= N
k_s = 1 + floor((N-1-o_s)/S)            otherwise
references = N*R
misses = N + (R-1) * sum_{s: k_s>W} k_s  (N,R>0)
hits = references - misses
resident = sum_s min(k_s,W)
evictions = misses - resident
fill_bytes = L*misses
```

Proof: the first sweep loads every line. Between consecutive uses of one line,
exactly `k_s-1` competing distinct lines appear. Therefore later sweeps all hit
in a fitting set and all miss in an overflowing set. At exit each set contains
its last `min(k_s,W)` lines, in reverse visit order. N=0 or R=0 leaves empty state
and all counts zero. Counts use checked unsigned 64-bit arithmetic.

`analyze_cold_sweep` evaluates this formula and constructs that outgoing state
in O(S + sum min(k_s,W)) time, independent of R. It does not build a trace.
For S=2,W=2,N=5, set populations are 3 and 2. Three sweeps yield 15 references,
11 misses, 4 hits and 7 evictions. A billion sweeps yield 5,000,000,000 references
and 3,000,000,002 misses with the same amount of analysis work. This is a directed
module example; the interleaved FC weight/bias sequence is not this sweep.

## 3. FC placement and traffic

The example routes weights and bias through the synthetic cache. Input and output
remain separate SRAM requests. This follows the target's flash-versus-SRAM distinction
in the [microarchitecture audit](l4r5-microarchitecture-audit.md), but does not
assert ART line size, associativity or complete-line fill behavior. Geometry
`L=8,S=2,W in {1,2,4}` is a teaching sensitivity sweep, not a board configuration.
The 8-byte choice is illustrative; a flash read width alone does not establish
cache allocation/fill granularity.

The example analyzes cold entry and a second call using the exact outgoing state.
Repeated execution need not hit throughout. Fill traffic is `misses*L` only under
the complete-line-fill abstraction. It excludes prefetch, instruction competition,
bus transaction overhead and implementation-specific sub-line fills.

For the existing N=5,M=3 example, there are 10 flash line references per call:

| Ways (S=2,L=8 bytes) | Cold misses | Second-call misses | Cold fill bytes |
|---|---:|---:|---:|
| 1 | 5 | 4 | 40 |
| 2 | 4 | 0 | 32 |
| 4 | 4 | 0 | 32 |

With two ways, F=15 MACs and Q_flash=32 bytes give I_flash=0.46875 MAC/byte.
The SRAM source tensor demand is 10 input bytes plus 3 output bytes. The
hypothetical ceilings below give bounds of 0.15 us compute, 1.6 us flash and
0.13 us SRAM; their maximum is 1.6 us. This is a scenario bound, not board latency.

## 4. Roofline and latency

For F MACs, compute ceiling P MAC/s, boundary traffic Q_r bytes and bandwidth
ceiling B_r bytes/s:

```
I_r = F/Q_r                         # MAC/byte, when Q_r>0
T >= max(F/P, max_r Q_r/B_r)
performance <= min(P, min_r B_r*I_r)
```

Each Q must describe the same resource boundary as its B. Tensor storage size is
not generally Q. Separate flash-fill and SRAM-request bytes; sum competing traffic
before applying a shared bus's bandwidth ceiling. Taking a maximum allows ideal
overlap and therefore yields a necessary lower bound, not evidence that this
overlap is achievable. The largest term identifies the active modeled constraint,
not a proven physical bottleneck. Real instruction/dependency overhead can dominate.

Latency needs an independent dependency model. If a proven critical chain requires
serial costs l_j, `T_dependency=sum_j l_j` supplies another lower bound; take its
maximum with the resource bounds. **Do not use all misses times miss latency**
without proving serialization. Do not add bandwidth time to the same transfer's
latency a second time. `roofline_bound` accepts a caller-derived dependency bound
in seconds; it does not derive that chain. That work belongs to I3.

Missing ceilings stay optional and produce named unknowns. The returned known
lower bound is partial when inputs are missing; zero is not a zero-time prediction.
`complete_roofline` means supplied resource ceilings are complete, not that the
hardware, all traffic or dependencies have been modeled. Nonfinite/nonpositive
ceilings and missing provenance for supplied ceilings are rejected.

The example's 100 million MAC/s, 20 million flash bytes/s and 100 million SRAM
bytes/s are explicitly hypothetical inputs. They are not measured, documented or
calibrated L4R5 parameters. A rigorous physical bound requires valid ceilings and
justified traffic; illustrative scenario values provide conditional bounds only.

## 5. Evidence, assumptions and next increments

| Quantity/rule | Source and status | Consequence / alternative |
|---|---|---|
| FC work and requested bytes | Software-derived pinned source algebra | Binary traffic remains pending |
| Modulo mapping, per-set LRU | Accepted synthetic specification | Different indexing/replacement changes misses |
| Full-line fills, no prefetch, read-only | Accepted synthetic specification | Sub-line fills/prefetch change traffic and state |
| Empty or explicit resident state | Caller-supplied specification | Physical entry procedure still unverified |
| Geometry sensitivity sweep | Directed artificial test | Not confidence intervals or ART configuration |
| Counts and final state | Exact under those abstractions | Not silicon accuracy |
| Roofline scenario ceilings | Hypothetical teaching inputs, units above | Board ceilings unknown; no board time prediction |

Tests use independent byte stepping and bounded timestamp-based LRU eviction,
not the analyzer's unlimited stack or sweep formula. Checks cover alignment,
crossing reads, same-set conflicts, direct mapping, full associativity, initial
states, composition, empty streams, overflow, unsupported writes, FC tails and
roofline units/unknowns. Exhaustive small sweeps test outgoing state as well as counts.

Verification on 29 September 2026: GCC 13 in WSL Ubuntu, Debug with address/undefined
sanitizers and Release both passed both CTest suites. The memory suite checks
13,440 sweep cases, 600 finite trace/composition cases, 24 FC schedules, geometry,
partial initial state and rejection cases. `memory_example` produced the table
above. The mounted Windows checkout produced timestamp-skew build warnings;
the changed source/test targets were compiled and linked in both configurations.
No MCU rebuild or board execution was needed for these host-only additions.

Next: derive symbolic reuse regions for the actual paired-row FC schedule and
check their boundary/state conditions against these independent tests. Then derive
one instruction dependency/service path (I3) and make bandwidth/latency provenance
concrete. Multilevel caches require ordered miss streams and routing/write policies;
a miss-rate histogram alone does not define them. General CNN layers remain later
scope. Board access is needed for I5 validation, not for these mathematical checks.
