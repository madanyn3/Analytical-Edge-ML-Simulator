# Edge ML Hybrid Simulator Research and Action Plan

Research synthesis and proposed implementation specification

23 September 2026

## 1 Executive decision

Build a **mechanistic hybrid performance simulator**, not a regression predictor and not a full cycle-by-cycle board emulator. Its prediction should arise from the deployed software's operations, dependencies, addresses and target resource rules.

Use three cooperating levels of fidelity:

1. **Symbolic and analytical models:** loop counts, regular address streams, reuse conditions, memory allocation, resource bounds and repeated steady-state execution.
2. **Access-level state simulation:** cache tags, replacement, dirty lines and flash-buffer state, without ticking a global clock each cycle.
3. **Event-driven timing:** dependencies, resource occupancy and memory completions where aggregate formulas lose execution order.

Trace-driven and analytical are not opposites. A trace can supply inputs to an analytical model. Conversely, a symbolic address generator can drive an exact cache-state simulation without recording a trace. Choose the workload representation separately from the hardware evaluation method.

**Research conclusion:** the proposed direction is well supported by prior work, but no reviewed source provides an already validated Cortex-M ML simulator that combines all these pieces. The work lies in faithful kernel representation, target-specific timing, and safe composition of fast and detailed modes. The recommendations below are a proposed design, not measured project results or claims of novel research.

This report supersedes the regression-based methodology of the earlier project plan. Energy remains a later extension. The original 20-week estimate should not be retained for a comprehensive multi-target simulator.

**Mathematics-first refinement:** Sections 20–23 extend this plan with Falcon, analytical arbitration/queueing research, verified public projects, and a specification-first implementation sequence. Use that sequence instead of interpreting Section 19 as a requirement to build tracing infrastructure first. No workload timing regression is required. Detailed simulation is a correctness reference and an explicitly selected fallback, not the default engine.

## 2 Research coverage and important corrections

This is a focused technical review of directly relevant primary papers, author repositories and vendor/tool documentation. Core methodological and limitation sections were examined for AMM, cache-miss equations, HayStack, reuse-based replacement models, ECM, Kerncraft and warping cache simulation. Other references provide implementation or extension context. It is not an exhaustive systematic review, and the software artifacts were not installed or reproduced during this research.

### The paper you identified

*An Analytical Memory Hierarchy Model for Performance Prediction* is by **Gopinath Chennupati, Nandakishore Santhi, Stephan Eidenbenz and Sunil Thulasidasan**, WSC 2017. W. K. V. Chan is a proceedings editor. Its AMM uses basic-block-labeled traces from small executions, sampled reuse distributions and an analytical cache-hit model inside PPT. It therefore supports a hybrid approach, not prediction from a board specification alone. Its demonstrations are matrix multiplication and Blackscholes on HPC hardware; they do not establish MCU accuracy. [R1]

### Corrections to the previous architectural advice

- A full ARM instruction trace is useful as a reference and fallback, but need not be the normal prediction path.
- A cache reference simulator need not be cycle accurate. Updating tags for each access is much cheaper than simulating a pipeline every cycle.
- STM32F446 includes a vendor flash accelerator; it must not be treated as a generic L1 data cache covering all SRAM. [R12]
- STM32F7/H7 cache documentation describes pseudo-random replacement and non-cacheable TCM paths. An exact LRU derivation is not automatically an exact model of that hardware. [R13]
- A functional emulator establishes instruction semantics and addresses, not physical timing. QEMU explicitly distinguishes instruction counting from cycle-accurate emulation. [R15]
- Replaying a fixed trace is trace-driven timing, not a fully execution-driven model with timing feedback. This distinction becomes important for interrupts, polling, concurrent threads and DMA; exclude them initially.

## 3 Literature map and implementation decisions

### 3.1 Analytical locality and cache behavior

| Work | Main contribution | Limitation relevant here | Decision |
|---|---|---|---|
| AMM, Chennupati et al., 2017 [R1] | Converts sampled basic-block reuse behavior into cache estimates within a performance toolkit | Trace-derived profiles and small-input extrapolation need checking; a locality histogram does not preserve dependency timing | Implement a small baseline, not the main timing engine |
| Cache Miss Equations, Ghosh, Martonosi and Malik, 1999 [R2] | Represents reuse and set conflicts algebraically for regular loop nests | Affine access/control restrictions; general symbolic solving is costly | Borrow address/set equations; start with hand-derived kernel templates |
| HayStack, Gysi et al., PLDI 2019 [R3] | Symbolically counts reuse distances and misses for fully associative LRU caches | Idealized replacement/associativity and source-to-machine access differences | Reference analytical backend and comparison baseline |
| Reuse-based Online Models, Sen and Wood, 2013 [R4] | Separates locality from associativity and replacement-policy modeling, including non-LRU policies | Statistical mapping assumptions and policy abstractions may fail on structured embedded layouts | Optional approximate backend; compare with exact address mapping |
| Warping Cache Simulation, Morelli and Reineke, PLDI 2022 [R5] | Detects cache-state equivalence under address renaming and safely skips repeated accesses | Requires analyzable future accesses, compatible state transformations and modeled policy behavior | Strongest template for the long-term fast state-preserving cache backend |

**Assessment:** for regular quantized kernels, symbolic addresses plus state-preserving acceleration are a better primary direction than assuming uniformly distributed cache conflicts. Keep statistical reuse models available for comparison and fast approximate exploration.

The key HayStack lesson is symbolic counting, not its reported numerical accuracy on other systems. Its fully associative LRU model, cold-cache assumptions and omission of instruction fetches must remain visible when used. Do not inherit its hardware error claims. [R3]

The key warping lesson is that skipping work requires a valid transformation of the final cache state. Repeated miss counts alone are insufficient. Its correctness reasoning applies to the modeled cache and analyzable accesses; extending it to coupled pipeline timing or an unknown hardware pseudo-random generator requires additional work. [R5]

### 3.2 Turning work and traffic into time

| Work | Useful idea | What not to assume |
|---|---|---|
| Roofline, Williams, Waterman and Patterson [R6] | Compute and bandwidth limits provide fast consistency checks and bottleneck bounds | A bound is not a latency prediction; burst costs and dependencies matter |
| Execution Cache Memory model, Hofmann, Eitzinger and Fey [R7] | Explicitly combines in-core work and transfers according to overlap rules | Intel overlap rules and streaming assumptions do not transfer unchanged to Cortex-M |
| Kerncraft, Hammer et al. [R8] | Modular machine/code descriptions with alternative analytical and simulated cache backends | It is not a ready-made Cortex-M model; supported kernel syntax and backend capabilities must be checked |
| LLVM MCA [R9] | Static machine-code throughput/resource analysis can help build and check block models | A usable, accurate Cortex-M scheduling model cannot be presumed merely because the compiler supports ARM |

Use **ECM-style decomposition**, but derive each overlap rule for the actual target. Begin with a conservative event-based resource model, then replace repeated regions with verified formulas. Kerncraft is the closest software-architecture precedent for interchangeable analytical and simulated backends. [R7, R8]

### 3.3 ML mapping and energy

Timeloop separates a workload from its mapping onto hardware resources. Borrow that separation: an ML graph is not an execution schedule. MCU execution is determined by the selected kernel, compiler and layout, rather than a freely chosen accelerator dataflow. [R10]

Accelergy separates action counts from the energy cost of those actions. This is a useful v2 interface: count instructions, memory transactions and active/idle periods now, attach defensible energy parameters later. Accelerator component models are not validated MCU-board energy models. [R11]

## 4 Define the prediction contract before writing the simulator

### Input modes

**Deployment-faithful mode:** TFLite model + pinned build recipe + linked ELF/map + target configuration + deterministic input. This is the validation mode.

**Exploration mode:** TFLite model + kernel mapping + hypothetical hardware description. Compiler choices, addresses or undocumented timing may be approximated. Label output as exploratory, not as a validated board prediction.

A `.tflite` file alone cannot determine final instructions, kernel selection, stack traffic or memory placement. Compiling a model with an existing toolchain is permitted; building a new compiler is not part of this project.

### Initial workload scope

