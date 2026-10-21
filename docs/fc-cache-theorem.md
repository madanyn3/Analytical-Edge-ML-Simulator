# FC cache theorem (Phase A)

This document derives exact miss counts for the int8 fully connected kernel's flash read
stream under the accepted synthetic cache contract. It covers:
- **A2:** repeated calls, proved here for *any* read-only stream;
- **A1:** a single cold call of the FC paired-row stream, in closed form, and its warm
  calls (§3.1).

Each result names the test that checks it against the independent timestamp-LRU oracle
in `tests/memory_tests.cpp`.

**Status of the claims.** The results are exact under the synthetic contract of
[memory-models.md](memory-models.md):
- modulo set mapping, per-set LRU;
- reads only, complete-line fills, no prefetch;
- an empty or explicit initial state.

They are not claims about the L4R5 ART accelerator. Its line size, associativity and fill
behaviour are unverified ([microarchitecture audit](l4r5-microarchitecture-audit.md)).
The stream is the pinned CMSIS-NN *source* schedule (`enumerate_events` in `src/fc.cpp`).
The compiled binary's access order is a separate, pending question.

## 1. Setting

**Cache and references.** A cache has line size `L`, `S` sets and `W` ways. Line
`x = ⌊A/L⌋` maps to set `x mod S`. An access of `b` bytes at address `A` becomes the
ascending line references `⌊A/L⌋, …, ⌊(A+b−1)/L⌋`.

**Calls.** A *call* is a finite line-reference sequence `σ = σ_0 σ_1 … σ_{R−1}`. For each
line `x` that occurs in `σ`, its *window* is `[f_x, l_x]`: the positions of its first and
last references in the call. `k_s` is the number of distinct lines of set `s` that occur
in `σ`.

**States.** A state gives, for each set, at most `W` resident lines ordered from MRU to
LRU. `E(σ, I)` is the exit state of one call of `σ` started from state `I`.

**Lemma 1.1 (LRU stack depth; Mattson et al. 1970).** Treat the initial state `I` as a
history in which each set's resident lines were referenced from LRU to MRU. Then a
reference to `x` hits iff two conditions hold:
- `x` has a previous reference in this history;
- fewer than `W` distinct other lines of `x`'s set are referenced between that previous
  reference and this one.

*Proof.* Within a set, LRU keeps lines ordered by recency, which is the per-set stack.
A reference moves its line to the top. A line's depth is the number of distinct other
lines of its set referenced since its own last reference: each such line moves above it
exactly once, and other sets never interact. A line is resident iff its depth is less
than `W`. ∎

`analyze_reuse` implements this lemma with unlimited per-set stacks. The test oracle
instead keeps at most `W` residents per set, with timestamps, and evicts the minimum.

**Lemma 1.2 (exit state).** For each set `s`, `E(σ, I)` restricted to `s` consists of two
parts, together limited to `W` lines:
1. the distinct lines of set `s` that occur in `σ`, most recently referenced first;
2. if there are fewer than `W` of them, the lines of `I(s)` that do not occur in `σ`, in
   their initial order.

*Proof.* At the end of the call the stack of set `s` lists lines by recency. Lines
referenced in the call are more recent than any line that was only resident initially.
The latter keep their relative order, because nothing references them. The cache holds
the top `W`. ∎

## 2. A2: repeated calls

**Theorem 2.1 (one call reaches a fixed point).** For any read-only stream `σ` and any
initial state `I`,

```text
E(σ, E(σ, I)) = E(σ, I).
```

So every call after the first starts from the same state and has the same hits, misses
and exit state.

*Proof.* Apply Lemma 1.2 to the second call, started from `E₁ = E(σ, I)`.
- **Part 1** of `E(σ, E₁)` depends only on `σ`, so it is the same as part 1 of `E₁`.
- **Part 2** takes the lines of `E₁(s)` that do not occur in `σ`, in their order in
  `E₁`. By Lemma 1.2 these are exactly the lines of `I(s)` that do not occur in `σ`, in
  their initial order. Part 2 also needs the same number of them, since part 1 has the
  same size.

Both parts coincide, so `E(σ, E₁) = E₁`. The second call starts from `E₁`, and so does
every later one, by induction. A call is a deterministic function of its stream and its
start state, so all calls after the first are identical. ∎

**Theorem 2.2 (warm-call criterion).** In any call after the first:
1. A reference to `x` other than its first in the call hits iff fewer than `W` distinct
   other lines of its set are referenced since its previous reference *in this call*.
   This is the same outcome as in every other call.
