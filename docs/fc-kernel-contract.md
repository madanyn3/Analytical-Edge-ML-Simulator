# First FC kernel contract — source derivation for review

27 September 2026. This document records the first theory/source-inspection milestone. The user subsequently approved the first C++ slice: source algebra and independent tests now exist and an ARM analysis ELF has been cross-compiled; see [the build report](i1-build-report.md). Actual DSP execution and binary-access equivalence remain unverified, so I1 is not complete. Source-level facts below must not be presented as binary traffic or silicon timing.

## 1. Immutable source and selected path

CMSIS-NN **v8.0.0** resolves to commit **`13c97dbb6f781d4aab38ed34e6e441f42b79aff4`**. Its annotated tag object is `fec593391437cd8d240053bae54eba8d1f36d242`; these are different identities. Git remote tag resolution and direct downloads at the full commit were used. File hashes are in [the source manifest](fc-source-manifest.json). Prefer these immutable links over cached tag-page previews.

| Source at that commit | What was inspected |
|---|---|
| [FC entry](https://github.com/ARM-software/CMSIS-NN/blob/13c97dbb6f781d4aab38ed34e6e441f42b79aff4/Source/FullyConnectedFunctions/arm_fully_connected_s8.c#L50) | Batch loop and arguments to the vector/matrix helper |
| [Vector/matrix helper](https://github.com/ARM-software/CMSIS-NN/blob/13c97dbb6f781d4aab38ed34e6e441f42b79aff4/Source/NNSupportFunctions/arm_nn_vec_mat_mult_t_s8.c#L498) | Zero filter-offset DSP branch; two-row blocks and odd-row tail |
| [Support functions](https://github.com/ARM-software/CMSIS-NN/blob/13c97dbb6f781d4aab38ed34e6e441f42b79aff4/Include/arm_nnsupportfunctions.h#L1568) | Four-byte reads and exact requantization operations |
| [Scratch-size functions](https://github.com/ARM-software/CMSIS-NN/blob/13c97dbb6f781d4aab38ed34e6e441f42b79aff4/Source/FullyConnectedFunctions/arm_fully_connected_get_buffer_sizes_s8.c) | DSP getter returns zero bytes |
| [Public API](https://github.com/ARM-software/CMSIS-NN/blob/13c97dbb6f781d4aab38ed34e6e441f42b79aff4/Include/arm_nnfunctions.h#L1600) | Shapes, offsets, per-tensor quantization |
| [Architecture macros](https://github.com/ARM-software/CMSIS-NN/blob/13c97dbb6f781d4aab38ed34e6e441f42b79aff4/Include/arm_nn_math_types.h) | DSP selection from compiler architecture features |

Select `arm_fully_connected_s8` -> `arm_nn_vec_mat_mult_t_s8`, with `ARM_MATH_DSP` enabled and `ARM_MATH_MVEI` absent. Use little-endian Cortex-M4, zero filter offset, present int32 bias, and one per-tensor multiplier/shift. The helper contains a GCC `unroll-loops` pragma: preserve it as part of the production source. The compiler may reorder loads, unroll loops, spill registers, or combine instructions despite the source schedule below.

Proposed quantization build contract: leave both `CMSIS_NN_USE_SINGLE_ROUNDING` and `CMSIS_NN_USE_REQUANTIZE_INLINE_ASSEMBLY` **undefined**, selecting the default two-stage implementation. Defining either macro to zero is insufficient because selection tests whether it is defined. Confirm the effective preprocessor configuration when compiling. These concrete choices are for review, not a claim that every benchmark FC maps to this API.

The DSP scratch requirement is **zero bytes**. Pass a valid context object with null `buf` and zero size; do not pass a null context pointer because the entry reads `ctx->buf`. The helper ignores the resulting `kernel_sum` pointer. No packing buffer or weight-sum preparation is needed for this path. Stack usage is separate and remains unknown until compilation.

## 2. Supported domain and explicit exclusions

Let `N>0` be inputs and `M>0` outputs; both dimensions and all products/addresses must fit their respective API and target address domains. Use contiguous int8 input, row-major contiguous int8 weights, contiguous int32 bias, and contiguous int8 output; disjoint objects and 16-byte bases per the accepted placement policy. Odd `N` is allowed: aligned tensor bases do not imply aligned weight rows. The four-byte source reader uses `memcpy`; its actual lowering and unaligned-access behavior require binary inspection.

Dimension fields for the harness are `input_dims=(n=1,h=1,w=1,c=N)`, `filter_dims=(n=N,h=1,w=1,c=M)`, `output_dims=(n=1,h=1,w=1,c=M)`, and `bias_dims.c=M`. Here `filter_dims.n` is accumulation depth, not number of output rows. The entry uses `input_dims.n`, `filter_dims.n`, and `output_dims.c`; our harness must validate consistency rather than rely on the kernel to reject malformed shapes.

Use input offset `z` in [-127,128], filter offset zero, output offset `o` in [-128,127], and int8 activation bounds `-128 <= a_min <= a_max <= 127`. Initial proposed multiplier domain is `1 <= m <= 2^31-1`, shift `-30 <= s <= 30`. The upstream documented shift range includes -31; defer that endpoint because the default C helper forms a signed `1 << exponent` mask. Do not generalize its language-level behavior without further analysis.

Require every accumulation intermediate, the left-scaled accumulator, and output-offset addition to fit signed int32. A conservative sufficient accumulation check is `abs(b[j]) + sum_i abs((x[i]+z)*w[j,i]) <= 2^31-1`, evaluated with wider checked arithmetic. This is stricter than necessary but protects every DSP accumulation order. Reject cases outside this first domain rather than silently wrapping. Positive requantization shifts additionally require `acc*2^s` to fit int32. The pinned target's signed right shifts must implement arithmetic shifts; the mathematical specification below uses floor division explicitly.

Initially unsupported: batch >1, absent bias, nonzero filter offset, per-channel quantization, int4/int16/MVE/scalar fallback paths, alternate rounding macros, buffer aliasing, overflowing arithmetic, invalid dimensions, and runtime graph allocation. These exclusions narrow our model, not CMSIS-NN itself.

## 3. Numerical equations

For output row `j`:

`acc[j] = b[j] + sum_(i=0..N-1) ((x[i]+z)*w[j,i])`.

The source processes input groups of four using two dual multiply-accumulate operations per row. Within each group the odd lanes (1,3) are accumulated first, then the even lanes (0,2). Remainder elements follow individually. With the no-overflow domain above this equals the semantic dot product.

For default requantization, define `l=max(s,0)`, `r=max(-s,0)`, and:

`v = acc * 2^l`

`h = floor((v*m + 2^30) / 2^31)`.

This first step rounds the scaled product to nearest, with ties toward positive infinity. It is the non-saturating high-multiply helper; do not replace it with a different saturating multiply primitive.

For the second rounding step:

`q = floor(h / 2^r)`

`u = h - q*2^r`

`t = floor((2^r-1)/2) + indicator(q<0)`

`R = q + indicator(u>t)`.

For `r>0`, this rounds division by `2^r` to nearest with ties away from zero; for `r=0` it leaves `h` unchanged. Then:

`y = min(a_max, max(a_min, R+o))`.

Two-stage rounding is intentional. Replacing it with one floating-point multiply and one rounding operation can change an output.

## 4. Worked example: five inputs, three outputs

Choose `N=5`, `M=3`, with:

```text
x = [-3, -1,  0,  2,  4]       z = 1
w = [ 1,  2, -1,  0,  2
     -1,  1,  2, -2,  1
      2,  0, -3,  1,  2]
b = [2, -12, 34]
m = 1073741824 = 2^30          s = -1
o = -1                         activation = [-4, 6]
```

The adjusted input is `[-2,0,1,3,5]`. The first four columns form one DSP block; column 4 is a scalar tail. Rows 0 and 1 form a pair; row 2 is the odd-row tail.

| Row | Bias | After odd lanes 1,3 | After even lanes 0,2 | After column 4 | High multiply h | Rounded divide R | R+o | Output |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 2 | 2 | -1 | 9 | 5 | 3 | 2 | 2 |
| 1 | -12 | -18 | -14 | -9 | -4 | -2 | -3 | -3 |
| 2 | 34 | 37 | 30 | 40 | 20 | 10 | 9 | 6 |

Expected output: **`[2,-3,6]`**. Row 0 shows the two rounding steps: 9 -> 5 -> 3. Row 2 exercises activation clamping. This example uses a simple multiplier to expose the real helper semantics; the supported contract is not restricted to power-of-two scales.

## 5. Ordered tensor-access abstraction

Represent a source tensor event as `(sequence, region, read/write, base+offset, width_bytes)`. Expand each four-byte `memcpy` reader as one four-byte source request. This is an explicit source abstraction, not a claim of one machine load or bus transaction. Exclude parameter-structure reads, instruction fetches, compiler stack/spills and helper implementation temporaries from these tensor-event counts; keep those categories unestablished until compiled mapping.

Let `P=floor(M/2)`, `U=M mod 2`, `Q=floor(N/4)`, `R=N mod 4`. For each pair `p=0..P-1`, row `j=2p`, emit:

1. Bias reads `(B+4j,4)` and `(B+4(j+1),4)`.
2. For each `g=0..Q-1`: input `(X+4g,4)`, weight row j `(W+jN+4g,4)`, weight row j+1 `(W+(j+1)N+4g,4)`.
3. For each `i=4Q..N-1`: input `(X+i,1)`, weight `(W+jN+i,1)`, weight `(W+(j+1)N+i,1)`.
4. Both requantizations and offset/clamp operations, then stores `(Y+j,1)` and `(Y+j+1,1)`.

If `U=1`, process row `j=2P`: one bias read, then groups with input and that weight row, scalar tails with input and that row, epilogue, one output store. Scalar epilogue values are function parameters; do not invent per-output multiplier-array reads for this per-tensor API.

For the example use synthetic bases `X=0x20040000`, `Y=0x20040020`, `W=0x08010000`, `B=0x08010020`. They are aligned, disjoint illustration addresses, not linked placements.

| Event | Action | Address expression | Bytes |
|---:|---|---|---:|
| 1 | read bias 0 | B | 4 |
| 2 | read bias 1 | B+4 | 4 |
| 3 | read input 0..3 | X | 4 |
| 4 | read weights row 0, columns 0..3 | W | 4 |
| 5 | read weights row 1, columns 0..3 | W+5 | 4 |
| 6 | read input 4 | X+4 | 1 |
| 7 | read weight row 0, column 4 | W+4 | 1 |
| 8 | read weight row 1, column 4 | W+9 | 1 |
| 9 | write output 0 after pair epilogue | Y | 1 |
| 10 | write output 1 | Y+1 | 1 |
| 11 | read bias 2 | B+8 | 4 |
| 12 | read input 0..3 again | X | 4 |
| 13 | read weights row 2, columns 0..3 | W+10 | 4 |
| 14 | read input 4 again | X+4 | 1 |
| 15 | read weight row 2, column 4 | W+14 | 1 |
| 16 | write output 2 after epilogue | Y+2 | 1 |

Note the unaligned four-byte weight requests at W+5 and W+10. The source abstraction records them without assuming how the MCU services them.

## 6. Counts derived from the schedule

Let `G=P+U=ceil(M/2)` be input traversals. Units below are semantic operations or source tensor-request bytes, never cycles.

| Quantity | Equation | Example |
|---|---|---:|
| Semantic MACs | N*M | 15 |
| Source SMLAD invocations in dot products | 2*M*Q | 6 |
| Scalar tail multiply-accumulates | M*R | 3 |
| Requantizations / output-offset additions | M each | 3 each |
| Clamp comparisons expressed in source | 2*M | 6 |
| Logical tensor storage | N+N*M+4*M+M | 35 bytes |
| Input requested bytes | G*N | 10 bytes |
| Weight requested bytes | M*N | 15 bytes |
| Bias requested bytes | 4*M | 12 bytes |
| Output requested bytes | M | 3 bytes |
| Total tensor-request bytes | G*N+M*N+5*M | 40 bytes |
| Four-byte input/weight requests | (G+M)*Q | 5 |
| One-byte input/weight requests | (G+M)*R | 5 |
| Total tensor events, including bias and stores | (G+M)*(Q+R)+2*M | 16 |

Check `2*(2*M*Q)+M*R = M*(4Q+R) = M*N`: each SMLAD invocation performs two scalar MACs. Input reuse across a row pair explains why input request bytes are `ceil(M/2)*N`, rather than `M*N`. Logical input storage remains only `N` bytes. Neither 35 nor 40 bytes is a flash bus-traffic estimate.

Loop-control operations, pointer arithmetic, offset lane expansion, intrinsic lowering and epilogue instruction costs must be included in a later compiled operation model. The table is not a total instruction count. Parameter/context metadata and alignment gaps are excluded from logical tensor storage; caller scratch is zero, while stack remains unknown.

## 7. Proposed minimal interfaces

```text
FCSpec(N, M, input_offset, output_offset, multiplier, shift, activation_bounds)
KernelManifest(commit, entry, helper_branch, rounding_macros, source_hashes)
Layout(X, W, B, Y, extents, alignment, regions, provenance)
derive_fc(spec, layout, kernel) -> KernelIR
count_tensor_work(ir) -> WorkSummary
enumerate_source_tensor_events(ir) -> events       # tiny checks only
```

`KernelIR` needs ordered pair/odd-row regions, group/tail domains, base-plus-offset expressions, widths and access kinds, accumulator dependencies, epilogue semantics and supported-domain checks. Mark this representation `source_tensor_schedule`. Attach binary evidence separately when available; never silently promote it to `compiled_access_schedule`.

`WorkSummary` carries semantic MACs, source intrinsic counts, logical tensor bytes and tensor-request bytes separately, with units, exclusions and provenance. Cycles, seconds, physical transactions, cache misses and energy are unsupported here. A separate value oracle computes outputs without sharing the analytical schedule/count implementation.

## 8. Meaningful tests and completion gates

After review and implementation authorization:

- Keep the 16-event example and output vector above as hand-established golden cases.
- Compare row-by-row semantic dot products against blocked DSP-order calculations using wide exact arithmetic; include positive and negative rounding ties, non-power-of-two multipliers, positive/zero/negative shifts, offsets and clamps.
- Check shape cross-products such as `N in {1,3,4,5,7,8,9}`, `M in {1,2,3,4,5}`. These exercise no full block, exact blocks, tails, paired and odd rows.
- Build an independent pointer-stepping source-event enumerator that does not call `derive_fc`, its address evaluator, or its counting formulas. Compare complete ordered events and counts, not just totals.
- Reject zero/negative or inconsistent dimensions, unsupported macros/options, overlap, address overflow, invalid bounds and arithmetic outside the stated domain. Test the supported shift endpoints and explicitly rejected -31 case.
- Verify four-byte requests stay within each tensor, including odd row strides and boundary layouts. Inspect effective macros, compile the actual DSP path and compare numerical results with the golden cases when a suitable execution environment is available. A host scalar fallback is not DSP-path execution evidence.
- Inspect disassembly for effective unrolling, load widths/order, helper lowering, stack/spills and unaligned loads. Add the wrapper/metadata/epilogue accesses needed for any binary-access claim. If independent binary observation is unavailable, leave equivalence open.

This document completes a source derivation for review. I1 remains incomplete until source/build/layout identities and supported semantics are frozen, the small C++ algebra module and independent checks exist, and every claimed exact result is verified. First review the numerical example and tensor schedule; then set up the build and inspect compiled mapping. No cache/timing infrastructure is part of this step.