- Batch one, static shapes, dense int8 inference, single thread.
- FC and 1×1 convolution first; then general convolution, depthwise and pooling.
- Fixed TFLite Micro and CMSIS-NN revisions and compiler flags.
- No interrupts within the measured region, no DMA, no radio traffic, no concurrent RTOS work.
- Explicit cold-start and repeated-inference modes; initialization separate from Invoke.
- Preserve activation, bias, requantization, padding and tail handling costs.
- Add utility operators only when their semantics, traces and timing are supported.

### Outputs

1. Whole-inference cycles and time, with per-layer attribution.
2. Dynamic instruction counts by class and executed kernel identity.
3. Memory transactions, cache hits/misses, writebacks and region traffic.
4. Resource busy time, visible stalls and critical-path contribution; do not present overlapping busy times as an additive latency breakdown.
5. Arena and template-firmware RAM, model bytes and linked flash footprint.
6. Model assumptions, parameter provenance, fallback regions and uncertainty type.
7. Simulator runtime, preprocessing runtime, peak host RAM and trace size.

Label every output as **exact under stated abstractions**, **approximate**, **measured**, or **unsupported**. A linked flash footprint is a build-derived quantity, not simulated timing.

## 5 Target strategy

Two goals require different test environments: debugging the simulator and demonstrating analytical cache research.

| Target | Role | Timing |
|---|---|---|
| Synthetic CPU/cache configurations | Validate analytical formulas against a controlled state simulator; sweep capacity, associativity, layout and replacement | From the first month; no board needed |
| Cortex-M4 STM32F446-class board | Validate instruction semantics, core timing, SRAM accesses and flash-path modeling | First physical target |
| Cortex-M7 STM32F767-class board | Validate a meaningful I/D-cache system and TCM-versus-cacheable placement | Second hardware milestone, essential to the cache-focused research claim |
| ESP32-class target | Cross-ISA extension requiring new frontend, kernels and timing characterization | Deferred |

If purchasing only one board and cache hierarchy is the overriding objective, choose the M7 after a documentation/emulation feasibility check. If the priority is a working simulator sooner, begin with M4 and use synthetic caches in parallel. Buying both eventually is useful, but not required to start.

**Do not model a fictional three-level hierarchy on a board without one.** SRAM banks, TCM, flash accelerators and caches are distinct components, not interchangeable levels.

The STM32F446 reference manual link was located, but its full text could not be retrieved through the research browser. Exact flash-buffer geometry, arbitration rules and wait-state tables are therefore an explicit documentation task, not verified constants in this report. The available Arm manual link also requires local verification against the chosen core revision. [R12, R14]

## 6 Proposed architecture and interfaces

Use Python for experiment orchestration, schemas and early algebra; C++ for access replay and event scheduling once profiling justifies it. Begin with explicit templates for a few kernels rather than a general compiler-analysis pass.

| Module | Input | Output | Main responsibility |
|---|---|---|---|
| Model inspector | TFLite | GraphManifest | Shapes, quantization and support checks |
| Build adapter | Model + pinned recipe | ELF, map, symbols, build hash | Reproduce deployed implementation |
| Kernel mapper | Symbols, dispatch conditions, small functional runs | KernelManifest | Identify actual optimized and fallback paths |
| Workload frontend | Kernel + shape + layout | RegionIR or TraceIR | Express work without imposing timing |
| Core model | Instruction/dependency representation | Resource events or region summary | Compute and dependency timing |
| Memory model | Ordered accesses or symbolic streams | State transitions, traffic, completions | Locality, transfers and contention |
| Hybrid controller | Region assumptions + state | Backend selection and state handoff | Preserve behavior across fidelity changes |
| Allocator model | Graph and runtime rules | Arena layout and high-water requirement | Resource capacity and placement |
| Reporter | Events, summaries, manifests | JSON and readable report | Explain predictions and limitations |

### Region representation

Represent a region as:

- integer iteration domain and ordered statement schedule;
- piecewise address functions and transfer width;
- read/write/instruction-fetch classification;
- operation classes and dependency edges, including loop-carried edges;
- prologue, repeated body, epilogue and boundary paths;
- input/output buffers, aliases and actual or symbolic base addresses;
- parameter validity domain and binary/kernel hash;
- incoming-state requirements and outgoing-state transformer.

Resolve tensor dimensions before affine analysis where needed. Products of a loop variable and an unfixed symbolic dimension are not ordinary affine expressions; specialize dimensions or use a representation that explicitly supports them.

### Trace representation

Use sequence number, PC, decoded instruction ID, register inputs/outputs, memory address, width, access type, branch outcome and region ID. Preserve access order within multi-access instructions and split accesses spanning lines. Avoid a text file containing every decoded instruction: maintain a static instruction dictionary and a compressed dynamic stream.

Tracing executed PCs does not capture all speculative or prefetched instruction fetches. Add those through an explicit fetch model or declare them omitted.

### Functional execution is separate

Skipping timing analysis must not skip computation needed to determine subsequent execution. A simple first implementation still executes the firmware functionally and compresses the timing representation. This speeds timing replay but not necessarily trace generation.

The faster path later generates validated regular kernel regions directly. It must either compute outputs through a functional kernel or demonstrate that subsequent control/access patterns do not depend on omitted values. Keep golden-output testing separate from timing abstraction.

## 7 Analytical memory models to implement

### 7.1 Memory capacity before access timing

For a tensor with extents d and element size e, storage is e times the product of its extents, rounded according to the runtime's alignment. This is not sufficient for peak RAM: lifetimes, persistent metadata, scratch and initialization temporaries matter.

Reproduce the pinned TFLM planner or its allocation decisions, including ties and alignment. A maximum-live-bytes calculation is a lower bound when fragmentation or placement constraints exist. Arena placement and reuse also determine cache conflicts, so export actual offsets to the access model. Validate allocation and timing separately. [R16]

### 7.2 Address and traffic algebra

For a regular access, construct byte address:

```text
a(i) = base + element_bytes × (c0 + c1*i1 + ... + ck*ik)
line(i) = floor(a(i) / line_bytes)
set(i) = line(i) mod number_of_sets
```

These are a restricted representation for the actual compiled access schedule, not a claim that all kernels are affine. Padding branches, tails and packing stages become separate regions. Include compiler spills, stack accesses, bias reads and quantization arrays.

Count separately: logical tensor elements, explicit loads/stores, line fills, dirty evictions, bus beats and instruction-fetch transactions. These counts must not be conflated. Line-based traffic depends on alignment and state, not just tensor byte size.

### 7.3 Fully associative LRU reference analysis

Define reuse distance D as the number of distinct **line addresses** since the previous reference to the same line. First references have infinite D. For a fully associative LRU cache of K lines, a reference hits exactly when D < K, under ordinary allocation behavior and without interfering invalidations/prefetches.

Implement an explicit reuse-distance calculator for small traces, then a symbolic special case. Standardize the zero-based definition: papers sometimes count stack position differently. Tests must cover D = K−1, D = K and compulsory misses. [R3, R4]

### 7.4 Set-aware analysis and a statistical alternative

For a W-way LRU cache, the analogous test uses distinct intervening lines in the **same set**. Derive this exactly from actual addresses when feasible; base offsets matter. [R2]

For comparison, a random-independent-mapping approximation with S sets gives:

```text
P(hit | D) ≈ sum over k=0..min(W−1,D)
             choose(D,k) × (1/S)^k × (1−1/S)^(D−k)
```

This assumes each intervening distinct line independently maps to the reused line's set. It is an approximation to set conflicts, not an exact prediction for strided arrays, pseudo-random replacement or arbitrary hierarchies. Cold misses remain separate. Use numerically stable probability routines. [R1, R4]

**Diagnostic example:** for a hypothetical 4-set direct-mapped cache with 32-byte lines, alternating addresses 0 and 128 repeatedly conflicts despite a tiny working set. Alternating 0 and 32 does not. A global reuse histogram cannot distinguish these mappings. Include this as an early unit test.

### 7.5 Working-set and layer conditions

For a chosen tile, derive the live input patch, weight block, output block and scratch footprint. Use this to identify plausible reuse regimes and partition a size sweep around capacity thresholds.

