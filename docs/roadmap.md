# Project roadmap — proposal for review

16 October 2026. This roadmap orders the remaining work around the project's goal:
an **analytical** model of the cache/memory hierarchy, fed by a
**cycle-accurate instruction frontend**, that predicts edge-ML latency, throughput and later energy
on one real Cortex-M4 board. It refines research-plan section 22 (I1–I6) and the
[implementation plan](implementation-plan.md). It does not change any closed decision.
Hour estimates are engineering guesses for one developer, not commitments.

## 1. Where the project stands

Verified on 16 October 2026: host Release build with GCC 13, all 7 CTest groups pass.

| Increment | Status | Evidence |
|---|---|---|
| I1 — FC workload/address algebra | **Source level done; binary gate open** | `fc.cpp`, golden `[2,-3,6]` case, independent event/rounding oracles, ARM analysis ELF + manifest |
| I2 — Cache mathematics | **Done for synthetic contract** | Set-assoc LRU reuse analyzer, cold-sweep closed form (13,440 cases), roofline bounds |
| I3 — Dependency/resource algebra | **Memory side only** | FIFO/credit/beat service recurrence + periodic closed form; no instruction timing |
| I4 — Stateful composition | **Partial, synthetic** | Multilevel hierarchy, inclusion, fixed-point block acceleration; not on the FC binary |
| I5 — Board validation | Not started | No board yet |
| I6 — Expansion | Not started | — |
| Frontend (fetch/decode/execute) | **Not started** | Only static disassembly inspection |
| Energy | Deferred by design | — |

Things to fix first:

1. **Most I2/I3/I4 work is not committed.** `git status` shows 20+ new source, test and doc files.
   Commit them before anything else.
2. **The analytical model has not yet been run on the real kernel's access stream.**
   The cache math runs on the *source* schedule. The binary does different things: it reorders
   the bias loads, adds stack traffic, and fetches instructions. Section 3 fixes this by
   making the frontend produce that stream.
3. **The metrics need to match the workloads.** Time-to-first-token is an
   autoregressive-LLM metric. The four MLPerf Tiny models are single-shot CNN/FC inference,
   so the measurable quantities are **cycles and seconds per inference**, per-layer attribution,
   and throughput = inferences/s (equal to 1/latency at batch one with no overlap). TTFT only
   becomes meaningful for a later sequence-model target; this roadmap treats it as out of scope.

## 2. Architecture the roadmap builds toward

```text
  ELF + layout ──► Frontend: Thumb-2 fetch/decode/execute (functional, C++)
                     │  dynamic stream: ifetch, loads/stores (addr, width), retired instrs
                     ▼
            Core timing: Cortex-M4 3-stage in-order pipeline (TRM tables + stated assumptions)
                     │  memory requests with issue times and dependencies
                     ▼
   ┌──────── Analytical memory model (the research contribution) ────────┐
   │ closed-form / piecewise miss counts for regular regions             │
   │ ART flash I/D cache + prefetch model; SRAM banks; bus service       │
   │ exact state hand-off at region boundaries; finite fallback otherwise │
   └──────────────────────────────────────────────────────────────────────┘
                     │
                     ▼
     cycles/inference, per-layer breakdown, traffic, (later) energy actions
```

The **detailed** path (frontend → pipeline → access-level cache) is the reference oracle.
The **analytical** path replaces whole loop regions with formulas whose outputs and exit
state are proven equal to the reference under the stated abstraction. This split already
exists in the codebase (`analyze_reuse` versus `analyze_cold_sweep`, finite versus
`stable_blocks`). The roadmap scales it up to real kernels.

## 3. Phases

Each phase ends in a **gate**, a check that must pass before claiming the phase is done.

**Pre-December track (decided 16 October 2026):** Phase 0, Phase A, Phase K (multi-core
cluster) and the write-up snapshot. Everything is validated by directed tests in testbench
mode; no board or frontend is needed. Phases B–H follow after the application deadlines.

### Phase 0 — Consolidate (≈4–6 h)

- Commit the uncommitted I2/hierarchy/service/testbench work in reviewed chunks.
- Move the working checkout into the WSL filesystem. The mounted-drive build has had clock-skew warnings.
- Add CI (GitHub Actions: GCC 13, Debug+ASan/UBSan and Release, `ctest`).
- Order the NUCLEO-L4R5ZI now (≈US$25). It is cheap, and Phase E is the project's credibility step.

**Gate:** clean checkout builds and passes in CI; README badge.

### Phase A — Analytical FC cache model: the showcase (≈25–35 h)

