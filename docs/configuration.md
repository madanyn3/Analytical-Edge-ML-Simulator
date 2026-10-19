# Configuration and direct module testbench

Visual companions: [configuration parser and knobs](diagrams/07-configuration.svg),
[testbench modes and injection points](diagrams/08-testbench.svg), and the
[complete visual study guide](visual-guide.md).

Build the host C++ targets as in the README. No board, ARM binary or instruction
frontend is needed. In a WSL shell at the repository root:

```sh
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/edge_bench configs/hierarchy.ini
./build/edge_bench configs/memory-latency.ini
./build/edge_bench configs/memory-bandwidth.ini
./build/edge_bench configs/hierarchy-directed.ini --trace configs/reads.csv
```

All supplied profiles are synthetic. `hierarchy.ini` demonstrates one billion
closed-loop L1 requests with guarded fixed-state acceleration. `memory-bandwidth.ini`
uses the cold periodic-memory closed form. The other profiles use bounded finite
evaluation and print the evaluated request completions.

## Version 2: tiled cluster

`version=2` files describe the synthetic k×k mesh/torus cluster. Their sections and
constraints are specified in [the cluster design](cluster-design.md), section 7. They use
the same lexical rules as version 1 (shared lexer `src/ini.cpp`). `edge_bench` reads
`[model] version` and runs version 2 files through the exact cluster evaluator
(`run_cluster`); `--trace` is rejected for them. Only `engine=finite` runs today:
`periodic_state` parses but the evaluator fails explicitly until it is implemented.

Examples (synthetic parameters, not hardware claims):

| File | Topology | MC tile | Stimulus |
|---|---|---|---|
| `configs/cluster-2x2-mesh.ini` | 2×2 mesh | 0 | 4 cores × 1000 reads, 64 private L2 lines each |
| `configs/cluster-3x3-torus.ini` | 3×3 torus | 4 (centre) | 9 cores × 1000 reads, 32 lines each |
| `configs/cluster-4x4-mesh.ini` | 4×4 mesh | 0 (corner) | 16 cores × 1000 reads, 64 lines each; L2 exactly full |

The report (`report_cluster`) prints, all exact under the configured abstraction:

- totals: reads, events, transactions, makespan T (last completion over all cores);
- per core: tile coordinates, XY hops to the MC, read count, mean/min/max latency,
  last completion, L1 hits/misses/evictions, reads per 1000 cycles;
- fairness: Jain's index over per-core throughput reads/(last_done − start) and the
  max/min ratio of per-core mean latency;
- per bank: L2 references/hits/misses/evictions, grants, wait cycles (grant − arrival),
  busy cycles and utilization busy/T;
- memory requests, wire bytes and wait cycles;
- per directed link that carried traffic: packets, flits, busy and wait cycles,
  utilization busy/T, plus the busiest link;
- final non-empty L1/L2 sets (MRU to LRU), memory-service state, and the effective
  configuration with provenance (it parses back to the input).

Utilizations are ratios over the whole run including the cold start and drain, not
steady-state estimates. The report has no per-read latency breakdown yet.

## Strict version-1 syntax

The parser implements a deliberately small INI dialect with no dependency download.
This is not TOML and is not advertised as a general INI parser.

- `[model]`, `[memory]`, `[stimulus]`, and up to eight `[cache.NAME]` sections.
- Cache section declaration order defines upper-to-lower hierarchy order.
- `key=value`; identifiers use ASCII letters, digits and underscores.
- Numbers are unsigned decimal or lowercase-prefix `0x` hexadecimal, with no sign,
  exponent, fractional value or unit suffix. Units are in the key/contract.
- Trim spaces/tabs/CR; blank lines and `#` comments allowed. Values are unquoted;
  `#` starts a comment even in provenance text. No escaping, substitutions, includes
  or environment-variable expansion. A value cannot contain `=`.
- Every listed key below is required. No hidden physical defaults.
- Duplicate keys/sections, unknown keys/sections, missing fields, malformed numbers,
  overflow, invalid cross-field constraints and unsupported policy values fail.
- Diagnostics identify the source and key; lexical/number/unknown-key errors also
  identify the line. Cross-field validation identifies the violated constraint.
- Config size at most 1 MiB, line length at most 4096 bytes; control characters
  other than tab/CR/newline are rejected. Unsupported version fails explicitly.