Do not treat “tile bytes < cache bytes” as a sufficient hit guarantee. Check set occupancy, competing streams and replacement policy. Begin with exact proofs for simple streams; use capacity conditions only as labeled approximations elsewhere. The relevant working set can be a subset reused between two references, not every byte in the entire layer.

### 7.6 Hierarchy composition

The next level receives the previous level's misses, write-through traffic, writebacks and prefetch requests—not an unchanged copy of all CPU references. Preserve those traffic classes and their ordering where the downstream model needs it.

A miss-ratio histogram alone generally cannot reconstruct the lower-level stream. Initially support one cache level and SRAM/flash. Add multilevel composition only with either an emitted miss stream or a mathematically justified stream summary. Avoid inventing an L2 requirement for M7 hardware that does not have one.

### 7.7 Warm state and layer boundaries

Each layer must consume the state left by its predecessor. Reused activation buffers can retain unrelated dirty lines; weights compete with scratch; code consumes instruction-cache capacity. Cold-cache-per-layer prediction is a separate diagnostic, not the default total-inference estimate.

A region summary should implement:

```text
summarize(region, entry_state) -> counters, exit_state, timing_summary, assumptions
```

If an analytical model produces a histogram but no concrete exit state, it cannot silently hand off to exact replay. Replay the boundary/state transition, retain symbolic state, or report an approximate state ensemble. A state model that is adequate for miss counts may be inadequate for dependent memory completions.

## 8 Core timing and memory service models

### 8.1 Static core model

Build instruction-class tables from the selected revision's manuals and targeted experiments. Include issue occupancy, result latency, operand-dependent timing where relevant, load/store behavior, branch refill and DSP instructions. A compiler's instruction support is not a timing specification.

For a loop, useful checks include:

```text
resource bound = max over resources (required occupancy / capacity)
recurrence bound = max over dependency cycles (latency / iteration distance)
```

These are lower bounds under the stated resource graph, not guaranteed throughput. Finite-loop startup, instruction ordering, fetch stalls and in-order restrictions may increase time. Use actual instruction patterns rather than MAC count alone.

Where a fixed repeated body reaches a verified steady state, summarize:

```text
region cycles = startup + (number_of_repeats − 1) × initiation_interval + drain
```

Define the first-iteration and final-completion conventions in tests. Preserve loop-carried dependency state. A basic-block cache keyed only by PC is insufficient when entry register-readiness or memory state differs.

### 8.2 Minimal event scheduler

Track next issue opportunity, source readiness, functional-resource availability, pending loads, store-buffer occupancy and memory completion events. Advance to the next relevant event instead of incrementing every clock cycle.

A starting rule is:

```text
issue_i = max(frontend_ready_i, operands_ready_i, resource_ready_i)
complete_i = issue_i + service_latency_i
```

Extend this with the target's in-order issue, forwarding and branch/fetch rules. Distinguish unit initiation interval from completion latency. For M7, pairing and dependency restrictions must be modeled explicitly before claiming dual-issue benefits.

### 8.3 Memory service

Begin with explicitly blocking SRAM transactions and an isolated flash path. For each region define access width, alignment rules, latency units, bus width, clock ratio and service occupancy. Convert bus cycles to a common timebase before combining them with CPU cycles.

For a serial burst service model:

```text
service cycles = initial_latency + (beats − 1) × beat_interval
```

This is a proposed model form, not a universal controller rule. Document whether initial latency includes the first beat and whether arbitration, turnarounds and queueing are already included.

Upgrade to a small queue/arbiter only when two streams share a resource or outstanding transactions matter. Instruction fetch and data traffic may contend even with a single application thread. External DRAM timing is out of v1 scope.

### 8.4 Never equate miss count with stall time

The same miss count can produce different execution times for a dependent load chain and independent loads with arithmetic between them. A miss can occupy the bus while the processor does useful work; conversely, a small number of misses may dominate the critical path.

Use analytical transfer totals where overlap is established and stable. Use event scheduling where dependency order, outstanding requests or shared-resource contention determines overlap. AMAT is a diagnostic service metric, not something to multiply indiscriminately by all accesses and add to already counted instruction cycles.

For a simplified pair of work components, max(compute, transfer) and compute + transfer illustrate full-overlap and serial scenarios. They are not universal bounds once omitted queueing, fetch work or dependencies change the components themselves. Do not copy server ECM overlap rules onto an MCU. [R7]

## 9 Hybrid acceleration policy

### Fidelity ladder

| Level | Behavior | Promotion requirement |
|---|---|---|
| F0 | Arithmetic/resource bounds and ideal locality | Diagnostic/exploratory only |
| F1 | Symbolic kernel counts, regular transfers, validated steady-state timing | Binary mapping and preconditions match |
| F2 | Symbolic/generated accesses with explicit cache state | Access order and replacement model known |
| F3 | Instruction/dependency replay with event-driven memory timing | Used for unsupported regions and cross-checks |

F3 is a reference implementation of your abstraction, not guaranteed hardware truth. Compare it with real measurements before using it to certify a faster model.

### Region selection procedure

1. Identify the exact kernel binary and region boundaries.
2. Check shape, layout, alignment, memory route and control-flow assumptions.
3. Select the lowest-cost backend already validated for those assumptions.
4. Validate a development subset against the next higher-fidelity mode.
5. Preserve or reconstruct entry/exit cache and timing state.
6. Fall back with a reason when an assumption fails; never silently extrapolate.

Initially use a manual capability registry. Automated model selection is a later optimization; do not build a machine-learning selector.

### Restricted warping experiment

First implement deterministic LRU, one cache level and one affine streaming loop. Detect repeated normalized cache states and verify that subsequent address translations preserve set relationships, access order and policy metadata. Stop at tile, padding, loop-tail or control boundaries. Validate hits, misses, dirty evictions and exit state against complete replay. [R5]

Only then extend to timing-state summaries. Repetition of cache state alone does not prove repetition of pending loads or pipeline readiness. If the hardware uses pseudo-random replacement with hidden state, use a declared approximate policy/seed ensemble or disable exact warping claims. Do not pretend that randomly choosing a way reproduces a specific hardware replacement sequence.

## 10 Worked design example for an int8 convolution

This is a modeling procedure, not a claim that CMSIS-NN uses one fixed convolution algorithm.

1. Inspect the pinned wrapper and confirm which kernel is selected for the shape and options.
2. Separate input packing, multiply-accumulate loop, requantization, activation clamp and output store.
3. For the selected tile, derive iteration counts and actual input/weight/scratch access order.
4. Verify a tiny instance against disassembly and a functional trace, including tails and boundary padding.
5. Derive symbolic access streams for regular interior tiles; retain trace/event fallback for exceptional paths.
6. Analyze weight and input reuse using addresses, not just MAC totals. Include scratch writes and subsequent reads as separate operations.
7. Model repeated arithmetic with a dependence-aware steady-state summary when valid.
8. Send cache fills and visible load dependencies to the timing model. Avoid charging scratch traffic both inside a block summary and again as an external penalty.
9. Carry state through output writes and the next layer.
10. Compare one layer, a two-layer chain and a complete inference separately.

A cache-fit calculation may say weights remain resident; a trace check can reveal competing scratch lines mapping to the same sets. This is precisely where set-aware analysis or a small state simulator adds value without full cycle-level simulation.

## 11 Research experiments before committing to complexity

Treat each experiment as a falsifiable design decision. Save predictions before collecting final hardware results.

| ID | Question | Controlled experiment | Decision criterion |
|---|---|---|---|
| E1 | Does the chosen cache model capture address conflicts? | Stream, stride, pointer offsets and same-set thrashing on synthetic caches | Exact variants match every hit/miss and final state |
| E2 | When is a global reuse model adequate? | Compare statistical, set-aware and replay backends across alignment sweeps | Keep statistical mode only where errors and limitations are characterized |
| E3 | Can regular regions avoid complete tracing? | FC/conv tiny traces versus symbolic counts and generated accesses | Exact agreement on all claimed deterministic counts/addresses |
| E4 | Do cache misses explain timing? | Dependent versus independent loads with similar traffic; compute interleaving | Introduce event timing where aggregate composition fails |
| E5 | Are summaries composable? | Layer A then B versus B alone; cold and warm execution; aliasing buffers | Same counters/state as full replay under the same abstraction |
| E6 | Does warping pay for itself? | Increase loop extent and measure setup plus runtime, not replay alone | Retain only where end-to-end speed gains exceed implementation cost |
| E7 | Does an LRU approximation work on the chosen M7? | Capacity and conflict stress with repeated runs and alternative policies | Publish sensitivity; no policy-exact claim without evidence |
| E8 | Does the model respond causally to changes? | Vary legal clock/wait-state settings, placement and alignment independently | Correct trend and magnitude without per-workload refitting |