This phase is the mathematical-modeling contribution, so it comes first. It already has
everything it needs: the FC source schedule (`fc.cpp`), the exact reuse analyzer, and the
independent oracles.

**A1. Single-call miss theorem for the paired-row stream.** The DSP kernel reads weight
rows `j` and `j+1` interleaved, four bytes at a time, plus bias. Under modulo mapping, row
`j+1`'s current line lies `N` bytes after row `j`'s. Whether the two lines collide in a set
is therefore a function of `N mod (S·L)` and base alignment. When they collide and `W=1`,
every access can miss even though the working set is two lines: a *resonance* effect.
Derive the miss count `M(N, M_out, base, L, S, W)` as an explicit piecewise formula,
covering the bias lines, odd-row tail, column tail and line-straddling reads. Prove it.

**A2. Repeated-call (warm) count.** Compose A1 with the existing cold-sweep result: which
weight lines survive to the next call depends on per-set population `k_s` versus `W`. Give
the exit state constructively, as `analyze_cold_sweep` already does.

**A3. Verification.** Exhaustive comparison against the independent timestamp-LRU oracle
over small grids (N, M ≤ 64; all bases mod S·L; S, W, L sweeps), plus random large cases.
Check counts **and** final state.

**A4. Approximation study (research-plan experiment E2).** Over the same sweeps, compare
three models with the exact result:
- a fully associative reuse-distance model (HayStack-style abstraction);
- the binomial random-set-mapping approximation (AMM-style, research-plan §7.4);
- the exact set-aware formula.
Plot error against `N mod (S·L)`. The expected finding, to be shown rather than assumed, is
that the statistical models miss the resonance spikes. That is a crisp, explainable result.

**A5. Cost.** Analysis time of the closed form versus finite replay as N·M grows (the expected
shape is O(M) or O(1) against O(N·M)). Report end-to-end time including setup, not only the
evaluator.

**Gate:** zero mismatches against the oracle on all supported cases. The approximation
errors are reported with their failure regions. The theorem has a written proof in
`docs/fc-cache-theorem.md`.

**Snapshot cut line:** A1–A3 are required for December. A4 and A5 are stretch goals.

### Phase K — Multi-core cluster with shared cache hierarchy and memory (≈30–40 h)

**Superseded in detail by [cluster design revision 2](cluster-design.md).** That revision
uses tiles on a k×k mesh or torus: each tile has a core, a private L1, an L2 bank and a
router. Lines are interleaved across banks, there is one memory controller, links are
contended under XY routing, and the grid sizes are 2×2, then 3×3, then 4×4. The text
below is the original single-arbiter outline.

This scope was added on 16 October 2026. P synthetic cores, each with a private cache path,
share a lower cache level and/or the memory service. The phase extends the existing models
from one injection entry to several competing request sources with explicit arbitration.
It is **synthetic only**: the selected NUCLEO-L4R5ZI is single-core, so the board cannot
validate it. Hardware validation would need a separate multi-core target and a separate
scope decision. Reads only, so there is no coherence protocol. Writes stay rejected.

**K1. Core as a dependency-driven request source.** Each core runs a directed program of
segments `(c_k, request_k)`: `c_k` cycles of compute, then a read (address, width). With one
outstanding blocking request, the arrival of the next request is
`a_(k+1) = d_k + c_(k+1)`. More than one outstanding read per core is deferred. This is
the closed-loop feedback that research-plan §20.4 warns an
unstalled trace cannot supply. It is an abstract core, **not** the cycle-accurate ISA
pipeline of Phase D. Phase D's pipeline will later produce these segment streams.

**K2. Exact shared-resource semantics.** Topology comes from configuration: private L1 per
core, an optional shared L2, and one shared memory service. Arbitration at each shared
resource is `fifo` (by offered time, ties by core index), `round_robin` (explicit pointer
initial state and update rule), or `fixed_priority`. The exact evaluator is a
request-level recurrence: at each grant instant, choose among the pending head requests by
policy. It reuses the cache and credit/beat service rules and adds no per-cycle ticking. A
shared L2 sees one interleaved miss stream, so cross-core capacity and conflict eviction is
exact under LRU.

