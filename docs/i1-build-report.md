# I1 first implementation and build report

27 September 2026. The user authorized build setup and the first C++ slice. This report records source correctness and static binary inspection, not board execution or timing accuracy. I1's binary-equivalence gate remains open.

## Implemented

- `include/edge/fc.hpp` and `src/fc.cpp`: validated FC specification/layout, compact pair/group/tail KernelIR, constant-cost analytical counts, bounded tiny-case event enumeration and checked integer numerical evaluation using the pinned default rounding equations.
- `tests/fc_tests.cpp`: hard-coded golden output/event sequence, independent pointer-stepping event oracle, independent magnitude/remainder rounding oracle and semantic row-dot-product checks.
- `firmware/`: direct pinned CMSIS-NN call, target flags and explicit linker placement in an analysis ELF. No simulator timing, startup, board flashing, or peripheral setup.
- `scripts/`: pinned local dependency setup, build/test workflow, effective macro and complete preprocessor dependency hashing, compiler/link commands, map/disassembly/symbols and stack-usage capture.

KernelIR is deliberately a source-schedule representation. Region order is pair regions followed by an odd row, each containing bias, groups, tails and epilogue. Numerical dependencies are executed in the evaluator; there is no generic instruction dependency graph or timing engine. Layout bases are configurable synthetic inputs; extents are derived from dimensions. Physical region capacity enforcement is currently provided by the analysis linker, not the generic synthetic layout validator.

## Environment and pins

Existing WSL2 `Ubuntu` distribution: Ubuntu 24.04.3 LTS, x86-64. Host GCC 13.3.0 package reports `Ubuntu 13.3.0-6ubuntu2~24.04`; CMake 3.28.3. No system packages were installed. The original Windows checkout remains in place.

CMSIS-NN commit: `13c97dbb6f781d4aab38ed34e6e441f42b79aff4`. Original six inspected source hashes are checked by the recorder; additional compiler/header dependencies are captured in the generated manifest. No external CMSIS-Core checkout was needed by this selected build.

Target compiler: Arm GNU Toolchain 15.2.Rel1 (Build arm-15.86), GCC 15.2.1 20251203. Binutils: 2.45.1.20251203. Downloaded official Linux x86-64 archive SHA-256: `597893282ac8c6ab1a4073977f2362990184599643b4c5ee34870a8215783a16`. This records downloaded artifact identity, not an independently verified publisher signature. Bootstrap checks this hash on subsequent runs.

The mounted Windows filesystem emitted clock-skew warnings during Make; compilation and tests completed. The initial compiler extraction was slow and an early cross-build could not find cc1; the completed project-local extraction resolved that failure. Prefer a Linux-filesystem checkout for subsequent development. No source migration or deletion was performed.

## Verification

The sanitizer-enabled Debug and optimized Release host builds both passed CTest and produced the same example outputs and counts. Checks cover the 16-event golden schedule and `[2,-3,6]` output; 35 shape combinations around four-column/two-row boundaries; independent numerical and event comparisons; signed rounding with power-of-two and non-power-of-two multipliers and supported shift endpoints; malformed dimensions, overlap, alignment, target-address overflow, accumulator overflow, invalid activation bounds, tensor extent mismatch and enumeration-limit rejection. Checks use explicit exceptions rather than assertions that disappear in Release.

The example reports 15 semantic MACs, 35 logical bytes, 40 source tensor-request bytes and 16 tensor events. These exclude parameter loads, stack/spills and instruction fetches. No source schedule is claimed to equal machine access order.

## Static ARM findings

The fixed target build uses `-O3`, no LTO, hard-float Cortex-M4 flags, and preserves upstream's GCC unrolling pragma. Compile-time guards and the recorded macro dump establish the DSP path, absence of MVE and absence of both alternate rounding macros. The generic helper retains its nonzero-filter-offset branch in the binary; our call selects zero filter offset at runtime. Unused size-getter code is removed by section garbage collection.

For the recorded build:

| Symbol | Address | Size |
|---|---|---:|
| `probe_entry` | 0x08000000 | 180 bytes |
| `arm_fully_connected_s8` | 0x080000b4 | 118 bytes |
| `arm_nn_vec_mat_mult_t_s8` | 0x0800012c | 3612 bytes |
| bias | 0x08000f50 | 12 bytes |
| weights | 0x08000f60 | 15 bytes |
| input | 0x20040000 | 5 bytes |
| output | 0x20040010 | 3 bytes |

These differ from the illustrative contract addresses. Each rebuild must use its own map. ELF size reports 4028 text bytes, zero initialized data bytes and 4112 BSS/NOLOAD bytes (including a 4096-byte stack reservation and alignment). This is not full application footprint.

At 0x0800014a the helper branches to the zero-offset path at 0x08000866. At 0x080008de it loads bias row 1 before row 0 at 0x080008e0: a concrete compiler reordering relative to the source-event abstraction. The block code contains four-byte `ldr.w` operations, `sxtab16`/`sxtb16` lane expansion and `smlad`. Scalar tails use signed byte loads. The generated code has unrolled bodies, stack-relative loads/stores, and `smlal` in requantization (e.g. 0x08000b88). These are static observations, not executed instruction counts or unaligned bus-transaction predictions.

Compiler `.su` files report static local frame use: 152 bytes for the probe, 96 for the FC entry, 120 for the vector/matrix helper. Their direct nested chain suggests 368 bytes, excluding exception entry and any future startup/harness changes; this is not a measured stack high-water mark. Caller scratch is still zero: stack is a different resource.

Initial ELF SHA-256: `3ae6669d6abe586ce892078aff61cde8d0e90fbcdabe58a583baab08e0fbdde9`; disassembly SHA-256: `62111bdc5e149dfeb446f5fefa5b3a699423d5b6c1db40a985a33028b3a5af78`. The generated `build-arm/manifest.json` is authoritative for subsequent builds; paths/debug metadata can change hashes across checkouts.

## Remaining before completing I1

Review the small C++ algebra and independent checks. Independently execute the actual DSP binary in an appropriate functional environment or on the board and compare numerical outputs. Establish the compiled access mapping, including wrapper/epilogue and stack traffic, before claiming exact binary accesses. Configure and verify unaligned-access settings for the odd-stride case at eventual board bring-up. Static inspection alone does not close those gates.

Do not begin cache or timing implementation merely because the source example passes. The first slice is usable for workload/source-address learning now, while compiled execution evidence remains explicitly incomplete.