2. The first reference to `x` hits iff `D_cyc(x) < W`, where

   ```text
   D_cyc(x) = (k_s − 1) − #{ y ≠ x in set(x) : f_x < f_y and l_y < l_x }.
   ```

   Here `s = set(x)`, so `D_cyc(x)` counts the other lines of `x`'s set whose windows
   are not nested strictly inside `x`'s window.

*Proof.*
1. The previous reference lies in the same call, so Lemma 1.1 depends only on the
   references in between, which are identical in every call.
2. The previous call ran the same `σ`, so the previous reference of `x` is its last
   reference in that call, at position `l_x`. By Lemma 1.1, the reference hits iff fewer
   than `W` distinct other lines of the set are referenced in the *cyclic gap*: after
   `l_x` in the previous call, or before `f_x` in this one.

   A line `y` of the set occurs in that gap iff `y` has a reference after `l_x` or
   before `f_x`. It does *not* occur iff all its references lie strictly inside
   `(f_x, l_x)`, that is iff `f_x < f_y` and `l_y < l_x` (positions are distinct).
   Lines that are only resident, and never referenced, do not count. ∎

**Corollary 2.3 (cold and warm counts).** Let `re` be the number of references that are
not the first reference of their line in the call and that miss. By Theorem 2.2(1), `re`
is the same in every call. Then:
- **Cold call** (empty start): misses `= #distinct lines + re`.
- **Every warm call:** misses `= #{x : D_cyc(x) ≥ W} + re`, and evictions `= misses`.

*Proof.* The miss counts follow from Theorem 2.2. For evictions: a set's occupancy never
decreases (there are no invalidations), and by Theorem 2.1 it is the same at the start
and end of a warm call. So occupancy is constant during the call. A miss in a set that
is not full would raise it, so every set that misses is full and every miss evicts. ∎

**Corollary 2.4 (the cold-sweep theorem as a special case).** For a sweep of distinct
consecutive lines, every window is a single point, so nothing is nested and
`D_cyc(x) = k_s − 1`. Lines in sets with `k_s ≤ W` hit in every warm call, and lines in
sets with `k_s > W` miss in every warm call. This is the formula of
[memory-models.md](memory-models.md) §2, which `analyze_cold_sweep` implements.

**Why nesting matters for FC.** In the FC stream some windows are long:
- a bias line is reused across several row pairs;
- a weight line shared by two rows is touched at the start and at the end of a pair.

Short-lived lines of the same set can be nested inside such a window. They never enter
the cyclic gap, so a long-window line can survive between calls even when `k_s > W`. The
plain sweep rule would get this wrong.

**Implementation.** `analyze_repeated(geometry, events, calls, initial)` in
`src/memory.cpp` works in two parts:
- It evaluates the first call with the finite analyzer.
- For the steady call it computes windows, and counts nested windows per set with a
  Fenwick tree, in `O(R log R)`. `re` comes from a cold finite run as
  `cold misses − distinct lines`.

Totals for any number of calls cost the same as for two.

*Check.* `memory_tests` runs 1,608 repeated-call cases against the oracle, which replays
every call from the previous call's exit state. They cover:
- random streams with `L ∈ {1, 3, 8}`, `S = 1–5`, `W = 1–4`, 1–5 calls, and empty or
  random warm entry states;
- sweeps checked against `analyze_cold_sweep`;
- FC flash streams with `n ∈ {1, 3, 4, 5, 8, 9, 13}`, `m ∈ {1, 2, 3, 5}`,
  `S ∈ {1, 2, 4}`, `W ∈ {1, 2}`.

Every per-call count and state matches. Two mutations each fail the suite: changing
`D_cyc ≥ W` to `> W`, and dropping the nesting term (which reduces the rule to the sweep
rule).

## 3. A1: one cold call of the FC stream

**Stream.** The flash stream of one call is a sequence of *regions*. Pair `p` (rows
`2p, 2p+1`) gives one region, and an odd last row gives a final one-row region. Write
`B_p = weights + 2pn` and `b_p = bias + 8p`. Each region issues, in order:
1. its bias words: 4 bytes each, at `b_p`, `b_p + 4`;
2. accesses `A(0) B(0) A(1) B(1) … A(K−1) B(K−1)`, where `K = G + T`, `G = ⌊n/4⌋` and
   `T = n mod 4`.
   - Access `k` starts at row offset `t(k)` with size `s(k)`: `t(k) = 4k` and `s(k) = 4` for
     `k < G` (groups), then `t(k) = 4G + (k−G)` and `s(k) = 1` (tails).
   - Cursor `A` reads row `2p` at `B_p + t(k)`; cursor `B` reads row `2p+1` at
     `B_p + n + t(k)`.
   - The one-row region has `A` only.