**K3. Analytical models, verified against K2.**
- **K3a. Symmetric deterministic theorem.** Take P identical cores (compute `Z`) on a
  pipelined shared memory (latency `D`, issue spacing `I`, `K` credits) under FIFO or
  round-robin. Admissions obey a one-variable max-plus recurrence with eventual slope
  `λ = max(I, D/K, (Z+D)/P)`. So the asymptotic throughput bounds
  `X = min(1/I, K/D, P/(Z+D))` are attained exactly, with latency `R = P·λ − Z`. A blocking
  server with service `S` is the case `K=1, I=D=S`. Proving the transient length is
  open: a scratch check found transients of up to 20 admissions for P ≤ 6, so it is not
  bounded by P. See [the cluster design](cluster-design.md) §4.1.
- **K3b. Approximation study.** For heterogeneous `Z_p` and mixed hit/miss streams, compare
  against the exact K2 result: mean value analysis (MVA, exact for product-form networks,
  approximate here), balanced job bounds, and the asymptotic bounds. Expect MVA to be
  pessimistic for deterministic service. Measure and report the error against P and
  utilization; don't assume it. This mirrors Phase A's A4 study on the timing side.
- **K3c. Shared-cache sufficient condition.** For disjoint cyclic per-core sweeps into a
  shared LRU level, if the per-set population summed over cores satisfies `Σ_p k_s^p ≤ W`
  for every set, then every post-cold reference hits **for any interleaving**. Prove this
  and test it adversarially. Overflowing sets depend on interleaving and stay with exact K2
  evaluation.

**K4. Testbench and configuration v2.** Add `[core.NAME]` sections (segment program or
generator, compute gaps, outstanding limit, private path), a `shared` attribute on cache
levels, and `[arbiter]` with policy and initial state. The strict parser keeps the same
rules: no hidden defaults, provenance required, version 1 files still accepted unchanged.
Reports give per-core latency, throughput, fairness (max/min per-core throughput),
wait-time breakdown and shared-level interference counters.

**K5. Directed tests.** An independent tick-stepped multi-source oracle in `tests/`, sharing no
code with K2. Hand-worked golden cases: two cores and one server, RR versus FIFO tie
handling, fixed-priority starvation, credit-limited pipelining, and cross-core L2 conflict
thrashing. A parameter sweep of the K3a theorem at every prefix. The K3b error tables. K3c
adversarial interleavings.

**Gate:** the exact K2 evaluator matches the oracle on all directed and swept cases
(completion times, counters, final cache and arbiter state). The K3a theorem holds
exactly across the sweep. Approximation errors are reported with their regimes.

**Snapshot cut line:** K1, K2, K3a, K4 and K5 are required for December. K3b and K3c are stretch goals.

### Phase B — Functional frontend: fetch/decode/execute (≈25–40 h)

The original "frontend" goal. It also closes the open I1 binary-equivalence gate.

- **B1.** ELF loader with a memory map matching the analysis linker script: flash, SRAM1–3, and the SRAM2 alias.
- **B2.** Thumb-2 decoder for exactly the instruction set appearing in the pinned
  disassembly (`probe_entry`, `arm_fully_connected_s8`, `arm_nn_vec_mat_mult_t_s8`).
  Generate the opcode inventory from `build-arm` disassembly. Unsupported encodings fail
  explicitly.
- **B3.** Executor: registers, flags, IT blocks, the DSP subset (`sxtb16`, `sxtab16`, `smlad`,
  `smlal`, saturation) and unaligned `ldr` semantics.
- **B4.** Dynamic stream output: PC/ifetch, data accesses (address, width, R/W), retired
  instruction classes, all in the stable TraceIR from research-plan §6.
- **B5.** Independent oracles: (i) numerical outputs equal the existing C++ requantization
  oracle across the 35-shape boundary suite; (ii) the access stream is differentially compared
  with QEMU (`qemu-system-arm -cpu cortex-m4` + TCG plugin). Keep QEMU as a test-only dependency,
  so the two implementations share no code.

**Gate:** bit-exact outputs and access stream agreement on all directed shapes. The I1
binary gate is then closed, and the I1 report is updated.

### Phase C — Analytical model on the binary stream (≈15–20 h)

- Re-derive Phase A on the frontend's real stream: compiler load order, widened loads,
  stack spills, and the instruction stream.
- **Instruction footprint question:** the helper is 3,612 bytes against a 1 KiB ART
  I-cache. Determine whether the hot unrolled body fits, from the disassembly's loop
  addresses and its set mapping. Derive its steady-state I-cache behavior.
- Region boundaries: prologue, steady loop, tails, epilogue. Each region gets a
  `summarize(region, entry_state) -> counters, exit_state`; finite replay at boundaries.

**Gate:** analytical counts and exit state equal the frontend-driven cache replay on every
directed shape.

### Phase D — Cycle-accurate core timing and the ART model (≈45–65 h)

This is I3 for real.

