# Implementation plan — design for review

**Implementation status, 27 September 2026:** the user authorized I1 setup and the first C++ slice after reviewing the FC contract. The source algebra, independent host checks and ARM analysis build now exist; see [the build report](i1-build-report.md). Earlier design-only authorization notes below record the scope at decision time and are superseded for this bounded I1 work. Actual DSP execution and binary-access equivalence remain open; cache/timing implementation is not started.

## 1. Requirements and authority

The updated `research-plan.md`, including sections 20–23, has been reviewed. Section 22's I1–I6 sequence governs implementation where it refines sections 12 and 19. Start with specification-derived mathematics; tracing infrastructure, event scheduling, and board bring-up are not prerequisites for the first algebra exercise.

| Requirement | Consequence |
|---|---|
| Derive from code and architecture | Trace each equation to an explicit schedule or resource rule |
| Minimize physical measurement | Zero end-to-end workload fitting; no mandatory per-op timing dataset |
| Detailed simulation only where necessary | Independent reference or explicitly labeled fallback |
| Explain assumptions, units, and sources | Versioned contracts and provenance; unknown is never silently zero |
| Start with one int8 FC kernel | One pinned CMSIS-NN path, batch one, static dense tensors, no graph parser |
| Learn incrementally | Derivation, tiny executable example, independent check, then next mechanism |
| Defer energy and multiple boards | Synthetic configurations first; eventual validation on one physical target |
| Review before implementation | This phase contains documentation only |

Separate workload representation from hardware evaluation. Symbolic addresses and enumerated accesses can both feed mathematical models. Functional emulation and event replay do not establish silicon accuracy. Classify outputs as **exact under stated abstractions**, **approximate**, **measured**, or **unsupported** (research section 4).

## 2. Decision record

Closed by the user on 26 September 2026:

1. **Kernel identity:** target a pinned production CMSIS-NN int8 fully connected kernel. A project-owned scalar kernel is no longer the implementation target. Release v8.0.0 was subsequently accepted below; full commit, entry point, helper path, and quantization options remain to be recorded.
2. **Quantization semantics:** preserve the selected CMSIS-NN implementation's exact offsets, multiplier/shift behavior, rounding, saturation, activation clamp, and relevant build options. The earlier artificial power-of-two contract is retired.

3. **First core/board (closed by user after documentation review):** NUCLEO-L4R5ZI (standard non-P), Cortex-M4 DSP, using internal flash/SRAM. Selected after memory and documentation reviews; supersedes F446RE. The user explicitly closed this decision after reviewing [the microarchitecture audit](l4r5-microarchitecture-audit.md). Recorded timing gaps remain implementation proof obligations, not resolved physical facts. Exact build fit and timing profile remain unverified. See [memory audit](benchmark-memory-and-board.md).

4. **Simulator language:** C++ is the primary implementation language, including the eventual instruction frontend/execution machinery. Python is optional for analytical memory/cache experiments, independent reference models, and analysis. This supersedes the earlier Python-first implementation proposal. The user's C++ experience, performance goals, and computational-science portfolio objectives motivate the choice; no admissions outcome is implied.
5. **Target compiler:** Arm GNU Toolchain using `arm-none-eabi-gcc`; release 15.2.rel1 subsequently accepted below. Record its bundled compiler/binutils identities when provisioning. The host C++ simulator uses a separate native compiler, not the bare-metal cross-compiler.
6. **Optimization policy:** one fixed optimized target build, with `-O3`, no LTO, and the target flags accepted below. Debug builds are separate evidence and do not supply optimized-build timing counts.
7. **Initial harness:** direct call to the selected production CMSIS-NN FC kernel. TFLite Micro integration comes later for whole-model evaluation.
8. **Build system:** CMake, with separate host simulator and MCU cross-build configurations.
9. **Development environment:** WSL2 with Ubuntu 24.04 LTS is the primary environment for the host C++ simulator, module tests, and target cross-builds, using native GCC/G++ 13 and C++20. These details were accepted on 27 September 2026. Selecting WSL does not authorize installation, repository migration, or simulator implementation. Board flashing/debug transport remains a later setup choice.

### Software baseline accepted on 27 September 2026

| Component | Accepted choice |
|---|---|
| Development environment | WSL2, Ubuntu 24.04 LTS; eventual working checkout in the Linux filesystem |
| Host simulator | C++20, native GCC/G++ 13; CMake with separate host and MCU configurations |
| Kernel library | CMSIS-NN v8.0.0, int8 FC Cortex-M4 DSP path |
| MCU toolchain | Arm GNU Toolchain 15.2.rel1, `arm-none-eabi-gcc` |
| MCU optimization | `-O3`, LTO disabled; normal compiler inlining allowed |