Input reads and output writes go to SRAM and are not part of this stream. The two rows of
a pair are adjacent, so a region's weights are one contiguous range of `2n` bytes, read
by two cursors `n` bytes apart.

**Supported domain.**
- `n ≥ 2L + 8`;
- the weight lines and the bias lines are disjoint ranges.

Outside it `analyze_fc_flash` throws, because the classification below would need extra
cases. Wide layers satisfy it for the usual line sizes. Narrow ones, such as an
autoencoder bottleneck, stay with the finite analyzer. Bases are 16-byte aligned, as `derive_fc` requires; the proofs do not use this.

**Lemma 3.1 (geometry in the domain).** In a pair region:
1. The lines of `A(k)` and of `B(k)` are disjoint, and so are those of `B(k)` and
   `A(k+1)`. Their nearest bytes are at least `n − 7 ≥ L + 1` apart.
2. Cursor `A` touches lines `[X, M]` and cursor `B` touches `[M′, E]`, where
   `X = ⌊B_p/L⌋`, `M = ⌊(B_p+n−1)/L⌋`, `M′ = ⌊(B_p+n)/L⌋` and `E = ⌊(B_p+2n−1)/L⌋`. The two
   ranges share exactly one line, the *middle line* `M = M′`, iff `L ∤ B_p + n`.
3. Consecutive regions share exactly one weight line, `E_{p−1} = X_p`, iff `L ∤ B_p`. They
   share a bias line, `⌊b_p/L⌋`, iff `L ∤ b_p`.
4. If `M` is shared, `B` makes its last reference to `M` in access `k*`, the access holding
   row-`2p+1` offset `(M+1)L − 1 − (B_p+n) < L`. `A` makes its first reference to `M` in
   access `k_a`, the access holding row-`2p` offset `ML − B_p ≥ n − L + 1`. Since
   `n ≥ 2L + 8`, `t(k_a) ≥ n − L − 2 > L > t(k*)`, so `k_a > k*`.

*Proof.* Each item is interval arithmetic on byte offsets, using `s(k) ≤ 4`. Bytes at
least `L` apart lie in different lines. ∎

**Theorem 3.2 (classification of re-references).** In one call, every reference that is
not the first reference of its line in the call is of exactly one of the following kinds.
Each comes with the set of lines referenced between it and the previous reference of the
same line (its *gap*):

| kind | when | gap |
|---|---|---|
| in-access / bias word | `b_p + 4` starts inside the line of word 1, or a later line of the same access | empty |
| one-row region | `A(k+1)` continues `A(k)`'s line | empty |
| cursor A | `A(k+1)` starts inside `A(k)`'s last line `x` (`L ∤ B_p + t(k+1)`) | lines of `B(k)` |
| cursor B | `B(k+1)` starts inside `B(k)`'s last line (`L ∤ B_p + n + t(k+1)`) | lines of `A(k+1)` |
| middle | `A(k_a)` first touches `M` | `[lo, hi] \ {M}`, `lo = ⌊(B_p+t(k*+1))/L⌋`, `hi` = last line of `B(k_a−1)` |
| boundary | `A(0)` of region `p ≥ 1` starts on `E_{p−1}` | bias lines of region `p` |
| bias | region `p ≥ 1`'s first bias line was region `p−1`'s last | weight lines `[X_{p−1}, E_{p−1}]` |

*Proof.* Every access covers consecutive bytes and the cursors only advance. So a line's
references by one cursor are consecutive accesses of that cursor, and its first reference
by that cursor is either new or one of the cross-cursor or cross-region shares of
Lemma 3.1. These shares are:
- the middle line: `B` touches it first, `A` last;
- the boundary line: region `p−1`'s `B` touches it last, region `p`'s `A` first;
- the shared bias line.

The gaps follow from the issue order. For example, between `B(k*)` and `A(k_a)` the
stream contains `A(k*+1 … k_a)` and `B(k*+1 … k_a−1)`. These sweep every line from `lo`
to `M−1` and from the line after `M` to `hi`, because both cursors cover their bytes
contiguously (Lemma 3.1.4 gives `k_a > k*`). Region `p−1`'s last flash access is `B(K−1)`,
which ends on `E_{p−1}`. Only region `p`'s bias words follow it before `A(0)`. ∎