- **D1. Pipeline model.** Cortex-M4 3-stage in-order timing from the TRM r0p1 §3.3 tables:
  issue cycles, pipelined back-to-back loads/stores, address-dependency stall, multiply/MAC
  timing, unaligned penalties. Branch refill (1–3 cycles) is a recorded assumption with sensitivity.
- **D2. ART flash model.** 64-bit dual-bank reads, five wait states at 120 MHz, prefetch
  buffer, 1 KiB I-cache (LRU), 256 B D-cache, and DCode-over-ICode priority (RM0432 §3.3.4).
  Every undocumented transition goes into an assumption ledger with alternatives (audit gaps 1–3).
- **D3. Event coupling.** The pipeline issues memory requests into the existing service and
  hierarchy recurrences, under one timebase and with no double-charged latency (research-plan §8.4).
- **D4. Analytical loop timing via max-plus algebra.** A loop's state update has the
  form `x(k+1) = A ⊗ x(k)` in max-plus algebra. The steady-state initiation interval is then
  the maximum cycle mean of `A`'s precedence graph, computed with Karp's algorithm. Total region
  cycles = startup + (n−1)·II + drain, with transient length proven or checked. This is the
  timing analogue of Phase A, and it connects to the max-plus framing already in the
  [service contract](hierarchy-service-contract.md).
- **D5. Sensitivity.** Sweep the assumption scenarios (branch refill, prefetch behavior,
  ART fill granularity). Report per-scenario predictions, not confidence intervals.

**Gate:** max-plus summary equals the cycle-level pipeline on every directed loop. The
pipeline reproduces hand-computed TRM examples. Every timing parameter has provenance.

### Phase E — First hardware validation (≈25–35 h + board)

This is I5. The board ordered in Phase 0 is used here.

- Bring-up with clock 120 MHz, Range 1 boost, 5 WS, ART on, per the accepted profile. DWT `CYCCNT`
  timing; record silicon revision and errata (ES0393).
- **Discriminating microbenchmarks first:** dependent vs independent load chains, SRAM vs flash
  code, ART on/off as diagnostics, same-set conflict layouts. Each targets one assumption
  from D5.
- Freeze the model version and the error threshold **before** viewing held-out timings,
  as required by the validation contract.
- Held-out FC shapes extracted from the four MLPerf Tiny models. Cold and repeated execution
  are reported separately; per-case signed error, median/worst, variability.

**Gate:** published per-case table. Every residual is explained by mechanism (counts →
addresses → hits → service → overlap), and no correction factors are fitted.

### Phase F — First whole model: anomaly detection (≈30–45 h)

The MLPerf Tiny anomaly-detection model is a dense autoencoder. If inspection of the pinned
`.tflite` confirms it uses only FC plus supported utility ops (to verify), it is the first
whole-model validation available with **no new compute kernels**.

- `.tflite` inspection, operator inventory, TFLM revision pin, arena-offset extraction.
- Layer-to-layer state hand-off: weights stream from flash, activations reuse arena buffers.
- Whole-inference cycles, per-layer attribution, inferences/s.

**Gate:** exact outputs; whole-inference prediction vs measurement, cold and repeated.

### Phase G — Operator expansion (≈40–60 h)

This is I6. Supporting the remaining three MLPerf Tiny models requires these, in order:
1×1 convolution (a matrix multiply), depthwise convolution, general convolution
(CMSIS-NN im2col + matmul), and pooling. Each gets the same treatment: source contract →
frontend check → analytical regions (tiled affine accesses; Falcon-style decomposition
becomes worth studying here) → timing → held-out validation.

### Phase H — Energy (≈25–40 h + power profiler)

This phase stays deferred until Phase E passes. Use action counts × incremental energy
(Accelergy-style separation, research-plan §15). Characterize per-action costs with isolated
microbenchmarks on an STLINK-V3PWR or Nordic PPK2, then validate on held-out inferences over
GPIO-marked windows.

### Phase W — Write-up (continuous; ≈15–25 h focused)

- A technical report / arXiv preprint after Phase A, extended after Phases D–E. Plausible venue
  for a later version: a TinyML or workshop-level performance-modeling venue. Claim an
  "MCU adaptation and validation study" unless the evidence supports more (research-plan §17).
- Keep a one-page **project summary** (problem, one theorem, one figure, one validation table)
  separate from the detailed contracts. Reviewers will read that page, not 2,000 lines of
  contract text.

## 4. Timeline

