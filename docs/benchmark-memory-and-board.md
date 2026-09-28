# Benchmark memory audit and first-board decision

Checked 26 September 2026. This is a source/artifact audit, not a firmware build or hardware validation. No simulator implementation or purchase was performed.

## Board selection (closed after user review)

Select **NUCLEO-L4R5ZI (standard non-P variant)** as the first physical target: Cortex-M4 with DSP, up to 120 MHz, 2 MiB internal flash, and 640 KiB internal SRAM. This supersedes the F446RE candidate while retaining the M4 core direction. Use internal memory only initially; no SD card or external RAM is required by the selected memory strategy. Exact clock, flash settings, compiler, runtime revisions, and linker placement remain profile-freeze tasks, not closed timing assumptions.

Evidence: [ST MCU specification](https://www.st.com/en/microcontrollers-microprocessors/stm32l4r5zi.html), [device datasheet, section 3.5](https://www.st.com/resource/en/datasheet/stm32l4r5zi.pdf), and [Nucleo MB1312 manual](https://www.st.com/resource/en/user_manual/dm00368330-stm32-nucleo144-boards-mb1312-stmicroelectronics.pdf). MLPerf Tiny's [reference README](https://github.com/mlcommons/tiny/blob/4addd0fa08d216e20637637874e084895f289da4/benchmark/README.md) identifies STM32L4R5ZI as its reference platform. That is integration evidence, not proof of our future build's memory use or timing accuracy.

## Benchmark scope and evidence

The user accepted the four conventional MLPerf Tiny workloads at the snapshot below on 27 September 2026: anomaly detection, image classification, keyword spotting, and visual wake words. Begin with supported extracted FC cases and directed tests; these full graphs are future targets, not currently supported simulator inputs. The [accepted validation contract](implementation-plan.md#benchmark-and-validation-contract-accepted-on-27-september-2026) defines holdouts, exact correctness checks and hardware error reporting. Dataset/runtime pins, exact holdout membership and the physical accuracy threshold remain follow-up work.

Repository snapshot: `mlcommons/tiny` commit `4addd0fa08d216e20637637874e084895f289da4`. Downloaded six model artifacts, measured their byte lengths, and recorded SHA-256 hashes in [the inventory](benchmark-memory-inventory.json). For the four initial models, independently reconstructed the reference C byte arrays and verified that their hashes match the downloaded TFLite artifacts. No datasets or full toolchain were downloaded. Model downloads remain temporary; the permanent evidence is the manifest and immutable source URLs.

| Workload | Exact model bytes | Model KiB | Reference reserved arena | F446RE with 128 KiB SRAM |
|---|---:|---:|---:|---|
| Anomaly detection | 276,976 | 270.48 | 10 KiB | Arena budget fits; total build not verified |
| Image classification / small ResNet | 98,496 | 96.19 | 100 KiB | Only 28 KiB left before other RAM; not verified |
| Keyword spotting | 53,936 | 52.67 | 200 KiB | Reference arena exceeds total SRAM |
| Visual wake words | 333,288 | 325.48 | 200 KiB | Reference arena exceeds total SRAM |

One KiB is 1024 bytes. Model bytes include the FlatBuffer, not linked firmware. Arena numbers are `kTensorArenaSize` allocations in the reference source, **not measured minimum RAM requirements**. The build may reserve more than it needs, and a different TFLM/CMSIS-NN revision can need different scratch/persistent space. We have not proved that a carefully optimized F446RE implementation is impossible; we reject it as the default because it would make memory optimization a prerequisite.

Arena sources at the pinned commit: [anomaly detection](https://github.com/mlcommons/tiny/blob/4addd0fa08d216e20637637874e084895f289da4/benchmark/reference_submissions/anomaly_detection/submitter_implemented.cpp), [image classification](https://github.com/mlcommons/tiny/blob/4addd0fa08d216e20637637874e084895f289da4/benchmark/reference_submissions/image_classification/submitter_implemented.cpp), [keyword spotting](https://github.com/mlcommons/tiny/blob/4addd0fa08d216e20637637874e084895f289da4/benchmark/reference_submissions/keyword_spotting/submitter_implemented.cpp), [visual wake words](https://github.com/mlcommons/tiny/blob/4addd0fa08d216e20637637874e084895f289da4/benchmark/reference_submissions/person_detection/submitter_implemented.cpp).

The snapshot also contains a larger image model (512,024 bytes) and streaming wakeword model (74,520 bytes). Their CMSIS-NN arena requirements are unknown here. [MLPerf Tiny v1.3 includes streaming wakeword](https://mlcommons.org/2025/09/mlperf-tiny-v1-3-tech/); do not describe the four-model target as complete latest-suite coverage. The streaming reference uses an ST AI runtime and a separate audio interface setup, so its memory cannot be transferred to our CMSIS-NN profile. These extensions remain outside the accepted subset and require a separate scope decision.

## Memory management plan

Keep three different resources separate:

- **Flash:** read-only model/weights plus program code and initialized-data load images.
- **SRAM:** activations, scratch, runtime metadata, stack, mutable globals, and communication buffers.
- **Host storage:** the dataset and result logs. Transfer one test input at a time, outside the kernel timing interval. A whole dataset need not fit on the board.

Build and flash **one benchmark at a time**. Do not sum the four arena reservations. Keep the model as a flash-resident constant and verify the linker map does not copy it into SRAM. Register only required operators without removing model operations.

The L4R5 SRAM is banked: SRAM1 is 192 KiB at `0x20000000`, SRAM2 is 64 KiB at `0x10000000` (also aliased at `0x20030000`), and SRAM3 is 384 KiB at `0x20040000`. An alias is not extra capacity. Proposed later linker placement: a 16-byte-aligned arena in SRAM3, stack/globals/communication in SRAM1, with SRAM2 initially reserved. A provisional 256 KiB arena budget in SRAM3 exceeds the largest inspected 200 KiB reservation and leaves 128 KiB in that bank. This is an engineering budget, not a measured requirement or guaranteed fit.

The inspected [reference linker script](https://github.com/mlcommons/tiny/blob/4addd0fa08d216e20637637874e084895f289da4/benchmark/reference_submissions/streaming_wakeword/sww_ref_l4r5zi/STM32L4R5ZITX_FLASH.ld) lists all banks but routes ordinary `.bss` into 192 KiB SRAM1. Merely buying a 640 KiB board does not ensure the arena uses all its memory: create an explicit arena section and check its map address. Do not modify firmware in this design phase.

Fit conditions for the eventual build are:

`flash_used = linked_code_and_rodata_including_model + initialized_data_load_images <= flash_capacity`

`RAM_used[bank] = nonoverlapping_allocations_in_bank + stack/heap_reserve <= bank_capacity`.

Count allocations once: TFLM commonly owns scratch/input/output inside its arena. Do not add them again unless the build allocates separate copies. Verify allocation/preparation peaks, successful invocation, numerical outputs, stack high-water mark, and linker-region overflow on the pinned build. Exact whole-application fit remains **unverified**; the user has closed board selection after the [microarchitecture review](l4r5-microarchitecture-audit.md), whose technical gaps remain tracked. [TFLM allocation documentation](https://github.com/tensorflow/tflite-micro/blob/main/tensorflow/lite/micro/docs/memory_management.md)

### First-kernel placement accepted on 27 September 2026

For the direct FC harness, place code, weights and constant bias in flash; separate static input/output/required scratch buffers in SRAM3; stack and ordinary globals in SRAM1; reserve SRAM2 initially. Use 16-byte main tensor alignment subject to kernel requirements, disallow overlapping buffers, and perform no allocation during the kernel. Obtain production addresses from the matching ELF/map and use disjoint synthetic layouts for directed tests. Scratch size comes from the selected kernel path.

This closes the placement policy for I1, not compiled fit or timing. The provisional 256 KiB arena above concerns later whole-model integration; I1 allocates only its required buffers. See the [accepted contract](implementation-plan.md#memory-placement-accepted-on-27-september-2026).

## Why not a memory card?

An SD card is storage accessed through a controller; it does not enlarge the CPU-addressable tensor arena. It can hold datasets or model files, but data must still be transferred to usable memory. Streaming weights or spilling activations would require a different kernel/runtime schedule and explicit I/O timing. That would complicate both the production-kernel contract and validation.

External SRAM/PSRAM is a different option: appropriate hardware/controllers can provide more working memory, but introduce new latency, routing and contention rules. Do not add it for the first target when internal SRAM has sufficient planning headroom. The MCU datasheet distinguishes SRAM, external-memory interfaces, and SDMMC; these are not interchangeable memory levels.

## Availability and documentation follow-through

The [element14 India listing](https://in.element14.com/stmicroelectronics/nucleo-l4r5zi/dev-board-32bit-arm-cortex-m4/dp/2845537) retrieved during this check showed 60 units, Rs.3,315.960 each, and 4–6 business days from UK stock. Confirm postcode, stock, shipping and tax at checkout. Use the standard non-P board to keep the first profile fixed; no purchase has been made.

Use Arm Cortex-M4 documentation plus **RM0432**, the L4R5 datasheet/errata, and **UM2179/MB1312** for the board. Do not reuse F446 RM0390 flash/bus constants. The L4R5 datasheet was retrieved and SRAM placement checked; RM0432 was subsequently downloaded and inspected directly; see the microarchitecture audit for verified details and unresolved timing rules. Hardware selection does not certify documentation completeness or silicon accuracy.
