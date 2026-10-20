# FC cache theorem (Phase A)

This document derives exact miss counts for the int8 fully connected kernel's flash read
stream under the accepted synthetic cache contract. It covers:
- **A2:** repeated calls, proved here for *any* read-only stream;
- **A1:** a single call of the FC paired-row stream, in progress.

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

## 3. A1: one call of the FC stream (in progress)

The flash stream of one call is the concatenation of *regions*. Each pair of rows
`(j, j+1)` with `j = 2p` gives one region, plus a final region for an odd last row. A
region consists of:
1. the bias words of its rows: 4 bytes each, contiguous, at `bias + 4j`;
2. for each group `g < ⌊n/4⌋`: 4 bytes of row `j` at `weights + jn + 4g`, then 4 bytes of
   row `j+1` at the same offset plus `n`;
3. for each tail column `i` in `[4⌊n/4⌋, n)`: 1 byte of row `j`, then 1 byte of row
   `j+1`.

Rows `j` and `j+1` are adjacent in memory. So a region's weight bytes form one contiguous
range of `2n` bytes, read by two cursors `n` bytes apart. For `n = qL + r` the two
current lines differ by `q` or `q + 1`. They fall in the same set exactly when that
difference is `≡ 0 (mod S)`. This is the resonance: with `W = 1`, every access in such a
phase misses, even though the working set is two lines.

The remaining steps are:
- derive the windows and the within-call re-miss count `re` in closed form, per region;
- derive the distinct-line count and the nesting counts that A2 needs;
- state the supported domain explicitly (for example `n ≥ 2L`, bias and weight ranges
  not sharing a line), and reject unsupported cases;
- verify exhaustively for `n, m ≤ 64`, all bases mod `S·L`, and sweeps of `S`, `W`
  and `L`.

## References

- R. L. Mattson, J. Gecsei, D. R. Slutz, I. L. Traiger. Evaluation techniques for
  storage hierarchies. *IBM Systems Journal* 9(2), 1970.
- T. Gysi, T. Grosser, L. Brandner, T. Hoefler. A fast analytical model of fully
  associative caches (HayStack). PLDI 2019.