**Theorem 3.3 (cold call).** Let `c_s(I)` be the number of lines `y` in a line range `I`
with `y ≡ s (mod S)`. A re-reference to `x` misses iff `c_{x mod S}(gap) ≥ W`, by
Lemma 1.1, since the lines of the gap differ from `x` (Lemma 3.1). For the middle kind
the condition is `c_{M mod S}([lo, hi]) − 1 ≥ W`. Then, from an empty cache:

```text
misses     = ⌈weight lines⌉ + ⌈bias lines⌉ + re_cursor + re_middle + re_boundary + re_bias
evictions  = misses − Σ_s min(W, k_s)
```

The final state follows from Lemma 1.2 with the last-reference order below.

Each `c_s` over a range is a difference of two floors:
`c_s([lo, hi]) = ⌊(hi − s)/S⌋ − ⌊(lo − 1 − s)/S⌋`, with the natural convention for
negative arguments.

The resonance of the paired rows is the *cursor* kind:
- **W = 1.** A continuation misses whenever the other cursor's access touches its set, so
  whole phases of a pair miss on every access although the pair's working set is two lines.
- **W = 2.** A continuation can miss only when `S = 1` and the other cursor's access
  straddles a line.
- **W ≥ 3.** Continuations never miss.

The middle, boundary and bias kinds are the long-gap reuses. Whether they hit depends on
how many lines of the right set fit into a whole row (middle) or pair (bias), which is
what the counts `c_s` measure.

**Lemma 3.4 (periodicity).** Shifting every address by a multiple of `S·L` shifts every
line by a multiple of `S`. It changes neither sets nor line offsets, so it changes no
hit or miss. Hence:
- a group access's contribution depends on `k` only through `k mod S·L/gcd(S·L, 4)`;
- a region's contribution depends only on `(B_p mod S·L, b_p mod S·L, rows, p > 0)`.

**Implementation.** `analyze_fc_flash(geometry, ir)` in `src/fc_cache.cpp` works as
follows.
- For each region type it sums the group accesses over one period with multiplicities,
  and treats the at most four tail transitions explicitly.
- It memoises regions by type: there are at most `S·L` types per row count.
- The final state comes from Lemma 1.2. Each line gets a last-reference key
  `(region, access, cursor, line)`: its last byte's access, or `A(K−1)` for a middle
  line. Region numbers do not decrease with the line number, so a downward scan of each
  set stops once `W` candidates come from later regions.

The counts cost `O(m log(SL) + S·L · min(m, S·L))`, independent of `n`. The final state
costs `O(S·W + n/L)`. A layer of `2^20 × 2^10` bytes runs in the test without
enumeration.

*Check.* `fc_cache_tests` (ctest `fc_cache_theorem`) compares counts, evictions and the
final state with the oracle replaying the enumerated stream:
- 38,400 exhaustive cases: `L ∈ {4, 8, 16, 32}`, `S ∈ {1, 2, 3, 4, 8}`, `W = 1–4`,
  every `n` in `[2L+8, 2L+27]`, `m ∈ {1, 2, 3, 4, 5, 7}`, two weight-base offsets within an
  `S·L` period, and bias just after the weights or apart;
- 300 random layers, `n` up to 700, `m` up to 40, `S` up to 16;
- 3,770 dense-region layers with many pairs per `S·L` period (768 on a grid, 3,000 random,
  2 directed; see §3.1). That makes 42,470 cases in all.

Every kind produces misses in the suite (cursor 165,924; middle 24,071; boundary 3,712;
bias 10,004). Seven mutations each fail the suite: weakening any of the miss conditions
from `≥ W` to `> W`, dropping either cursor kind, mis-keying the middle line's last
reference, or stopping the final-state scan early. Domain violations are rejected.

### 3.1 Warm calls of the FC stream

Theorems 2.1 and 2.2 reduce warm calls to counting nested windows: a warm first reference
to `x` misses iff `(k_s − 1) − nested(x) ≥ W`, and the re-misses are those of A1. For the
FC stream every window and every nested count has a closed description.

**Windows.** Within one cursor piece (row `2p` read by `A`, row `2p+1` read by `B`) the
recency of a byte's reference increases with the byte. A line's bytes fall into at most
three pieces, so its window `[f_x, l_x]` is the minimum and maximum of the keys of the
first and last byte of each piece. This makes the middle line's window run from `B(0)` to
`A(K−1)`, the boundary line's from `B(k)` of one region into `A` of the next, and a bias
line's window span every region whose bias words it holds.

