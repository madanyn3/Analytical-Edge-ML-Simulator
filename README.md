# Analytical Edge-ML Performance Simulator

[![ci](https://github.com/madanyn3/Analytical-Edge-ML-Simulator/actions/workflows/ci.yml/badge.svg)](https://github.com/madanyn3/Analytical-Edge-ML-Simulator/actions/workflows/ci.yml)

This project will estimate edge-ML execution costs using mathematical models derived from kernel code and architecture. The C++ source-algebra slice reproduces a pinned CMSIS-NN FC example, counts work and enumerates small source tensor schedules. Synthetic memory models include set-associative LRU, configurable blocking cache hierarchies, pipelined FIFO memory service, analytical regular-stream acceleration and conditional roofline bounds. A strict config parser and direct-stimulus testbench exercise these modules without a CPU frontend. Validated hardware timing predictions do not exist yet.

We will learn and implement the theory in small steps, beginning with one pinned CMSIS-NN int8 fully connected kernel and its exact quantization semantics. The first milestone derives operation counts and ordered byte addresses, including the quantization epilogue, in a small `KernelIR`. Module-level directed tests check the algebra; a separately frozen standard benchmark suite will evaluate supported workloads. Memory and architectural timing models follow separately.

The selected first physical target is **NUCLEO-L4R5ZI**, using internal flash and SRAM. The user has closed the [documentation review](docs/l4r5-microarchitecture-audit.md); its remaining timing gaps are tracked for implementation. The [benchmark memory audit](docs/benchmark-memory-and-board.md) explains this choice and separates reference arena budgets from verified build requirements.

The simulator will use **C++20 with native GCC/G++ 13**, with optional Python analytical experiments and independent checks. CMake will manage separate host and MCU builds. The accepted target baseline is **CMSIS-NN v8.0.0 with Arm GNU Toolchain 15.2.rel1**, using `-O3` and no LTO. The [implementation plan](docs/implementation-plan.md#software-baseline-accepted-on-27-september-2026) records target flags and the remaining artifact pins and build-verification obligations.

## Working principles

WSL2 with Ubuntu 24.04 LTS is the primary development environment. The initial build used existing GCC 13/CMake and project-local target dependencies from the Windows-mounted checkout; the repository has not been migrated. A checkout inside the Linux filesystem is preferable for extraction/build speed and avoiding mounted-filesystem timestamp skew.

- Prefer code- and architecture-derived models; do not fit per-operator latency regressions.
- Minimize physical measurements and use them for focused validation.
- Introduce an independent access/event reference only when needed to test an analytical model.
- Make assumptions, units, sources, uncertainty, and unsupported cases visible.
- Defer energy modeling and multi-board support.

Logical tensor storage, executed memory accesses, and physical memory traffic are different quantities. The first milestone will report only the quantities it can derive. It will not claim board latency, TFLite Micro arena size, whole-application fit, or hardware accuracy.

## Design documents

- [Project roadmap](docs/roadmap.md): current status, phased plan (analytical FC cache theorem, Thumb-2 frontend, M4/ART timing, board validation, whole-model, energy) and timeline.
- [SVG visual study guide](docs/visual-guide.md): nine detailed diagrams covering cache mathematics, hierarchy flow, memory timing, acceleration, configuration and direct testbench modes.
- [Hierarchy and service contract](docs/hierarchy-service-contract.md): research interpretation, exact timing equations, acceleration proofs and assumption ledger.
- [Configuration and direct testbench](docs/configuration.md): strict knob schema, cache/memory stimulus and runnable examples.
- [Cache and memory mathematics](docs/memory-models.md): research, equations, supported cases, independent tests and conditional roofline bounds.
- [First FC kernel contract](docs/fc-kernel-contract.md): pinned DSP source path, exact rounding equations, a 5-input/3-output worked example, ordered tensor accesses and review gates.
- [I1 build report](docs/i1-build-report.md): completed checks, compiled findings, limitations and next steps.
- [Implementation plan](docs/implementation-plan.md): requirements, open decisions, equations, proposed interfaces, tests, and review gates.
- [Research plan](docs/research-plan.md): research rationale and constraints. Sections 20–23 refine the earlier roadmap; section 22 defines the implementation sequence.
- [Project instructions](AGENTS.md): durable rules for contributors and coding agents.

The sequence is workload/address algebra → mathematical memory models → dependency/resource algebra → stateful composition → one-target hardware validation → gated expansion. Detailed simulation is an independent reference or explicitly selected fallback. No end-to-end workload fitting or mandatory per-operator timing dataset is required. Synthetic agreement does not establish silicon accuracy.

## Build and run in WSL

Prerequisites: Ubuntu 24.04 x86-64, `g++-13`, CMake >=3.22, Make, Git, curl, tar and Python 3. Run from the repository in a WSL shell:

```sh
bash scripts/bootstrap.sh  # downloads pinned CMSIS-NN and Arm GNU 15.2.rel1 into .deps
bash scripts/build.sh      # host tests/example, ARM analysis ELF, artifact manifest
```

For the host-only slice (no downloads):

```sh
cmake -S . -B build -DCMAKE_CXX_COMPILER=g++-13 -DCMAKE_BUILD_TYPE=Debug -DEDGE_SANITIZE=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/fc_example
./build/memory_example
./build/edge_bench configs/hierarchy.ini
./build/edge_bench configs/memory-latency.ini
./build/edge_bench configs/memory-bandwidth.ini
./build/edge_bench configs/hierarchy-directed.ini --trace configs/reads.csv
./build/edge_bench configs/cluster-4x4-mesh.ini   # synthetic tiled cluster (version 2)
```

Expected example: output `2 -3 6`, 15 MACs, 35 logical bytes, 40 requested tensor bytes, 16 source events. Firmware outputs live in `build-arm`: ELF, map, disassembly, symbols, effective macros and `manifest.json` with versions/commands/dependency hashes. The ELF is an analysis harness, **not flashable board firmware**: it has no startup/vector table or clock/cache initialization. It has not been executed on Cortex-M4. `ARM_TOOLCHAIN_ROOT` can point to an extracted copy of the same toolchain; unset it to use `.deps` (reconfigure CMake with `--fresh` when switching compilers).
