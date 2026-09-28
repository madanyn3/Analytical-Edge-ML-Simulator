# L4R5 microarchitecture documentation audit

Checked 26 September 2026; assumption policy updated 27 September 2026. **Review status: resolved by explicit user direction after this audit; NUCLEO-L4R5ZI is selected.** Detailed public specifications exist. The technical gaps below remain unresolved physical facts. The user now authorizes reasonable assumptions for undocumented behavior, including replacement and prefetch details; complete documentation is not a prerequisite to proceeding with an explicitly approximate physical model. Kernel and quantization decisions remain closed.

## Retrieved primary documents

- [ST RM0432](https://www.st.com/resource/en/reference_manual/rm0432-stm32l4-series-advanced-armbased-32bit-mcus-stmicroelectronics.pdf), Rev 9, June 2021, 2,300 pages. Successfully downloaded from ST despite web-preview failures. SHA-256: `64357720fbe198e01b10a868022b374c72394494dc38e2bbabe345cfd58545a0`.
- [Arm Cortex-M4 TRM](https://documentation-service.arm.com/static/5fce431be167456a35b36ade), r0p1, `100166_0001_04_en`, 2020, 107 pages. SHA-256: `7415b77eeff5d0bfa1f8f43eb4fe3e97b22b34fe76baed21d0b791f3a238eb94`.
- [ST ES0393](https://www.st.com/resource/en/errata_sheet/es0393-stm32l4rxxx-and-stm32l4sxxx-device-errata-stmicroelectronics.pdf), Rev 10, March 2022. Accessed through the web tool; review applicable silicon revision and errata before freezing a profile.

These are the revisions actually retrieved, not a claim that no newer version exists. Relevant RM memory/flash sections and Arm core/timing/interface sections were inspected; representative timing/cache pages were also rendered and checked. This was not a page-by-page audit of every peripheral.

## What is specified

| Mechanism | Source location | Verified coverage |
|---|---|---|
| CPU timing | Arm TRM 3.3.1–3.3.3, pp. 3-30–3-38 | Instruction cycle tables, load/store overlap, address-dependency stall, unaligned access penalties |
| Core memory interfaces | Arm TRM 2.3, 3.4–3.5 | Address routing, system-bus fetch pipeline, write buffer |
| MCU routing/arbitration | RM0432 2.1, pp. 87–92 | Master/slave topology; round-robin BusMatrix policy |
| Flash service settings | RM0432 3.3.3, Table 12, p. 122 | Wait-state requirements versus HCLK and voltage range |
| ART behavior | RM0432 3.3.4, pp. 123–126 | Current/prefetched buffers, sequential timing diagram, bank-dependent read width, I/D cache capacities, I-cache LRU, DCode priority |
| Revision exceptions | ES0393 applicability and system errata | Revision identifiers and flash/cache-related limitations |

For example, RM0432 specifies 1 KiB instruction-cache and 256-byte data-cache storage, with 64-bit dual-bank or 128-bit single-bank flash reads. It states LRU for instruction replacement and describes data caching as working similarly. These are flash-path caches, not a general SRAM cache. At 120 MHz in range-1 boost, Table 12 requires five flash wait states. Its zero-wait-state performance description is a CoreMark comparison, not universal access latency.

## Gaps that prevent an unconditional pass

1. **Exact branch/fetch timing:** Arm's refill parameter spans one to three cycles depending on alignment, instruction width and early speculation. The reviewed sections do not provide a complete selection procedure for every preceding state. A cycle range is not a deterministic predictor.
2. **Complete ART transitions:** ST supplies useful policy and buffer descriptions, but the inspected sections do not fully resolve sub-line fill/validity and replacement-update details for every interleaving of hits, misses and prefetches. Do not infer whole-row fill transactions simply from row capacity. The branch-miss description gives a minimum penalty, not a universal equality.
3. **Arbiter implementation:** round-robin alone does not specify every grant-update, initial-state and burst/stall interaction needed for arbitrary cycle-exact contention. DCode-over-ICode flash priority must also be reconciled with the BusMatrix model.
4. **Deployment identity:** actual core/silicon revision, bank mode, clock/voltage configuration, enabled accelerators, kernel binary and addresses are not pinned yet. Documentation availability cannot close these facts.

These are unresolved in this audit, not proof that no vendor document can resolve them. Arm also identifies an Integration and Implementation Manual in its product documentation; its availability and usefulness for the missing rules were not verified. Even access to that manual would not by itself specify ST's flash controller.

## Modeling policy updated by the user on 27 September 2026

- For the selected compiled FC path, inventory every used instruction class, dependency pattern and memory route.
- Attach a source section and executable rule to each required timing/state transition. Distinguish fixed values, conditional rules and bounds.
- Use additional vendor documentation when available; otherwise adopt reasonable, explicit assumptions consistent with known behavior. Record rationale, affected outputs, plausible alternatives and validation status. No vendor contact was made in this task.
- Check sensitivity to assumptions that can materially change predictions. Scenario differences are not statistical confidence intervals or proven physical bounds. If an assumption cannot be justified, retain an unknown or unsupported result rather than inventing a cost.
- A narrower execution configuration remains an option where useful; SRAM execution or disabling prefetch does not automatically remove core fetch/dependency uncertainties. Preserve the accepted flash-code/weights baseline unless a scope change is agreed.
- Keep characterization optional and focused, and validate against held-out board results later. This authorization permits mechanistic assumptions, not per-operator latency regressions or fitted correction factors.
- Exact agreement with a reference model establishes correctness under the stated assumptions, not exact hardware behavior. Do not call the target documentation-complete simply because missing rules have been modeled.

No silicon-accuracy claim is established by this audit. A documented model still requires independent directed checks and held-out board validation. The user-approved board selection must not be represented as proof of complete documented timing.