**Lemma 3.5 (monotone runs).** Let `s_j = ⌊(B_j + n)/L⌋` be the line where row `2j+1`
starts. Take the weight lines strictly between `s_{j−1}` and `s_j`. If `s_j` is not shared
with row `2j`, include it in run `j+1`. On each such run, both `f_y` and `l_y` are
non-decreasing in `y`. The same holds for the whole bias range.

*Proof.* Run `j` holds row `2j−1`'s lines (cursor `B` of pair `j−1`), then row `2j`'s
(cursor `A` of region `j`). Each cursor's keys increase with the byte, and pair `j−1`
precedes region `j`. The only lines whose windows break this order are the shared middle
lines: their first reference is `B(0)` but their last is `A(K−1)`. These are excluded and
handled one by one. The boundary line closes a run with the largest `l` and opens nothing
earlier. Bias lines are read in ascending order, one region after another. ∎

**Theorem 3.6 (nested count).** For a window `[f_x, l_x]`, the lines nested in it are, on
each run, the interval of `y` with `f_y > f_x` and `l_y < l_x`. Two binary searches find
it, and `c_{x mod S}` counts the interval. Only the runs, middle lines and bias lines of
the regions that `x`'s window spans can be nested. That is at most three regions for a
weight line, and `⌈L/8⌉ + 1` for a bias line. So `nested(x)` costs `O(L log(mn))`.

*Proof.* Lemma 3.5 and the definition of nesting. A line referenced outside those regions
has a reference outside `x`'s window. ∎

**Theorem 3.7 (warm FC counts).** From an empty cache, every call after the first has:

```text
misses    = re_cursor + re_middle + re_boundary + re_bias
          + #{x : (k_{x mod S} − 1) − nested(x) ≥ W}
evictions = misses
```

Its exit state is the cold exit state of A1.

*Proof.* Corollary 2.3, Theorems 2.1 and 3.3, with `nested` from Theorem 3.6. ∎

**Implementation.** `analyze_fc_flash_repeated(geometry, ir, calls)` sums the indicator
over all lines, with a memo by region type:
- A weight line belongs to the region of its first byte. Its window, and every window
  nested in it, lies in regions `p … p+2`.
- Its count therefore depends only on `B_p mod S·L`, `b_p mod S·L`, `p > 0`, and the number
  of regions after `p`, clamped at 3 (Lemma 3.4).
- Bias lines are summed directly.

The cost is `O(min(m, S·L) · (n/L) · L log(mn) + m)`. It is independent of the number of
calls, and of `m` once `m > S·L`. Its dependence on `n` is linear in the lines of one row,
not in the references.

*Check.* Every `fc_cache_tests` case also replays three calls in the oracle. It compares
call 1 with A1 and calls 2 and 3 with the steady counts and state. Warm calls differ from
cold ones in 16,399 cases. In 7,782 cases a warm call keeps only part of the stream.

The memo key needed its own tests:
- **Dense regions.** 3,000 random layers with 8–20 pairs and 4–12 sets, so region types
  recur while sets hold few lines.
- **Two directed cases**, found by searching against mutants of the key:
  - dropping the bias residue (`L = 8, S = 15, W = 4, n = 30, m = 17`);
  - clamping the remaining regions at 2 (`L = 16, S = 1, W = 38, n = 42, m = 13`).
- **Memo-free comparison.** A separate one-off run compared the memoised analyzer with a
  memo-free copy over a dense grid, and they agree on cold and warm counts in all
  11,151,360 configurations. The grid: `L ∈ {4, 8, 16, 32}`, `S = 1–8`, `W = 1–40`,
  `n ∈ [2L+8, 4L+8)`, `m = 3–24`, and several weight and bias offsets.

Mutations each fail the suite:
- ignoring nesting;
- dropping the middle-line, bias-run or run-range terms;
- shifting a run start;
- collapsing window pieces;
- dropping a bias line;
- either memo-key mutation.

**Not yet covered.**
- **A3**, the exhaustive campaign over all base residues and larger `n, m`.
- Cases outside the domain (`n < 2L + 8`, or shared weight/bias lines).
- Warm calls from an arbitrary, non-empty initial state. Only the first call depends on
  it, and that call needs the cold analysis generalised.

## References

- R. L. Mattson, J. Gecsei, D. R. Slutz, I. L. Traiger. Evaluation techniques for
  storage hierarchies. *IBM Systems Journal* 9(2), 1970.
- T. Gysi, T. Grosser, L. Brandner, T. Hoefler. A fast analytical model of fully
  associative caches (HayStack). PLDI 2019.