Accepted target compilation flags:

```text
-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
-O3 -g3 -ffunction-sections -fdata-sections -fno-lto
```

Use `-Wl,--gc-sections` when linking; retain a linker map and disassembly. All linked components must use a compatible ABI. The FPU/ABI settings do not imply floating-point computation in the int8 kernel. Explicitly override CMSIS-NN's default `-Ofast` through its optimization setting and inspect effective build commands. Avoid globally applying `-fno-builtin` or `-ffreestanding` to the kernel because they can impair CMSIS-NN memory-copy optimizations.

These choices are accepted design inputs, not a tested build. Before freezing I1's execution contract, record the full CMSIS-NN commit and dependency revisions, host package versions, target archive checksum and compiler/binutils versions, quantization macros, complete commands, and linked artifacts. Build and check numerical correctness before claiming this pairing works in the project. Host optimization must not change predicted target counts. Installation, migration, and implementation remain pending authorization.

Selection references: [Ubuntu lifecycle](https://ubuntu.com/about/release-cycle), [Ubuntu G++ 13 package](https://packages.ubuntu.com/noble/g%2B%2B-13), [CMSIS-NN v8.0.0 documentation](https://github.com/ARM-software/CMSIS-NN/blob/v8.0.0/README.md), and [Arm GNU releases](https://gitlab.arm.com/tooling/gnu-toolchains-for-arm). Versions are a reproducible baseline, not a requirement to track the newest release.

Use module-level directed artificial tests for mathematical and implementation correctness. Final evaluation uses a separately selected and frozen standard benchmark suite, preserving supported models and actual dispatch. Directed tests do not replace standard benchmarks or silicon validation. A full standard model needing additional operators remains unsupported until those operators are modeled; do not present an extracted FC layer as complete-model validation.

| Open decision | Proposed direction | Required before |
|---|---|---|
| Build verification and remaining pins | Record full source/dependency revisions, package versions and archive hashes; select quantization options and verify the accepted build | I1 compiled mapping |
| Unknown physical details | Reasonable explicit assumptions authorized; record rationale, alternatives and sensitivity; unsupported where unjustified | Numerical timing claims |
| Benchmark execution details | Pin datasets/preprocessing/runtime/licenses and case membership; freeze physical accuracy threshold before inspecting held-out timings | Benchmark evaluation |

Core/board selection and documentation review are closed by user direction; unresolved timing details remain tracked. See [benchmark memory and board decision](benchmark-memory-and-board.md); the earlier [M4 feasibility note](cortex-m4-feasibility.md) is historical candidate analysis.

### Benchmark and validation contract accepted on 27 September 2026

Use the four audited MLPerf Tiny workloads (anomaly detection, image classification, keyword spotting, visual wake words) at repository commit `4addd0fa08d216e20637637874e084895f289da4`. Model identities and SHA-256 hashes are recorded in [the inventory](benchmark-memory-inventory.json). This is a pinned workload subset, not complete newer-suite coverage or an official MLPerf submission. Streaming wakeword and the larger image model remain outside the initial scope.

Begin real-workload evaluation with supported FC cases extracted from those models, alongside small module-wise directed tests. Preserve source model/operator identity, shapes, weights, bias, quantization parameters and relevant input tensors. Record the isolated case's actual layout and entry state; extracted FC results do not validate complete inference. Add whole-model evaluation only as required operators and runtime integration become supported. Before full-model evaluation, pin dataset versions, preprocessing, licenses, runtime/build artifacts and supported dispatch paths.

Directed tests cover hand-worked FC outputs/addresses, DSP blocking boundaries and tails, rounding/saturation, line crossings, same-set conflicts, explicit initial states and, later, dependency/resource composition. Keep physical directed tests small and discriminating; no per-operator latency dataset or fitted correction factors.

Hold out distinct FC shape/layout groups initially, then whole workloads where practical. Freeze membership before inspecting held-out hardware timings, and group near-duplicate execution paths rather than randomly splitting them. Input-value changes alone may not exercise different timing behavior. If a holdout drives a model revision, it becomes development evidence for that revision; obtain fresh holdouts for a new independent claim. Report each of the four workloads individually and avoid broad statistical claims from this small collection.

Require exact numerical agreement with the supported pinned int8 semantics, and exact agreement with independent references for quantities claimed exact under the abstraction: counts, ordered addresses/widths/types, cache events/outgoing state and defined dependency/resource calculations. This does not establish silicon accuracy.

For hardware timing, compare matching binaries, layouts, entry conditions and interval boundaries. Define signed percentage error for positive measured cycles `C_i` as `e_i=100*(predicted_C_i-C_i)/C_i`. Report per-case predicted/measured cycles, signed and absolute percentage errors, median absolute percentage error, worst-case absolute percentage error, measurement variability, supported-case coverage and explicit exclusions. Report cold and repeated execution separately.

Freeze the physical accuracy acceptance threshold and model version after a small development-only characterization stage but before inspecting held-out timing results. No numerical timing threshold is selected yet; do not choose it retrospectively to pass final results. Dataset pins, exact case/holdout membership and the threshold remain follow-up obligations, while this policy and workload selection are decided. No implementation or environment setup is authorized by this decision.

### Physical execution profile accepted on 27 September 2026

| Setting | Accepted production baseline |
|---|---|
| CPU/HCLK | Fixed 120 MHz |
| Internal voltage configuration | Range 1 boost |
| Flash latency | Five wait states |
| Flash organization | Dual-bank; verify option bytes and record each object's bank |
| ART instruction and data caches | Enabled |
| Instruction prefetch | Enabled |
| Entry conditions | Separately specified controlled-cold and repeated execution |
| Timing boundary | Direct FC call, including helpers and any packing performed inside it |
| Concurrent activity | No scheduled interrupts, DMA/competing application transfers, RTOS scheduling, logging/host communication, or flash programming/erase during the timed region |

Source: RM0432 Rev 9, section 3.3.3 Table 12 and section 3.3.4, as inspected in the [microarchitecture audit](l4r5-microarchitecture-audit.md). Convert cycles with `T_seconds=C_cycles/120000000`. Five flash wait states are not a per-instruction surcharge; buffers, caches, prefetch and overlap determine visible stalls. Changing frequency and wait states can change cycle counts as well as seconds.

Cold execution requires a defined reset/preparation and entry sequence: entry code can populate caches and buffers before the kernel starts. Repeated execution preserves state across a specified invocation sequence; it does not assume all accesses hit. Pin the warm-up count and intervening harness work before validation. Claim steady state only after checking relevant state or behavior has stabilized. Application-context entry after other operators is deferred.

Exclude input transfer, external buffer initialization, cache preparation and output checking from the kernel interval. Match simulator and measurement boundaries and account for timing instrumentation. During board setup, record the clock-source/PLL recipe, option bytes, effective register settings, exact reset/entry sequence, core/silicon revision and applicable errata. These procedures and the resulting hardware behavior are not yet verified. Features-disabled configurations remain later directed diagnostics, not the production baseline. The accepted synthetic I2 cache still has no prefetch; target ART modeling is a separate specialization. No simulator or firmware implementation is authorized by this profile decision.

### First cache contract accepted on 27 September 2026

Start I2 with configurable **set-associative LRU**, following the user's correction to the fully associative proposal. Let `S` be the positive number of sets, `W` the positive ways per set, and `L` the positive line size in bytes. Data capacity is `S*W*L` bytes, excluding metadata. Map byte address `A` by `line=floor(A/L)`, `set=line mod S`, and `tag=floor(line/S)`. Fully associative (`S=1`) and direct-mapped (`W=1`) configurations are special cases for testing, not separate prerequisite milestones.

Use exact LRU independently within each set. Hits and newly filled lines become most recently used; use an invalid entry before evicting a valid least-recently-used line. Start empty by default, and support explicit valid contents and per-set LRU order. Initially support reads only, complete-line allocation on misses, and no prefetch. Split a read spanning lines into ascending-address line references. Reject writes and unsupported policies explicitly. Report line-reference hits/misses, evictions, and final state; timing remains deferred.

For a previously referenced line, `D_set` counts distinct intervening other lines mapping to its set: `hit iff D_set < W`. First references need explicit initial-state treatment. Verify same-set thrashing despite spare capacity elsewhere, different-set independence, `D_set=W-1` versus `W`, hit-driven LRU updates, cold/warm states, cross-line reads, and outgoing contents/order against independent replay. Analytical formulas and replay must use independent counting/state logic.

This is a synthetic mathematical contract. Modulo set mapping and complete-line fills are not established ART facts. Specific test geometries can be chosen for hand-checkable cases; physical ART geometry, buffers and partial fills remain a separate model specialization. SRAM traffic is not automatically routed through this flash-cache abstraction. I1 still precedes I2; no implementation is authorized by this decision.

### Undocumented behavior policy accepted on 27 September 2026

The user permits reasonable assumptions for missing microarchitectural details, including replacement-policy updates and prefetch behavior. Prefer documented rules and never contradict them silently. For each assumed rule, record its rationale, affected mechanisms/outputs, plausible alternatives, and validation status. Assess sensitivity where it can materially affect predictions. Missing documentation alone does not block progress or reopen board selection.

Analytical/reference agreement may be exact under this declared abstraction while the resulting physical prediction remains approximate. Do not label scenario ranges as proven bounds or confidence intervals. Retain unsupported status when no defensible assumption is available. This policy does not authorize per-operator fitting, change the accepted memory placement, or start simulator implementation; physical measurements remain minimal and focused on later validation.

### Memory placement accepted on 27 September 2026

| Object or rule | Accepted first-kernel contract |
|---|---|
| Code, weights, constant bias | Internal flash |
| Input, output, required scratch | Separate static buffers in SRAM3 |
| Stack and ordinary harness globals | SRAM1 |
| SRAM2 | Reserved initially |
| Main tensor buffer alignment | 16 bytes, subject to checking the selected kernel's requirements |
| Buffer overlap | Disallowed |
| Allocation during the kernel | None |
| Directed test layouts | Explicit, disjoint synthetic address ranges |
| Production addresses | Extract from the matching ELF/linker map; freeze layout with the binary |

Select regions through explicit linker sections rather than assigning permanent numeric addresses to every object. Derive scratch requirements from the selected CMSIS-NN path; do not assume scratch is required. I1 uses only the actual buffers and does not reserve the later provisional 256 KiB runtime arena. Arena allocation and lifetime reuse remain deferred to runtime integration.

For an allocation ending at exclusive byte address `E`, the next base aligned to `a` bytes is `a * ceil(E/a)`. Account for padding, check per-bank capacity, reject overlapping ranges and arithmetic overflow, and verify placement against linked artifacts. A 16-byte-aligned tensor base does not imply every row is aligned. Production fit and kernel alignment compatibility remain unverified until build inspection; choosing separate banks is not evidence of contention-free or parallel service.

Code/weight relocation to SRAM can be a later directed experiment, not the baseline. Any such experiment must distinguish setup/copy cost from the kernel interval. The accepted physical profile above specifies clock/flash policy and entry modes; exact setup/entry procedures and physical timing rules still require verification. Placement acceptance does not authorize firmware or simulator implementation.

### Language and execution-model boundaries

Implement the initial `KernelIR`, workload/address algebra, and module tests in C++. Mathematical models do not require Python; Python can be used selectively to derive or independently check them. Keep interfaces in terms of explicit requests, counts, assumptions and state. Do not require an embedded Python interpreter or per-access language crossings for I1; the eventual integration mechanism remains a later decision.

Fetch/decode, instruction semantics, completion and architectural-state updates belong in C++ if needed by a later functional or timing reference. Keep their functional behavior separate from timing/resource rules. Execution and retirement are execution-model concepts, not merely input parsing; do not infer physical pipeline stages from software module names. Any retirement bookkeeping must be justified by the selected core abstraction, not imported from an unrelated out-of-order processor model.

Language choice does not change the mathematics-first I1–I6 sequence. Begin with FC equations and ordered regions; add instruction machinery only when a concrete validation or unsupported-path need calls for it. A full ISA emulator or cycle-by-cycle pipeline is not an I1 prerequisite. Preserve the option to obtain functional execution through an existing verified frontend rather than writing a full decoder immediately.

Independent checks may be written in either C++ or Python. Independence comes from separate logic and evidence, not language choice. C++ counters/address calculations need checked arithmetic and declared integer domains so host overflow cannot silently invalidate mathematical results.

## 3. First milestone: I1 for a production CMSIS-NN FC kernel

The [first source-derived FC contract](fc-kernel-contract.md) now pins CMSIS-NN commit `13c97dbb6f781d4aab38ed34e6e441f42b79aff4`, identifies the two-row/four-column DSP schedule and zero caller scratch, and proposes the default two-stage requantization domain. It includes a worked numerical/address example and a [source hash manifest](fc-source-manifest.json). Review it before implementation; effective macros, full build dependencies, binary mapping and kernel execution remain unverified. Temporary arithmetic and event-count checks are documentation verification, not completion of I1.

**Deliverable:** one pinned kernel/build/layout, a small `KernelIR`, hand-derived work/address equations, and independent checks including the real quantization epilogue. No timing engine, full firmware tracer, or board purchase is required for the initial algebra.

Proceed in small pieces: select and inspect the exact CMSIS-NN path; derive its loop regions and quantization behavior; encode the algebra and tiny independent tests; check the compiled mapping. The selected path may pack data, process multiple outputs together, or use DSP instructions. Do not reuse the former scalar schedule's traffic equations without proving them for that path.

### Equations and derivation obligations

For batch-one dense FC with `N` inputs and `M` outputs, the semantic MAC count remains `N*M`. With int8 input/weights/output and a present int32 bias, logical tensor storage is `N + N*M + 5*M` bytes. This excludes scratch, metadata, alignment gaps, and stack. Neither formula determines executed instructions or accessed bytes.

A semantic template, to specialize against the pinned source, is:

`acc[o] = bias[o] + sum_i((x[i] + input_offset) * W[o,i])`

`y[o] = clamp(requantize_exact(acc[o], multiplier, shift) + output_offset, activation_min, activation_max)`.

This is not a definition of the requantization routine. Its rounding, intermediate widths, overflow domain, and build-dependent options must be taken from the pinned implementation before coding the oracle. Likewise, any optional bias or quantization variant must have an explicit supported contract.

For each actual loop region with block width `k`, derive full groups `floor(N/k)` and tail `N mod k`; do not select `k` until source inspection. Derive output blocking separately. Count packing/scratch, bias, quantization parameters, loop control, and entry/exit work as applicable.

For a contiguous row-major logical layout, element addresses are `X+i`, `W0+o*N+i`, `B0+4*o`, and `Y+o`. These locate tensor elements; they do not specify the order or widths of DSP loads. Actual accesses must come from the selected schedule and compiled mapping, including widened loads, stack/spills, and helper calls. Specialize dimensions before affine analysis.

### Proposed interfaces (not implemented)

```text
KernelManifest(CMSIS_NN_commit, entry_point, helper_paths, build_options)
FCSpec(dimensions, offsets, quantization_parameters, activation_limits)
Layout(buffer_bases, extents, alignment, memory_regions)
BuildManifest(compiler_version, flags, ISA, ELF_hash, disassembly_hash)
derive_fc(kernel, spec, layout) -> KernelIR
count_work(ir) -> WorkSummary
enumerate_ir(ir) -> ordered_accesses                 # tiny tests only
reference_case(kernel, spec, layout, values) -> outputs, observed_accesses
```

`KernelIR` contains specialized domains, ordered regions/statements, access widths/types/addresses, dependencies, boundary/tail paths, epilogue behavior, assumptions, and provenance. `WorkSummary` separates semantic work, dynamic instructions where established, logical storage, and requested access bytes. Cycles, physical traffic, cache misses, and energy remain unsupported in I1.

The test reference must calculate its events independently of the analytical address evaluator/count formulas. Functional outputs should be checked against the pinned kernel and independently established golden cases. If binary accesses cannot be independently observed yet, label the source-level evidence honestly and leave the binary equivalence gate open; matching counts alone does not prove access order.

### Meaningful tests and gate

- Hand-worked tiny output and address cases under the actual kernel semantics.
- Directed dimensions immediately below, at, and above actual input/output blocking widths; include bias, packing, and tails.
- Positive/negative rounding boundaries, offsets, saturation/clamp limits, and supported multiplier/shift extremes; no invented tolerance.
- Ordered address/width/type equality against independent enumeration, plus separate total-count checks.
- Alignment and memory-range boundary cases; reject malformed shapes, unsupported options, and invalid arithmetic domains explicitly.
- Audit pinned disassembly for instruction classes, helpers, loop/epilogue paths, widened accesses, and spills. Track source versus binary evidence separately.

I1 completes when the source/build/layout and numerical semantics are pinned and all claimed exact counts/addresses pass independent checks. Unsupported paths stay explicit. Explain each derivation before adding another mechanism. Module tests establish correctness under the model; later standard benchmarks and physical observations assess broader validity and accuracy.

## 4. Subsequent increments from section 22

| Increment | Deliverable | Gate |
|---|---|---|
| I2 — Memory mathematics | Contiguous-line counts, strided line/set mapping, one defined LRU model, explicit initial state/write policy | Exact agreement with independent access replay on supported cases, including final state |
| I3 — Dependencies/resources | Resource/recurrence bounds and deterministic service recurrence for one path | Checked dependent chains, independent streams, startup/drain, and compute/memory overlap; latency distinct from initiation interval |
| I4 — Stateful composition | Cache/buffer/resource state across regions and relevant shared-resource scheduling | Equations match finite independent event replay in counters and outgoing state before queueing approximations |
| I5 — One-target validation | Small discriminating suite, then held-out kernels with fixed ELF/configuration | Mechanistic residual explanations and provenance; no fitted layer corrections |
| I6 — Gated expansion | More kernels, dispatch, allocation, warm transitions; later second target | Supported domain, fallback frequency, analysis cost, held-out accuracy |

I2 starts with byte-to-line/set algebra. For `n>0` contiguous bytes at byte address `A`, line size `L>0`, and `S>0` sets:

`line(A) = floor(A/L)`; `set(A) = line(A) mod S`

`lines_touched = floor((A+n-1)/L) - floor(A/L) + 1`.

This counts touched lines, not misses. Split accesses crossing line boundaries. For the accepted `W`-way set-associative LRU model, `D_set` counts distinct intervening lines in the same set; a repeated reference hits iff `D_set<W`. Initial contents require explicit treatment. Test `D_set=W-1`, `D_set=W`, cold/warm states, alignment, capacity and same-set conflicts. Unsupported replacement/write policies fail explicitly in the first contract; LRU and modulo set mapping are not automatically exact physical rules.

I3 derives bounds from actual dependencies/resources:

`resource_bound = max_r(required_occupancy_r / capacity_r)`

`recurrence_bound = max_dependency_cycles(total_latency / iteration_distance)`.

With per-iteration demand and compatible units these bound cycles per iteration; they are not realized throughput. For a single work-conserving, non-preemptive FIFO server with known arrivals `a_n`, service durations `s_n`, and initial availability `d_0`:

`b_n = max(a_n, d_(n-1))`; `d_n = b_n + s_n`; `w_n = b_n - a_n`.

Use one timebase. This recurrence is exact only under its service contract. It does not determine CPU arrivals independently of stalls. Model dependency feedback; unstalled traces do not supply stalled wall-clock arrival rates. Convert cycles to seconds only with declared frequency: `T=C/f_Hz`. Never charge service once in instruction cost and again as a stall penalty.

I4 adds queueing only for genuinely competing streams and matching policy/buffer assumptions. Finite deterministic sequences come first. Acceleration must preserve outgoing state, including pending requests/readiness where relevant; equal miss counts are insufficient. Event simulation remains an independent reference/fallback, not the default evaluator.

## 5. Evidence and study cadence

Each parameter carries `value_or_range`, units, target/revision, operating conditions, provenance/source location, and sensitivity. Preserve section 22's six categories: specification input, software-derived input, mechanistically derived quantity, unknown implementation detail, optional isolated characterization, and validation observation.

I1 constants derive from the pinned CMSIS-NN contract; compiled facts require pinned artifacts. Physical unknowns later receive explicit scenarios/ranges or unsupported status. Scenario ranges are not statistical confidence intervals. If holdouts drive a revision, freeze a new version and obtain fresh holdouts. Synthetic agreement cannot establish silicon accuracy or transfer validation between targets.

Follow section 23 just in time: A for addresses/geometry, B for LRU reuse, C for dependencies/service algebra. D–F require a demonstrated contention or acceleration need; G (energy) stays deferred. Study Falcon selectively; downloading its full artifact is not a prerequisite. Section 21's projects are candidates, not installed/reproduced dependencies: pin revisions and check licenses before reuse. No new literature review or external artifact reproduction was performed in this documentation update.

## 6. Review checkpoint

Kernel identity, quantization policy, board/documentation review, software baseline, memory-placement contract, first synthetic set-associative LRU contract, physical execution profile, and benchmark/validation contract are closed by the user. Full artifact identities, quantization options, alignment compatibility, concrete linked layout, and build verification remain obligations before freezing I1. Exact board setup and cold/repeated entry procedures must be recorded and verified before reproducible timing experiments. The four benchmark model artifacts are selected; dataset/runtime pins, holdout membership and the physical accuracy threshold remain to be frozen under the accepted policy. After implementation is authorized, start with the hand-worked example and explain its algebra before extending it. Do not begin memory or timing infrastructure in this documentation task.