## Knobs

| Section | Keys | Meaning / domain |
|---|---|---|
| model | version | Exactly 1 |
| model | clock_hz | Positive conversion frequency; ticks remain integer cycles |
| model | inclusion | `inclusive` or `noninclusive` |
| model | access, prefetch, cache_concurrency | Exactly `read_only`, `none`, `blocking`; alternatives rejected |
| cache.NAME | replacement | Exactly `lru` |
| cache.NAME | line_bytes, sets, ways | Positive geometry, checked size arithmetic; lower line sizes must be multiples of upper sizes |
| cache.NAME | lookup_cycles | Positive cost per line reference, hit or miss |
| cache.NAME | fill_cycles | Nonnegative extra cost after lower response, only on misses |
| cache.NAME | initial_lines | `empty` or comma-separated **line numbers**, in MRU-to-LRU order within each set; not byte addresses. Duplicates/mapping/capacity/inclusion checked |
| cache.NAME | provenance | Nonempty source/assumption label for this level; synthetic in supplied profiles |
| memory | policy | Exactly `fifo` |
| memory | first_beat_cycles | Positive admission-to-first-beat completion latency |
| memory | beat_bytes, beat_cycles | Positive beat width and per-beat resource occupancy |
| memory | issue_cycles | Positive minimum initiation spacing, separate from response latency |
| memory | outstanding | Positive in-flight credit count, at most 1,000,000 |
| memory | provenance | Nonempty source/assumption label for service parameters |
| stimulus | entry | A configured cache name or `memory`; fixed for a run |
| stimulus | mode | `open_loop` for offered timestamps, or `closed_loop` for completion feedback |
| stimulus | engine | `finite`, `periodic` or `stable_blocks`, with restrictions below |
| stimulus | count | Nonnegative generated request count |
| stimulus | address, bytes | Base byte address and positive request width |
| stimulus | stride, address_period | Address of request i is base + (i mod period)*stride; period positive |
| stimulus | arrival_cycles, spacing_cycles | First offered tick and open-loop interval or closed-loop think time |
| stimulus | max_requests | Actual finite evaluation budget, positive and at most 1,000,000 |
| stimulus | max_transactions | Expanded cache-line plus memory-transaction budget, positive and at most 10,000,000 |

Combined cache capacity is limited to one million modeled lines, independent of
their byte size. These are implementation budgets, not claims about physical cache
capacity. Counters include all analytically skipped work; budgets charge evaluated
work only. `finite` requires count <= max_requests. API users also pass through the
same semantic validator as file configurations.

`periodic` requires direct `memory`, `open_loop`, and the same address on every
request (stride zero or period one). It uses constant width and cold service state.
`stable_blocks` requires a cache entry, `closed_loop`, and a block no larger than
the evaluation budget. It supports only a one-block fixed point; when none is
proven, the finite budget is enforced. Full derivations and unsupported mechanisms
are in [the model contract](hierarchy-service-contract.md).

## Directed stimulus

`--trace` requires a finite open-loop config. It overrides generated arrivals,
addresses, widths and count; the configured entry, hardware and budgets still apply.
The whole config must remain valid. Header and three columns are mandatory:

```csv
arrival_cycles,address,bytes
0,0x08010000,4
100,0x08010000,4
200,0x08010010,4
```

Values use the same integer convention. Offered arrivals must be nondecreasing;
equal timestamps preserve file order. Zero widths, extra/missing columns, signs,
trailing commas and address overflow fail. Blank rows after the header are ignored.
No quoting, comments or write requests are supported in CSV. File limit: 64 MiB;
row length: 4096 bytes; row count: max_requests.

For an **unloaded latency** test, space offers farther apart than their completion
times, or use closed-loop spacing. For **loaded bandwidth**, offer an open-loop
burst or a tight periodic stream; inspect useful-byte throughput including drain
separately from growing queueing latency. Change outstanding credits to expose the
latency-limited versus transfer-limited regime. For cache hits, initialize resident
lines explicitly or inspect a repeated call; warm does not universally mean hit.

For sensitivity experiments, copy a profile and vary one of sets/ways/line size,
inclusion, lookup/fill cycles, beat width/occupancy, latency, credits or arrival gap.
Record the resulting effective config printed with every report. Do not label these
scenario differences as measured confidence intervals.