Small-input symbolic derivations need boundary-size checks. Include dimensions just below/above SIMD widths, tile sizes, cache capacity and line boundaries; a small trace may never execute a fallback or tail that dominates another case.

## 12 Detailed implementation roadmap

Planning assumption: one developer at 6–8 hours per week. The proposed tasks total roughly **260–370 focused hours**, about **33–62 weeks**, excluding procurement delays and major tool-porting failures. This is an engineering estimate, not literature evidence. A useful single-kernel prototype should arrive well before the complete research system.

### P0 Research and scope freeze 20 to 28 hours

**Actions**

1. Read R1, R3, R5 and R8 at overview/assumption level; read R2, R4 and R7 for the chosen mechanism.
2. Maintain a paper matrix: input representation, cache assumptions, timing assumptions, validation workload, artifact and adaptation cost.
3. Audit board/core manuals and tool support; list inaccessible or undocumented details.
4. Choose the first board and one kernel/compiler/runtime tuple.
5. Define the scope and exclusion manifest, target schema and measurement boundary.
6. Check applicable employer outside-project/publication policies; use public sources and personal resources.

**Deliverables:** literature matrix, architectural decision log, initial machine profile, frozen scope.

**Gate:** every required hardware parameter is documented, experimentally identifiable, or explicitly uncertain. Do not quietly populate unknowns with generic Cortex defaults.

### P1 Reproducible kernel and trace feasibility 20 to 28 hours

1. Compile a tiny integer/DSP test and a small FC kernel for the target.
2. Save ELF, map, disassembly, source revisions, compiler flags and checksums.
3. Establish hardware correctness and cycle measurement.
4. Test a functional frontend with the actual instruction subset and memory layout. QEMU plugins are a candidate, not a promise of complete STM32 board support. [R15]
5. If board boot support is absent, run an isolated kernel harness with explicit memory mappings and a stub environment; keep its executable region identical and acknowledge excluded boot/peripheral behavior.
6. Export PC and memory events; check multi-access instructions and DSP semantics.

**Gate:** matching output and independently checked instruction/access traces for at least ten tiny tests. Stop and reassess if frontend integration requires a broad emulator port.

### P2 Reference cache and memory components 18 to 24 hours

1. Write an access-level cache simulator supporting configurable sets, ways, lines and LRU.
2. Add read/write policies and dirty eviction accounting needed by the intended experiments.
3. Add memory regions, line splitting and consistent address units.
4. Implement explicit cold state, warm-up, state snapshots and final-state comparison.
5. Test capacity, conflicts, alignment, writebacks, alternating streams and region boundaries.

**Gate:** hand-worked sequences and a second simple implementation agree exactly. This is the reference for analytical models, not a cycle-accurate simulator.

### P3 Region representation and first analytical memory backend 24 to 32 hours

1. Define RegionIR and TraceIR, versioned serialization and manifests.
2. Implement an affine FC/dot-product address generator and one tiled matrix-like kernel.
3. Derive compulsory/streaming counts and fully associative reuse conditions.
4. Add actual set mapping and base-offset sensitivity.
5. Compare analytical counts with reference replay across hundreds of small generated configurations.
6. Implement the AMM-style statistical calculation as a baseline if time permits.

**Gate:** exact claims pass exact differential tests; approximate claims carry quantified errors. Do not add a full polyhedral compiler frontend yet.

### P4 First core timing model 24 to 36 hours

1. Define instruction latency, occupancy and dependency semantics for used instructions.
2. Implement event-driven issue/readiness and a simple blocking memory interface.
3. Test arithmetic chains, independent instructions, MAC loops, loads/stores and branches from SRAM where feasible.
4. Isolate frontend/memory effects through controlled placement and documented settings.
5. Add a steady-state timing summary for one repeated loop; preserve startup and drain.

**Gate:** proposed target median error ≤10% on controlled core microbenchmarks, with every larger error investigated. Record actual evidence before expanding the instruction set.

### P5 Integrated single-kernel prototype 22 to 30 hours

1. Couple the core and region memory models under one cycle-accounting convention.
2. Add required flash/bus service rules with parameter provenance.
3. Support detailed replay and hybrid execution of the same kernel.
4. Exercise E3–E5 and compare counters before comparing total time.
5. Publish a local report with cycles, traffic, assumptions and simulator cost.

**Gate:** one FC and one convolution path run through the complete pipeline, with proposed median hardware error ≤15% and no unexplained double-counted time. This is the first useful project release.

### P6 State-preserving acceleration 26 to 38 hours

1. Implement region capability checks and explicit fallback reasons.
2. Cache decoded blocks and validated dependency summaries.
3. Add restricted LRU warping for one regular stream.
4. Verify final cache state and timing readiness against unaccelerated replay.
5. Measure preprocessing, functional execution and replay separately.

**Gate:** exact accelerated paths match the reference under identical assumptions. Aim for at least 5× timing-backend speedup on long regular cases, while reporting end-to-end speedup separately. If speedup is marginal, keep simpler summaries and defer general warping.

### P7 ML runtime and memory integration 24 to 34 hours

1. Add TFLite graph inspection and kernel dispatch manifests.
2. Implement arena layout reproduction, scratch and persistent allocation accounting.
3. Add general conv, depthwise, pooling and necessary utility operators one at a time.
4. Model wrappers, requantization, boundary work and interpreter execution.
5. Run linked graphs with preserved state; report unsupported paths explicitly.

**Gate:** outputs match reference execution; allocation requirements match the pinned runtime where claimed exact; complete small graphs have consistent per-layer and total accounting.

### P8 Cache-bearing hardware extension 28 to 42 hours

1. Bring up M7 with confirmed cache geometry, MPU attributes and routing.
2. Establish cache-disabled, cache-enabled and TCM-placement experiments.
3. Add I/D cache models and a declared replacement-policy approximation where exact policy state is unknown.
4. Add only the necessary core pairing, outstanding-request and bus behavior.
5. Test E2, E7 and E8 with shapes near capacity/conflict transitions.

**Gate:** a cache-bearing board is validated, not merely a synthetic cache attached to an M4 timing model. Revise schedule if M7 timing dominates the work.

### P9 Held-out evaluation and packaging 24 to 34 hours

1. Freeze architecture parameters and code; select model hashes before final evaluation.
2. Evaluate at least 20 supported graph/configuration cases across at least three architecture families where feasible; identify synthetic versus real workloads.
3. Use unchanged MLPerf Tiny models where supported and fitting; publish exclusions rather than simplifying them silently. [R18]
4. Run ablations: bounds-only, analytical memory, full reference replay and hybrid.
5. Produce a CLI, reproducibility script, model cards and target-porting guide.
6. Recruit an external tester to reproduce at least one board/model result.

**Gate:** accuracy and coverage targets below pass, or the release is explicitly labeled a limited research prototype.

### P10 Research writeup and decision 10 to 14 hours

1. Explain what each fidelity level buys in speed and accuracy.
2. Report counterexamples and limits, not just aggregate averages.
3. Identify whether the contribution is a new method, an MCU adaptation, an open validation dataset, or a useful engineering tool.
4. Decide whether to add energy, improve M7 fidelity, or generalize symbolic analysis. Do not attempt all three simultaneously.

## 13 Validation plan and quantitative gates

These are proposed engineering targets, not guaranteed results or standards.

