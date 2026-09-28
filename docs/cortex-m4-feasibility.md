# Cortex-M4 target feasibility â€” decision still open

Superseded for board selection by [the benchmark memory audit](benchmark-memory-and-board.md): NUCLEO-L4R5ZI is now selected. The F446 discussion below records the earlier candidate review and is not the active board decision.

Evidence checked 26 September 2026. Kernel identity (CMSIS-NN) and exact kernel quantization are closed decisions; Cortex-M4/NUCLEO-F446RE is a recommendation for review, not an approved purchase or frozen target.

## Documentation and modeling boundary

The public [Arm Cortex-M4 r0p1 TRM](https://documentation-service.arm.com/static/5fce431be167456a35b36ade) is accessible. Read sections 2.2â€“2.3 for processor/interfaces, 3.3 for instruction timings and their conditions, 3.4â€“3.5 for memory/write-buffer behavior, and chapter 9 for DWT. It describes a three-stage pipeline. Instruction timing tables assume zero-wait-state memory; they are not complete board latency predictions. Match the actual core revision before adopting constants.

Use the Armv7-M Architecture Reference Manual (referenced by that TRM) for architectural semantics, alongside the pinned compiled kernel. Neither an ISA description nor a nominal instruction-cycle table determines every dependency/fetch interaction in a deployed build.

For the chip, use [STM32F446 documentation](https://www.st.com/en/microcontrollers-microprocessors/stm32f446/documentation.html): RM0390 for memory map, bus matrix, clock tree, flash wait states and ART; the device datasheet for limits; ES0298 errata for revision-specific exceptions. RM0390 was located, but full PDF retrieval failed in this check. Its precise service/arbitration details have not been audited or converted into simulator constants.

The [STM32F446RE product specification](https://www.st.com/en/microcontrollers-microprocessors/stm32f446re.html) lists an M4 with DSP/FPU, up to 180 MHz, 512 KB flash and 128 KB SRAM. It includes a vendor ART flash accelerator. Model that path explicitly rather than treating it as a generic LRU data cache covering SRAM. Synthetic cache validation remains separate from physical validation of this MCU.

The [UM1724 board manual](https://www.st.com/resource/en/user_manual/dm00105823-stm32-nucleo-64-boards-mb1136-stmicroelectronics.pdf) covers board revisions, power, debug, and connectivity. Record the actual board/silicon revisions and configuration rather than inheriting assumptions from a generic M4 profile.

Assessment: sufficient public material exists to begin a narrow model, but a complete microarchitecture audit is still needed. Before closing the target, build a ledger of the kernel's used instruction classes, result latency/issue occupancy, memory routes, fetch/branch rules, and source sections. Unknown rules remain explicit; characterize only the unresolved mechanisms that materially affect predictions.

## Proposed validation progression

1. Directed software tests check numerical outputs, ordered accesses, tails, and epilogue paths for the pinned kernel.
2. On one board, use small dependency/load/store/branch cases to distinguish competing architectural hypotheses. These are mechanism checks, not operator latency regression data.
3. Validate DWT CYCCNT availability/enablement and rollover, counter-read overhead, and timing boundaries. The Arm manual documents CYCCNT but implementation presence must be checked. Keep logging outside the measured region; avoid breakpoints during timing. Use a verified platform timer if needed.
4. First study a controlled SRAM placement, explicitly accounting for shared instruction/data paths; SRAM execution is not automatically stall-free. Then validate the actual flash/ART placement as a separately recorded configuration.
5. Freeze predictions and test held-out FC cases. Later evaluate a frozen standard benchmark suite when its complete operator set is supported. Do not call extracted FC cases whole-model benchmark validation. Check memory fit before selecting the board for the full suite.

Pin ELF, source/compiler flags, clocks, memory placement, flash settings, input seed and numerical correctness. Disable unrelated interrupts/DMA in the timed region. Keep warm and cold conditions separate; collect repeated observations without fitting correction factors. If validation drives model changes, replenish holdouts.

Basic cycle validation needs no power profiler or instruction-trace probe. The [Nucleo product page](https://www.st.com/en/evaluation-tools/nucleo-f446re.html) documents on-board ST-LINK and USB power/debug connectivity. A suitable USB data cable and host are sufficient for initial programming and result collection; verify the connector against the supplied board revision.

## Availability snapshot

Assuming purchase in India, [element14 India](https://in.element14.com/stmicroelectronics/nucleo-f446re/dev-board-arduino-mbed-nucleo/dp/2491978) showed 1,683 in stock, a quantity-one price of Rs.1,753.690, and delivery estimates of 3â€“5 business days from Singapore stock or 4â€“6 from UK stock. This is a listing snapshot, not a delivered quote; taxes, shipping and postcode eligibility need checkout confirmation. No purchase was made.

Assessment: acquisition appears straightforward; building and checking the timing model is the harder part. Board availability alone does not establish that the eventual standard benchmark suite fits its 128 KB SRAM. Keep the core/board decision open until the user reviews documentation sufficiency and this capacity constraint.