Application deadlines start in the first week of December 2026, so the snapshot freezes
on **Friday 27 November**. The required pre-December scope (Phase 0 + A1–A3 + K1, K2,
K3a, K4, K5 + snapshot write-up) is about 60–75 h, or **10–12 h/week** over six weeks. At 8 h/week,
drop the stretch items first and keep K3a and A1 intact; they are the two theorems.

| Window | Phases | Milestone |
|---|---|---|
| Oct 19 – Oct 25 | 0, K design | Work committed, CI green, board ordered; config v2 schema and K2 semantics written down |
| Oct 26 – Nov 8 | A1–A3 | FC cache theorem, proof, exhaustive verification |
| Nov 9 – Nov 22 | K1–K5 | Cluster evaluator, oracle, K3a theorem and sweep |
| Nov 23 – Nov 27 | W | **Application snapshot frozen:** report v0, one-page summary, figures |
| Nov 28 – Dec 6 | buffer | Stretch items (A4, K3b, K3c) only if the snapshot is already frozen |
| Dec 2026 – Jan 2027 | B | Functional frontend; I1 binary gate closed |
| Feb 2027 | C | Analytical model on real binary stream incl. I-fetch |
| Mar – Apr 2027 | D | M4 pipeline + ART + max-plus loop timing |
| May 2027 | E | First board validation table |
| Jun – Jul 2027 | F, W | Anomaly-detection whole model; report v1 |
| Aug 2027 → | G, H | Operator expansion, then energy |

Total remaining: roughly 285–420 h, including Phase K. Phase K adds about 30–40 h to the
research plan's original 260–370 h estimate; the work already completed roughly offsets the
added frontend and whole-model milestones.

**Application snapshot (27 November).** The package:

1. Phase A: a proved, verified closed-form cache result for the real kernel's schedule.
2. Phase K: an exact multi-core shared-memory evaluator, the symmetric max-plus theorem,
   and directed-test evidence.
3. The existing verified hierarchy/service models and their acceleration proofs.
4. A short report and the one-page summary. Ongoing work (frontend, single-core board
   validation) is described as such. The cluster is labeled synthetic.

Do not claim hardware accuracy in the snapshot. "Exact under a stated abstraction, with
a validation plan" is honest, and it is stronger than an unsupported accuracy number.

## 5. Risks

| Risk | Mitigation |
|---|---|
| Thumb-2 decoder scope creep | Decode only the inventoried opcodes; fail on others; QEMU remains the broad reference |
| ART behavior underdocumented | Assumption ledger + scenario sweeps; Phase E microbenchmarks target the largest sensitivities |
| Closed form in A1 becomes case-explosion | Fall back to a per-row O(M) evaluator with proven per-row formula; still far below O(N·M) |
| Board result disagrees badly | Diagnose in the research-plan §13 order; never fit; freeze, revise, re-holdout |
| Time pressure before deadlines | Cut lines in A and K; the two theorems (A1, K3a) plus their oracles are the minimum defensible package |
| Cluster model mistaken for a hardware claim | Label every cluster output synthetic; L4R5 is single-core; multi-core hardware needs a separate scope decision |
| Arbitration semantics ambiguous (ties, RR pointer) | Specify grant rules and initial state in config; oracle implements them independently from the written spec |

## 6. Decisions

Recorded 16 October 2026 by the user:

1. Application deadlines start in the first week of December. Snapshot freeze: 27 November.
2. Before December: mathematical models of the cache hierarchy, a **multi-core cluster** and
   memory, validated by directed tests in testbench mode (Phases A and K). The frontend
   (Phase B) follows.
3. QEMU is accepted as a test-only differential oracle for the frontend.
4. TTFT is out of scope for the Cortex-M4/MLPerf Tiny target.

5. Working mode: about one supervised hour per day with Claude Code. Claude implements,
   the user reviews and owns the theory. The hour estimates above assume solo development;
   in this mode the bottleneck is review and understanding, so each session targets one
   reviewable step (one derivation, one module, or one test suite).

## 7. Study references for Phase K

Read only the sections each task needs.

- Baccelli, Cohen, Olsder and Quadrat, *Synchronization and Linearity* (1992), chapters 2–3:
  max-plus linear systems, cycle time, transients.
- Lazowska, Zahorjan, Graham and Sevcik, *Quantitative System Performance* (1984, free
  online), chapters 5–6: asymptotic and balanced job bounds, MVA.
- Reiser and Lavenberg, "Mean-Value Analysis of Closed Multichain Queuing Networks",
  JACM 27(2), 1980.
- Research-plan §20.2–20.4 (arbitration models; why to start with deterministic recurrences).