| Layer | Validation evidence | Proposed acceptance |
|---|---|---|
| Functional execution | Golden outputs and instruction semantics | Exact where specified; documented backend tolerance otherwise |
| Symbolic accesses | Differential comparison with tiny actual-kernel traces | Exact ordered addresses/counts within the claimed region domain |
| Deterministic cache backend | Hand traces and independent state replay | Exact classifications, traffic and final state |
| Approximate cache backend | Controlled geometry/layout sweeps | Report absolute miss-rate error in percentage points; initial target ≤2 points median, ≤5 p90 |
| Core timing | Controlled instructions and loops | Median absolute percentage error ≤10%; publish tails |
| Individual ML kernels | Shapes including boundary/fallback paths | Median latency error ≤15%; p90 ≤30% |
| Complete inference | Frozen unseen models, fixed target | Median latency error ≤15%; p90 ≤30%, separately per profile |
| Hybrid acceleration | Same workload versus unaccelerated reference | Exact where claimed; approximate latency delta ≤5% median, ≤10% p90 as an initial gate |
| Speed | Same host and cases including startup | ≥5× timing-backend speedup on long regular regions; disclose total pipeline cost |
| RAM/flash | Runtime allocation, map, stack measurement | Exact build-derived quantities; otherwise separate model error and unknown application overhead |

A low absolute miss-rate error can still be disastrous for a rare but expensive miss path. Also report absolute miss count, relative miss error when meaningful, traffic volume and cycle impact. Do not combine errors from different boards into one flattering median.

### Hardware protocol

1. Record silicon/core revision, clock tree, voltage, flash settings, caches/MPU, placement and build hashes.
2. Verify the timing source and counter rollover behavior; keep prints and host transport outside the timed window.
3. Start with 10 warm-ups and 100 repeated inferences over 3 rebooted sessions; increase repetitions only when necessary. Record raw samples, median and IQR.
4. Define cold execution through documented reset/maintenance steps; cache disabled is not equivalent to cold cache.
5. Measure layer instrumentation overhead and compare with a minimally instrumented complete Invoke window.
6. Validate counters only when the target actually exposes them. Do not assume MCU access to server-style L1/L2 PMU events; otherwise use controlled experiments and honest observability limits.
7. Diagnose systematic errors by mechanism before changing any parameter.

Hardware access can be owned, borrowed or supplied by a collaborator. Development without hardware is possible, but then the evidence is model-to-model agreement, not physical accuracy.

### Error attribution and ablation

For a failing case, check in order: executable identity; kernel path; instruction count; addresses; memory routing; hit/miss behavior; service time; dependencies/overlap; measurement contamination. Correct predictions can arise from compensating errors, so total latency alone is insufficient validation.

Maintain distinct suites for parameter characterization, development/debugging, regression tests and final held-out evaluation. Modifying the model after inspecting a held-out failure turns that case into development evidence; replenish the final suite.

## 14 Uncertainty without a fitted workload predictor

Uncertainty belongs to assumptions and missing hardware details, not necessarily a trained latency model.

- **Known constants:** documented geometry, configured frequency and ELF placement.
- **Characterized mechanisms:** experimentally bounded flash service, pipeline or bus parameters.
- **Approximation error:** analytical summary versus detailed reference.
- **Implementation uncertainty:** unknown replacement state or undocumented overlap.
- **Measurement noise:** run-to-run physical variation.

Use a parameter provenance record: value, units, revision, source section, experiment if any, allowed range and confidence label. A pure specification-only mode must retain unknowns; an optional characterized mode may use microbenchmarks to identify hardware properties, not per-workload latency coefficients.

Sweep plausible parameter/policy scenarios to report sensitivity ranges. These are not 90% confidence intervals unless a defensible probabilistic model has been established. Random-replacement seed variation is model variability, not proof of the distribution of silicon behavior. For a discrete choice such as replacement policy, report scenarios separately when useful.

Statistical coverage against unseen hardware runs can be studied later, but it should not conceal structural model uncertainty. No per-layer scale factors or correction tables fitted to final inference times.

## 15 Energy extension after performance validation

Maintain action counters now, but defer joules until component parameters have a defensible source.

Proposed decomposition:

```text
E_total = E_baseline_over_elapsed_time
        + sum(action_count_j × incremental_energy_j)
        + E_state_transitions
```

Define incremental energy relative to the baseline so it is not counted twice. Distinguish CPU instructions, SRAM operations, cache fills/writebacks, flash transactions and peripherals. Operand switching, voltage, frequency, temperature and clock gating can affect costs; a constant energy-per-MAC is not a general physical model.

Use the Accelergy separation of counts and component costs as an interface pattern, not an off-the-shelf MCU calibration. [R11] Characterize unknown costs with isolated physical experiments or publish an explicitly speculative design-space model. Public timing manuals alone generally do not determine trustworthy joules.

Validate energy by integrating measured voltage × current over a GPIO-marked window. State whether the measurement is board-input or isolated MCU-rail energy; account for regulators/debugger/LEDs accordingly. Batched inference windows can improve instrument resolution, but should not be presented as measured individual-layer energy.

## 16 Tool reuse and repository layout

| Component | Proposed starting point | Required feasibility check |
|---|---|---|
| Functional tracing | QEMU TCG plugins [R15] | Thumb/DSP semantics, exact memory map, callback completeness and practical harness support |
| Decode and build metadata | Pinned Arm compiler/binutils disassembly and ELF parser | Instruction aliases, symbol stripping, inlining and register dependencies |
| Algebra | Small explicit Python formulas; optional ISL later [R19] | Affine restrictions and actual symbolic counting cost |
| Cache reference | Small independently tested implementation | Target policies, writebacks, state serialization |
| Analytical experiments | HayStack and Kerncraft as research comparators [R3, R8] | Dependency versions, licenses and compatibility; not plug-and-play MCU models |
| Timing engine | Purpose-built small event scheduler | Scheduling semantics and accounting invariants |
| Larger simulator | gem5 only if a concrete need appears [R20] | Generic CPU/cache support is not validation for this Cortex-M board |

Suggested directories: `docs/research`, `docs/decisions`, `profiles`, `frontend`, `ir`, `models/core`, `models/memory`, `models/allocation`, `engine`, `kernels`, `firmware`, `experiments`, `tests`, `reports`.

Proposed commands, to be implemented:

```text
edge-sim inspect model.tflite
edge-sim build model.tflite --profile target.json
edge-sim trace firmware.elf --region invoke
edge-sim run firmware.elf --profile target.json --mode reference
edge-sim run firmware.elf --profile target.json --mode hybrid
edge-sim compare reference.json hybrid.json measured.json
```

A run manifest includes model, binary and profile hashes; frontend/model versions; input seed; initial state; selected fidelity per region; unresolved parameters; and the measurement or prediction boundary. Reuse cached summaries only when the relevant hashes and validity conditions match.

## 17 Risks and stop conditions

| Risk | Response |
|---|---|
| Emulator integration becomes a project of its own | Isolate kernel execution; stop broad peripheral emulation |
| Arbitrary-C symbolic analysis grows too large | Keep explicit kernel templates and trace fallback |
| Hidden flash/replacement behavior blocks exact modeling | Characterize mechanisms or expose sensitivity; reduce accuracy claims |
| M4 validates timing but not meaningful cache research | Keep synthetic cache tests and make M7 validation a distinct requirement |
| Hardware trend matches but counters are wrong | Debug intermediate events; reject compensating errors |
| Fast summaries cannot preserve state | Fall back at those boundaries; do not force analytical coverage |
| Analytical backend slower than replay on tiny models | Use size-aware selection and report break-even points |
| Physical parameters cannot be identified | Present scenario predictions, not calibrated certainty |
| Research novelty is unclear | Compare with AMM, Kerncraft and warping; claim an adaptation/tool unless evidence supports more |

**Potential research question:** How much trace processing can be removed from quantized MCU inference while preserving cache-state transitions and dependency-sensitive timing? Evaluate accuracy, end-to-end analysis cost and supported coverage together. Whether a publishable methodological contribution exists remains to be established.

## 18 Reading sequence and verified reference list

### Reading order with a concrete output

1. **R1 AMM:** draw its information flow and mark which inputs are trace-derived. Output: baseline assumptions sheet.
2. **R2 and R3:** work through one address-to-line/set example and one reuse-distance count. Output: ten hand-checkable cache tests.
3. **R8 then R7:** identify overlap and traffic interfaces. Output: target-specific cycle-accounting rules; no imported x86 constants.
4. **R5:** study state equivalence, warping conditions and limitations. Output: a narrowly scoped acceleration proof obligation.
5. **R4 and R13:** reconcile non-LRU theory with target policy evidence. Output: replacement-model decision and uncertainty labels.
6. **R14–R17:** audit the real instructions, runtime, allocator and tracing path. Output: minimal executable kernel and trace.
7. **R10–R11:** read later for representation and energy extensions, not as v1 dependencies.

### Core research

- **R1 — Chennupati, Santhi, Eidenbenz and Thulasidasan. 2017. An Analytical Memory Hierarchy Model for Performance Prediction.** Read Sections 3–4 and the scope of the evaluation. [Conference paper](https://www.informs-sim.org/wsc17papers/includes/files/068.pdf); [PPT source](https://github.com/lanl/PPT).
- **R2 — Ghosh, Martonosi and Malik. 1999. Cache Miss Equations A Compiler Framework for Analyzing and Tuning Memory Behavior.** Read program assumptions, memory-line/cache-set equations and conflict conditions before symbolic solution methods. [Author-hosted paper](https://mrmgroup.cs.princeton.edu/papers/toplas-ghosh.pdf).
- **R3 — Gysi, Grosser, Brandner and Hoefler. PLDI 2019. A Fast Analytical Model of Fully Associative Caches.** Read Sections 2–3, then experimental limitations. The arXiv posting is from 2020; the conference paper is 2019. [Paper](https://htor.inf.ethz.ch/publications/img/gysi-haystack.pdf); [HayStack source](https://github.com/spcl/haystack).
- **R4 — Sen and Wood. SIGMETRICS 2013. Reuse based Online Models for Caches.** Read locality definitions and replacement-policy models; use the authors' corrected copy. [Paper](https://research.cs.wisc.edu/multifacet/papers/sigmetrics13_caches_reuse.pdf); [author publication list with errata](https://pages.cs.wisc.edu/~david/includes/publications.html).
- **R5 — Morelli and Reineke. PLDI 2022. Warping Cache Simulation of Polyhedral Programs.** Read the running example, state/data-independence requirements, symbolic simulation and Section 7 limitations. [Extended paper](https://arxiv.org/pdf/2203.14845); [conference entry](https://pldi22.sigplan.org/details/pldi-2022-pldi/47/Warping-Cache-Simulation-of-Polyhedral-Programs).
- **R6 — Williams, Waterman and Patterson. Roofline.** Read operational intensity and bottleneck ceilings. [Berkeley technical report](https://www2.eecs.berkeley.edu/Pubs/TechRpts/2008/EECS-2008-134.html).
- **R7 — Hofmann, Eitzinger and Fey. 2015. Execution Cache Memory Performance Model Introduction and Validation.** Read single-core decomposition and overlap assumptions; separate principle from architecture-specific rules. [Paper](https://arxiv.org/pdf/1509.03118).
- **R8 — Hammer, Eitzinger, Hager and Wellein. Kerncraft A Tool for Analytic Performance Modeling of Loop Kernels.** Read Sections 2.3–2.4 on interchangeable performance/cache models. [Paper](https://arxiv.org/pdf/1702.04653); [source](https://github.com/RRZE-HPC/kerncraft).

### Tools and extension research

- **R9 — LLVM MCA documentation.** Read scheduling-model requirements, pipeline modeling and limitations. [Official documentation](https://llvm.org/docs/CommandGuide/llvm-mca.html).
- **R10 — Parashar et al. 2019. Timeloop A Systematic Approach to DNN Accelerator Evaluation.** Read workload/mapping/hardware separation. [Primary publication page](https://research.nvidia.com/publication/2019-03_timeloop-systematic-approach-dnn-accelerator-evaluation).
- **R11 — Wu, Emer and Sze. ICCAD 2019. Accelergy An Architecture Level Energy Estimation Methodology for Accelerator Designs.** Read action counts and component energy characterization. [Paper](https://accelergy.mit.edu/paper.pdf).

### Target and implementation evidence

- **R12 — ST STM32F446 documentation and RM0390.** Product evidence confirms the ART flash accelerator; retrieve and read the actual revision's flash, bus and clock chapters before extracting parameters. Full manual text was not successfully retrieved in this review. [Product](https://www.st.com/en/microcontrollers-microprocessors/stm32f446.html); [documentation](https://www.st.com/en/microcontrollers-microprocessors/stm32f446/documentation.html); [manual](https://www.st.com/resource/en/reference_manual/rm0390-stm32f446xx-advanced-armbased-32bit-mcus-stmicroelectronics.pdf).
- **R13 — ST AN4839 Level 1 Cache on STM32F7 and STM32H7 Series.** Read cache policy, memory routing and maintenance; use the selected MCU reference manual for exact device geometry. [Application note](https://www.st.com/resource/en/application_note/an4839-level-1-cache-on-stm32f7-series-and-stm32h7-series-stmicroelectronics.pdf).
- **R14 — Arm Cortex-M technical documentation.** Select the actual M4/M7 revision's TRM and programmer guide. Do not substitute another Cortex family's timing table. Full linked M4 PDF retrieval was unsuccessful here. [Official documentation index](https://developer.arm.com/community/arm-community-blogs/b/architectures-and-processors-blog/posts/cortex-m-resources).
- **R15 — QEMU TCG documentation.** Read callback semantics, successful memory accesses, instruction completion caveats and the explicit icount timing limitation. [Plugins](https://www.qemu.org/docs/master/devel/tcg-plugins.html); [instruction counting](https://www.qemu.org/docs/master/devel/tcg-icount.html).
- **R16 — TFLite Micro memory management.** Read persistent/temporary/head allocation and recording overhead. [Runtime documentation](https://github.com/tensorflow/tflite-micro/blob/main/tensorflow/lite/micro/docs/memory_management.md).
- **R17 — CMSIS-NN.** Read support matrix, selected wrapper/source, buffer-size API and quantization requirements for the pinned revision. [Official source](https://github.com/ARM-software/CMSIS-NN).
- **R18 — MLPerf Tiny.** Use original artifacts where supported; distinguish local validation from official benchmark submissions. [Official benchmark repository](https://github.com/mlcommons/tiny).
- **R19 — ISL.** Optional integer-set tooling for later symbolic generalization. [Project site](https://libisl.sourceforge.io/).
- **R20 — gem5 cache documentation.** Useful reference for explicit transactions and cache state, not proof of an accurate MCU target. [Classic cache documentation](https://www.gem5.org/documentation/general_docs/memory_system/classic_caches/).

## 19 First ten concrete tasks

1. Create the assumption/provenance ledger and a paper-comparison table.
2. Choose M4-first or cache-first M7 and retrieve its exact manuals.
3. Freeze one FC kernel, compiler and memory layout.
4. Compile and inspect ten tiny instruction/kernel tests.
5. Obtain a trustworthy functional trace for one test; verify addresses manually.
6. Write a small deterministic cache reference and same-set conflict tests.
7. Derive a symbolic stream and compare it with explicit accesses.
8. Implement a minimal dependency/resource timing scheduler.
9. Run the same FC case on hardware and compare intermediate counts and cycles.
10. Replace only one proven repeated region with an analytical summary, then measure accuracy and actual end-to-end speedup.

**First success criterion:** one deployed kernel, one target, one explainable prediction, and one analytical acceleration whose assumptions and state transition have been verified. Expand from that vertical slice.

## 20 Additional research: mathematical cache and shared-resource models

### 20.1 Falcon: the highest-priority additional cache paper

**Pitchanathan, Grover and Grosser, Falcon: A Scalable Analytical Cache Model, PLDI 2024.** Falcon decomposes large affine-program analyses into smaller local relationships rather than submitting the entire program as one large Presburger problem. Its evaluation includes 46 TorchVision networks. Study its structured representation, reuse-source analysis and threshold counting. Its cache abstraction is fully associative LRU; the hardware comparison approximates real set-associative caches, rather than proving that conflicts are absent. Multi-level extensions have specified write-policy assumptions. It is not a Cortex-M timing model. [R21]

**Proposed use:** retain the scalable decomposition idea, but make target set mapping and replacement policy explicit. First implement a tiny independently testable counting model. Only then decide whether the artifact or its integer-set machinery is worth integrating. Do not translate the complete paper before testing whether its assumptions fit the selected MCU.

The public artifact contains source, prebuilt tools and reproduction instructions. Its main cache entry point is `CacheModel::compute`, with reuse/dependence and threshold-counting components. The published archive is about 10 GB; full reproduction is a substantial undertaking. Start with one kernel, not its full benchmark workflow. This review verified the artifact page, not a separate maintained Falcon GitHub repository. [R22]

### 20.2 Analytical Performance Models for NoCs with Multiple Priority Traffic Classes

The supplied Mandal et al. paper models arbitration using discrete-time queueing, structural decomposition and effective service-time transformations. The important transferable idea is to replace detailed contention replay with equations derived from the resource's actual scheduling policy—not to fit end-to-end latency coefficients. [R23]

Its boundaries matter: the host simulation supplies traffic statistics periodically, and host-side throttling accounts for outstanding-request restrictions. Thus its low measurement dependence does not mean it derives every input from a topology description. Its reported comparisons concern network delay against simulation, not MCU inference latency against boards.

For this project, derive traffic from kernel accesses and target routing. Use queueing only where there are genuinely competing request streams: instruction fetch versus loads, writebacks versus demand reads, or DMA versus CPU traffic. A microcontroller bus matrix is not automatically a mesh NoC. Router-specific equations cannot be copied without establishing a matching arbitration, buffering and flow-control abstraction.

### 20.3 Follow-up arbitration research

- **Analytical Performance Modeling of NoCs under Priority Arbitration and Bursty Traffic** adds burst-aware traffic modeling. Read it before assuming geometric arrivals for tiled kernels, whose memory requests naturally occur in bursts. Its queue decomposition is a candidate technique, not evidence that independent steady-state arrivals describe a short inference phase. [R24]
- **Theoretical Analysis and Evaluation of NoCs with Weighted Round-Robin Arbitration** provides a closer reference when the actual interconnect uses weighted arbitration. Study effective service and arbitration assumptions before equations. Do not substitute WRR for an undocumented round-robin, priority or implementation-specific policy. The paper cites author code; this review could not verify that repository's availability. [R25]
- **Network Calculus: A Theory of Deterministic Queuing Systems for the Internet** provides an alternative when the desired result is a worst-case bound under arrival/service constraints. The NetCal DNC implementation is a useful executable reference. These are bounds, not estimates of expected execution time. [R26–R27]

### 20.4 Use the simplest adequate mathematical object

| Situation | First model to implement | What must be supplied | Escalation condition |
|---|---|---|---|
| Regular accesses, no contention | Exact line counts and documented service rules | Addresses, placement, width, latency/throughput | Stateful flash/prefetch or variable service |
| Regular reuse under a supported cache policy | Symbolic reuse/conflict conditions | Iteration domain, order, mapping, initial state | Unsupported replacement, branching or solver cost |
| Known finite FIFO request sequence | Deterministic completion recurrence | Arrival times, service times, initial availability | Reordering, priorities, multiple servers |
| Long phase with stochastic competing streams | Policy-matched queueing approximation | Rates, variability, dependence assumptions, buffers | Short bursts, high utilization or unstable closure |
| Required worst-case guarantee | Arrival/service curves and network calculus | Valid traffic envelope and service guarantee | Cannot justify an envelope or bound is unusably loose |
| Stateful irregular interaction | Request/event simulation | Explicit scheduling and state rules | Remains reference/fallback; optimize later |

For a single work-conserving, non-preemptive FIFO server with known arrivals, completion times satisfy

\[
b_n=\max(a_n,d_{n-1}),\qquad d_n=b_n+s_n,\qquad w_n=b_n-a_n.
\]

Here arrivals and service durations are in the same time units. This is exact under those assumptions, needs no physical measurements when inputs are known, and does not tick idle cycles. It still processes requests; it is not constant-cost closed-form evaluation. Derive batched/periodic forms where the sequence permits them. Priority scheduling, pipelining and multiple outstanding requests require additional state or different recurrences.

**Important feedback problem:** a CPU's memory-request rate depends on the stalls caused by those requests. An unstalled instruction trace does not provide valid wall-clock arrival rates for a stalled system. A proposed phase-level closure is:

\[
\lambda_k^{(j)}=N_k/T^{(j)},\qquad
T^{(j+1)}=(1-\eta)T^{(j)}+\eta F(\lambda^{(j)},\theta),\quad0<\eta\le1.
\]

This is an implementation proposal, not a result established by the cited NoC paper. `F` must include the actual dependency/overlap model; fixed-point convergence alone does not establish physical correctness. Check finite-buffer limits, saturation, burst assumptions and conservation of requests. Reject nonconvergence and invalid utilization instead of silently reporting a value. Start with deterministic recurrences and introduce this approximation only after a matching event reference exists.

## 21 Existing projects: what to study, reuse and avoid assuming

“Verified” below means the official repository/artifact page was accessible and its stated scope inspected during this review. It does not mean the code was built, tested, audited, or confirmed compatible with current dependencies. Pin revisions and check licenses before reuse.

| Project and link | Verified role | Relevance and limitation | Recommendation |
|---|---|---|---|
| [HayStack](https://github.com/spcl/haystack) | Analytical cache-miss computation | Closest compact starting point for symbolic cache analysis; fully associative abstraction and older research dependencies | Read and reproduce one small example |
| [Falcon artifact](https://zenodo.org/records/10972076) | Scalable analytical cache research implementation | Strong neural-network relevance; not an MCU timing engine; artifact rather than verified standalone GitHub project | Highest-priority newer paper; selective source study |
| [Kerncraft](https://github.com/RRZE-HPC/kerncraft) | Analytical loop-kernel performance toolkit | Useful separation of machine description, in-core work, traffic and performance composition; HPC assumptions and measured machine inputs may be involved | Architecture reference, not a drop-in target model |
| [PPT](https://github.com/lanl/PPT) | Performance-prediction toolkit associated with AMM work | Trace/reuse-based statistical modeling provides a comparison point; not purely specification-driven MCU modeling | Study interfaces and assumptions |
| [pycachesim](https://github.com/RRZE-HPC/pycachesim) | Access-level cache hierarchy simulation | Practical reference for cache traffic/state; not a pipeline or full-board model | Candidate oracle after policy checks |
| [OSACA](https://github.com/RRZE-HPC/OSACA) | Static assembly throughput/dependency analysis | x86/AArch64 scope does not supply Cortex-M support; analytical analysis can still depend on measured instruction parameters | Study dependency/resource representation |
| [NetCal DNC](https://github.com/NetCal/DNC) | Deterministic network-calculus analysis | Useful bound computation, not average inference latency | Optional bounds backend |
| [ASTRA analytical network](https://github.com/astra-sim/astra-network-analytical) | Analytical communication modeling | Distributed-AI network context, not MCU memory hierarchy | Interface/design precedent only |
| [Tenstorrent tt-npe](https://github.com/tenstorrent/tt-npe) | NoC performance estimation | Time-step/congestion modeling for different hardware; not a pure closed-form solution | Optional comparison, not core dependency |
| [Timeloop](https://github.com/NVlabs/timeloop) | DNN accelerator architecture/mapping analysis | Valuable workload/mapping separation; accelerator abstraction does not reproduce compiled MCU kernels | Study representation; defer integration |
| [Accelergy](https://github.com/Accelergy-Project/accelergy) | Architecture-level action-based energy estimation | Action counts need component energy values; does not supply unknown board constants | Energy v2 design reference |

Two qualifications prevent overstating the search:

1. The WRR paper points to an author GitHub repository; retrieval failed here. Treat it as a lead to investigate, not a confirmed reusable dependency. Likewise, no warping implementation was verified during this pass; the paper itself was reviewed.
2. A community repository, [analytical_latency_mode_NoC_ver2](https://github.com/Mags17/analytical_latency_mode_NoC_ver2), describes an implementation of earlier Ogras NoC analysis. It is not the authors' verified reference implementation. Use only after reproducing equations and checking tests independently.

**Conclusion:** there are strong reusable components and research artifacts, but this review did not identify a verified turnkey project that accepts TFLite int8 plus an MCU description and delivers specification-derived, dependency-aware latency and board energy. Build a narrow integration around proven component ideas rather than fork a large simulator without a target-fit audit.

## 22 Measurement-minimum contract and revised first implementation sequence

### Parameter acquisition hierarchy

For every parameter, record `value_or_range`, units, target/revision, operating conditions, provenance, and sensitivity. Distinguish these categories in the output:

1. **Architectural/specification input:** documented geometry, memory map, bus width, enabled cache configuration, clock and flash wait-state setting.
2. **Software-derived input:** instruction sequence, loop trip count, concrete layout, cache-line addresses, allocation lifetimes and dependency graph. Obtaining these from a compiler or functional execution is not measuring physical performance.
3. **Mechanistically derived quantity:** miss count under stated policy, service demand, arbitration delay and critical-path bound.
4. **Unknown implementation detail:** undocumented overlap, arbitration, replacement state or flash behavior. Represent explicit scenarios/ranges or declare unsupported. Do not hide the uncertainty in a fitted per-layer coefficient.
5. **Optional isolated characterization:** a small experiment identifying one otherwise unavailable physical parameter. Keep this separate from workload fitting; report when it was used.
6. **Validation observation:** held-out board results used to assess the model, not automatically to tune it. If they drive a model revision, freeze a new version and use fresh holdouts.

**Default promise:** zero end-to-end workload fitting and no mandatory per-op timing dataset. **Do not promise:** accurate arbitrary-board timing and energy from incomplete datasheets alone. Mathematical sophistication cannot identify an unprovided physical constant.

### Six tightly scoped implementation increments

This sequence refines the early work in Sections 12 and 19; it is not an additional mandatory six-stage project. If all research artifacts are reproduced, add a separate optional 24–48-hour study budget and reassess after the first build attempt.

**I1 — Exact workload and address algebra.** Freeze a scalar or DSP FC kernel, toolchain and memory placement. Derive iteration counts, affine accesses, boundary conditions and quantization epilogue instructions. Use tiny executable tests only to check functional counts/addresses. Deliver: `KernelIR`, hand-derived cases, and equality against explicit enumeration. No hardware purchase is needed.

**I2 — Mathematical memory models.** Implement contiguous-line counting, strided line/set mapping and one precisely defined LRU reuse model. Add compulsory/conflict/capacity stress cases, alignment sweeps and initial-state handling. Deliver: exact agreement with a small access-level oracle for every supported synthetic case. Unsupported cache policies must fail explicitly or choose a labeled fallback.

**I3 — Dependency and resource algebra.** Derive compute lower bounds, recurrence bounds and documented issue/resource constraints for the kernel. Add deterministic request-completion recurrences for one memory service path. Deliver: cycle accounting for small dependency chains, independent streams and overlapping compute/memory; separate service latency from initiation interval. Avoid using peak MAC throughput as realized throughput.

**I4 — Stateful composition.** Carry relevant cache/buffer/resource state across tiles and layers. Add only the real target's shared-resource scheduling. Compare equations against an independent event-level reference on finite sequences before steady-state queueing. Deliver: evidence that fewer events produce the same supported-state transition, not just similar aggregate misses.

**I5 — First real target validation.** Obtain access to one physical board through purchase, borrowing or a collaborator. Run a small discriminating suite, then held-out kernels. Keep configuration and ELF fixed. When mismatches appear, investigate counts, mapping, issue rules and stalls separately. Deliver: residual explanations and a parameter-provenance ledger, not an automatically fitted correction factor.

**I6 — Expand only after the gate.** Add convolution/depthwise/pooling, actual optimized kernel dispatch, allocator behavior and warm-cache transitions. Add a second MCU only after first-target assumptions are explicit. Deliver: supported-domain report, fallback frequency, analysis runtime and held-out latency accuracy. Energy starts only when its physical coefficients and measurement boundary are specified.

### Validation without owning every board

Synthetic tests establish mathematical correctness under a model. Comparing with an independent simulator establishes implementation consistency under its assumptions. Neither establishes silicon accuracy. You need eventual physical access to each target you claim is validated, but you do not personally need to buy every board. Publish a reproducible validation firmware and result schema so contributors can supply measurements. Label unmeasured target descriptions as unvalidated; do not transfer a Cortex-M4 result to another vendor's M4 memory system.

### What to implement yourself versus borrow

Implement the initial address/counting rules, target service rules and state interfaces yourself: these are small enough to understand fully and encode the project's actual assumptions. Borrow an integer-set library only when the symbolic domain outgrows hand-written cases. Use existing cache engines and analytical artifacts as independent references where possible. Independence matters: agreement between two wrappers around the same erroneous routine is weak validation.

## 23 Theory study plan and additional references

Each study unit should end in executable mathematics, not only notes. Suggested order:

| Unit | Read | Implement or derive before proceeding |
|---|---|---|
| A: Affine accesses and cache geometry | R2; address-generation sections of R3 | Enumerated versus symbolic line/set counts for FC and a tiled convolution |
| B: Reuse analysis and scalable counting | R3, then R21 | Exact toy LRU reuse distances; partition a two-statement loop into local reuse relations |
| C: Architectural timing composition | R7–R9 | Resource bound, recurrence bound and a finite deterministic service recurrence |
| D: Queueing and arbitration | R23, then R24 or R25 according to target | One-queue finite sequence; priority/WRR oracle; approximation error versus burst length and utilization |
| E: Bounds rather than mean estimates | R26–R27 | One justified arrival envelope/service curve; distinguish bounded delay from mean delay |
| F: Fast stateful evaluation | R5 | One provably legal repeated-state skip and adversarial tests that reject illegal skips |
| G: Energy | R11 | Action inventory with units, baseline/dynamic separation and explicit unknown coefficients |

Do not treat all units as prerequisites for v1. Units A–C plus target documentation are enough for a deliberately narrow initial model; D–F are selected by demonstrated bottlenecks and target behavior. NoC theory is useful, but can become an unnecessary detour on a single-core configuration with little competing traffic.

### Additional primary references

- **R21 — Pitchanathan, Grover and Grosser. 2024. Falcon: A Scalable Analytical Cache Model.** Read Sections 2–4 for representation/analysis and Section 5 for the distinction between mathematical cache assumptions and hardware validation. [Paper](https://grosser.science/static/aae77ad842c423d9b3fbe5e7337ae38d/pitchanathan-2024-falcon-a-scalable-analytical-cache-model.pdf); [DOI](https://doi.org/10.1145/3656452).
- **R22 — Falcon research artifact, version 4, April 2024.** Source organization and reproduction instructions. [Artifact](https://zenodo.org/records/10972076). Inspect requirements before downloading or running the full evaluation.
- **R23 — Mandal, Ayoub, Kishinevsky and Ogras. Analytical Performance Models for NoCs with Multiple Priority Traffic Classes.** CASES/TECS 2019; revised arXiv preprint 2020. Read assumptions, structural transformations and the host-simulator traffic interface. [Primary preprint](https://arxiv.org/abs/1908.02408).
- **R24 — Mandal et al. 2020. Analytical Performance Modeling of NoCs under Priority Arbitration and Bursty Traffic.** Read traffic assumptions and moment-based decomposition. [Primary preprint](https://arxiv.org/abs/2007.13951).
- **R25 — Mandal et al. Theoretical Analysis and Evaluation of NoCs with Weighted Round-Robin Arbitration.** Original preprint 2021; revised version 2023. Read effective service-time derivation, workload assumptions and evaluation boundary. [Primary preprint](https://arxiv.org/abs/2108.09534).
- **R26 — Le Boudec and Thiran. Network Calculus: A Theory of Deterministic Queuing Systems for the Internet.** Study arrival curves, service curves and delay/backlog bounds before network composition. [Authors' book repository](https://github.com/leboudec/netcal).
- **R27 — NetCal DNC.** Executable deterministic network calculus. [Official repository](https://github.com/NetCal/DNC).

**Updated project principle:** derive from code and architecture first; use state simulation where derivation is unsuitable; characterize only genuinely missing parameters; validate predictions independently on hardware. The first goal is one small model whose predictions can be explained equation by equation—not broad hardware coverage.
