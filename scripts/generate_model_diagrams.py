"""Generate editable, dependency-free SVG study figures for the implemented model.

Only the Python standard library is required. Text stays selectable; no raster
images, JavaScript, remote fonts or externally referenced SVG assets are emitted.
Coordinates are deliberately explicit so the diagrams remain easy to maintain.
"""
from pathlib import Path
from html import escape
import textwrap
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "docs" / "diagrams"
OUT.mkdir(parents=True, exist_ok=True)
INK, MUTED, BLUE, GREEN, ORANGE, PURPLE = "#142b43", "#51657a", "#2465a8", "#137866", "#a75616", "#7151a1"
PALE = {BLUE: "#edf5ff", GREEN: "#edf9f4", ORANGE: "#fff5e9", PURPLE: "#f4effa", INK: "#f0f4f8"}


class Figure:
    def __init__(self, filename, number, title, subtitle, height=1120):
        self.filename, self.height = filename, height
        self.parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="1440" height="{height}" viewBox="0 0 1440 {height}" role="img" aria-labelledby="title desc">',
            f'<title id="title">{escape(title)}</title><desc id="desc">{escape(subtitle)}. Implementation study diagram; exact only under the stated synthetic model.</desc>',
            '<defs>' + ''.join(f'<marker id="a{c[1:]}" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="8" markerHeight="8" orient="auto-start-reverse"><path d="M 0 0 L 10 5 L 0 10 z" fill="{c}"/></marker>' for c in [BLUE,GREEN,ORANGE,PURPLE,INK]) + '</defs>',
            '<style>text{font-family:Arial,Helvetica,sans-serif} .mono{font-family:Consolas,monospace}</style>',
            f'<rect width="1440" height="{height}" fill="#fbfcfe"/>',
            '<rect width="1440" height="144" fill="#142b43"/>']
        self.text(48, 38, f"EDGE-ML  /  IMPLEMENTATION ATLAS  /  {number:02}", 15, "#a5d8f5", weight=700)
        self.text(48, 82, title, 34, "#ffffff", weight=700)
        self.text(48, 117, subtitle, 19, "#d5e4f1")

    def text(self, x, y, value, size=20, color=INK, weight=400, mono=False):
        self.parts.append(f'<text x="{x}" y="{y}" font-size="{size}" fill="{color}" font-weight="{weight}"'+(' class="mono"' if mono else '')+f'>{escape(str(value))}</text>')

    def lines(self, x, y, lines, size=20, gap=30, color=INK, mono=False):
        for i, line in enumerate(lines):
            self.text(x, y+i*gap, line, size, color, mono=mono)

    def box(self, x, y, w, h, title, lines=(), color=BLUE, size=20):
        self.parts.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="14" fill="{PALE[color]}" stroke="{color}" stroke-width="1.5"/>')
        self.parts.append(f'<rect x="{x}" y="{y+15}" width="5" height="28" fill="{color}"/>')
        title_size = min(23, int((w-44)/max(1,len(title)*.53)))
        self.text(x+22, y+39, title, title_size, color, 700)
        while True:
            wrapped = []
            for line in lines:
                wrapped.extend(textwrap.wrap(line, width=int((w-44)/(size*.50)), break_long_words=False) or [''])
            gap = size+8
            if 76+max(0,len(wrapped)-1)*gap <= h-18:
                break
            size -= 1
            if size < 16:
                raise ValueError(f"Text exceeds card: {self.filename}: {title}")
        self.lines(x+22, y+76, wrapped, size=size, gap=gap)

    def arrow(self, points, color=BLUE, dashed=False):
        p=' '.join(f'{x},{y}' for x,y in points)
        self.parts.append(f'<polyline points="{p}" fill="none" stroke="{color}" stroke-width="2.5" stroke-linejoin="round"'+(' stroke-dasharray="7 5"' if dashed else '')+f' marker-end="url(#a{color[1:]})"/>')

    def rule(self, y):
        self.parts.append(f'<path d="M 48 {y} H 1392" stroke="#d4dfe9"/>')

    def bar(self,x,y,w,h,label,color=BLUE):
        self.parts.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="4" fill="{color}"/>')
        if label: self.text(x+10,y+h/2+6,label,18,"#ffffff",700)

    def save(self, source):
        self.rule(self.height-77)
        self.text(48,self.height-48,"SCOPE  Synthetic read-only model • one clock • no board-accuracy claim",17,MUTED)
        self.text(48,self.height-22,"CODE  "+source,16,MUTED)
        self.parts.append('</svg>')
        svg='\n'.join(self.parts)
        ET.fromstring(svg)
        (OUT/self.filename).write_text(svg,encoding='utf-8')


f=Figure('01-system-map.svg',1,'From configuration to an explainable result','Module boundaries, data flow and the three execution engines')
f.box(48,180,390,195,'1  Configuration', ['ModelConfig: clock and policies','CacheConfig[]: ordered levels','ServiceConfig: latency / beats / credits','StimulusConfig: entry / mode / engine'])
f.box(524,180,390,195,'2  Request source',['Generated address pattern or CSV','Request = {arrival, address, bytes}','One injection entry per run','No CPU frontend required'],GREEN)
f.box(1000,180,392,195,'3  Engine selection',['finite: bounded request processing','periodic: direct-memory closed form','stable_blocks: cache fixed-point proof','Every unsupported combination fails'],PURPLE)
f.arrow([(438,273),(524,273)])
f.arrow([(914,273),(1000,273)],GREEN)
f.box(48,455,625,225,'Cache path  ·  Hierarchy::submit / access',['Admission waits for the previous cache request.','Each level: split lines → lookup → hit or full-line miss.','Misses recurse down; completion and fills return upward.','Inclusive eviction can invalidate upper copies.','This is the bounded stateful fallback.'])
f.box(766,455,626,225,'Memory path  ·  MemoryService::submit',['FIFO offered requests; finite in-flight credits.','Beat demand sets duration and initiation spacing.','Direct memory injection bypasses the cache gate.','Independent requests can overlap in flight.','periodic_memory solves eligible streams algebraically.'],GREEN)
f.arrow([(1195,375),(1195,416),(1080,416),(1080,455)],PURPLE)
f.arrow([(1100,416),(350,416),(350,455)],PURPLE)
f.arrow([(673,557),(766,557)])
f.text(683,539,'miss',16,BLUE)
f.box(48,755,625,220,'Reports and preserved state',['Per-level hits / misses / evictions / invalidations / fills','Requested bytes and memory wire bytes stay distinct.','Completion, latency and throughput include defined boundaries.','Cache MRU order; memory issue readiness and pending credits','Effective config and provenance travel with each report.'],GREEN)
f.box(766,755,626,220,'Independent correctness checks',['Timestamp-based resident maps check cache behavior.','Tiny discrete-tick reference checks memory admissions.','Compare final state and future continuation, not just totals.','Analytical skips are counted separately from evaluated rows.','Hardware timing validation remains a later gate.'],ORANGE)
f.arrow([(360,680),(360,755)])
f.arrow([(1080,680),(1080,720),(600,720),(600,755)],GREEN)
f.save('config.cpp • hierarchy.cpp • service.cpp • testbench.cpp • hierarchy_tests.cpp')

f=Figure('02-cache-hierarchy.svg',2,'One request through the cache hierarchy','Blocking read path, full-line allocation, LRU replacement and inclusion',1280)
f.box(48,177,414,172,'1  Admit a request',['start = max(arrival, ready)','ready is prior request completion.','Validate width, ordering and entry.'])
f.box(513,177,414,172,'2  Split into line references',['first = floor(address / line_bytes)','last = floor((address+bytes−1)/L)','Process lines in ascending order.'])
f.box(978,177,414,172,'3  Look up this level',['set = line mod sets','Charge lookup_cycles per line.','Search the selected resident set.'])
f.arrow([(462,260),(513,260)])
f.arrow([(927,260),(978,260)])
f.box(48,425,590,177,'HIT  ·  no lower request',['Increment hits; remove the old MRU position.','Insert the line at MRU; completion = lookup_done.','A hit does not refresh replacement order below.'],GREEN)
f.box(800,425,592,177,'MISS  ·  fetch the entire line',['Increment misses; base = line * line_bytes.','Recurse to the next level, or submit to memory.','Wait for the complete response; add fill_cycles.'],ORANGE)
f.arrow([(1185,349),(1185,389),(345,389),(345,425)],GREEN)
f.text(548,378,'resident',16,GREEN)
f.arrow([(1260,349),(1260,425)],ORANGE)
f.text(1277,395,'absent',16,ORANGE)
f.box(800,657,592,181,'After the lower response',['Count this level’s full-line fill bytes.','Choose a victim only now: recursion may invalidate lines.','If full: evict LRU; otherwise use available space.','Install at MRU; repeat for the next crossing line.'],ORANGE)
f.arrow([(1100,602),(1100,657)],ORANGE)
f.box(48,657,590,181,'Completion and accounting',['After all lines: ready = done; return Completion.','A later cache request cannot pass this completion.','References = hits + misses at each level.','Invalidations and local evictions are separate counters.'],GREEN)
f.arrow([(345,602),(345,657)],GREEN)
f.arrow([(800,755),(638,755)],GREEN)
f.box(48,901,650,231,'Inclusive example  ·  lower line = 32 bytes',['Lower line 0 covers byte addresses 0…31.','Upper 16-byte lines 0 and 1 both lie inside that span.','Evict lower line 0 → invalidate both upper copies.','Surviving upper lines retain their relative MRU order.','Invalidation is atomic with no extra charged latency.'],PURPLE)
f.box(744,901,648,231,'Noninclusive policy and restrictions',['Lower eviction leaves upper copies untouched.','This is not an exclusive / victim-cache organization.','Lower line sizes must be multiples of upper sizes.','No writes, dirty lines, prefetch or hit-under-miss.','Lookup / fill costs are supplied synthetic parameters.'],INK)
f.save('src/hierarchy.cpp: Hierarchy::submit, access, invalidate_above')

f=Figure('03-cache-mathematics.svg',3,'Cache geometry, reuse and the sweep formula','The original memory algebra module complements the stateful hierarchy',1200)
f.box(48,180,650,210,'Byte address → line → set',['line(A) = floor(A/L); set(A) = line(A) mod S','For n > 0: touched = floor((A+n−1)/L) − floor(A/L) + 1','For n = 0: touched = 0','Example: A=15, n=4, L=16 touches lines 0 and 1.','Touched lines alone do not determine cache misses.'])
f.box(744,180,648,210,'Set-aware LRU reuse',['D_set counts distinct intervening lines in the same set.','A repeated line hits exactly when D_set < W.','S=2, W=2: 0 → 2 → 1 → 0 ends in a hit.','Only line 2 competes with line 0; line 1 maps elsewhere.','0 → 2 → 4 → 0 ends in a miss: D_set = 2.'],GREEN)
f.box(48,443,650,235,'analyze_reuse  ·  finite analysis',['Keep an unlimited recency stack for each set.','Position in that stack is the same-set reuse distance.','Cold or explicit initial residents; reads only.','Final residents are the first W entries of each stack.','Default expansion budget: 10,000 line references.','This still processes accesses; it is not symbolic FC analysis.'])
f.box(744,443,648,235,'analyze_cold_sweep  ·  closed form',['N consecutive lines, repeated R times, initially empty.','Let k_s be the distinct line count in set s.','First sweep misses N times.','Later sweeps: all hit if k_s ≤ W; all miss if k_s > W.','M = N + (R−1) * sum(k_s for k_s > W)','Final set = its last min(k_s,W) lines, newest first.'],PURPLE)
f.box(48,739,650,283,'Worked sweep  ·  S=2, W=2, lines 0…4',['Sequence per sweep: 0 → 1 → 2 → 3 → 4','Set 0 sees {0,2,4}: 3 competitors exceed 2 ways.','Set 1 sees {1,3}: both fit in 2 ways.','Three sweeps: 15 references, 11 misses, 4 hits.','Final MRU→LRU: set 0 [4,2]; set 1 [3,1].','Evictions = misses − residents = 11 − 4 = 7.','Fill bytes = misses * L; no prefetch or partial fill.'],ORANGE)
f.box(744,739,648,283,'What is and is not accelerated',['Sweep counts need no N*R access vector.','Runtime: O(S + sum min(k_s,W)) for counts plus state.','One billion sweeps of 5 lines → 3,000,000,002 misses.','The paired-row FC source schedule is not this simple sweep.','Hierarchy stable_blocks uses a different fixed-state proof.','Independent replay validates counts and outgoing state.','Neither agreement nor exact arithmetic proves ART behavior.'],INK)
f.save('src/memory.cpp: contiguous_lines, analyze_reuse, analyze_cold_sweep • tests/memory_tests.cpp')

f=Figure('04-memory-service.svg',4,'FIFO memory service and in-flight credits','Latency is distinct from initiation spacing and external queueing delay',1260)
f.box(48,180,650,253,'1  Derive one request’s demand',['Inputs: address A, width n, offered arrival a','B = beat_bytes; τ = beat_cycles; L = first_beat_cycles','b = ceil(((A mod B) + n) / B)','D = L + (b−1)*τ                       duration in cycles','I = max(issue_cycles, b*τ)          initiation spacing','wire_bytes = b*B                     transferred bytes'],BLUE)
f.box(744,180,648,253,'2  State before admission',['next_issue: earliest next start allowed by resource use','pending[]: sorted completion ticks occupying credits','last_arrival: preserves offered FIFO ordering','K = outstanding: maximum in-flight requests','The offered queue may wait without bound.','K is not the capacity of the external waiting queue.'],PURPLE)
f.box(48,493,650,237,'3  Find the admission tick',['s = max(a, next_issue)','Remove pending completions at or before s.','If all K credits are occupied:','    s = earliest pending completion; release again.','Equal-tick completions release credits before admissions.','Offered arrivals must be nondecreasing.'],GREEN)
f.box(744,493,648,237,'4  Complete and preserve state',['d = s + D','next_issue = s + I','Insert d into sorted pending[]; record last_arrival = a.','Return {arrival=a, start=s, done=d, wire_bytes}.','Queueing delay = s−a; response latency = d−a.','Validate arithmetic before committing service state.'],GREEN)
f.arrow([(372,433),(372,493)])
f.arrow([(1068,433),(1068,463),(653,463),(653,493)],PURPLE)
f.arrow([(698,611),(744,611)],GREEN)
f.box(48,793,650,259,'Aligned 32-byte read  ·  synthetic profile',['B=8 bytes; τ=2 cycles; L=12 cycles; issue_cycles=2','b=4 beats → D=12+3*2=18 cycles','I=max(2,4*2)=8 cycles; wire_bytes=32','K=4 allows overlap; K=1 forces serialized completions.','A=7, n=2 would touch 2 beats: wire_bytes=16.','An access can consume more bus bytes than useful bytes.'],ORANGE)
f.box(744,793,648,259,'Model limits and composition',['One FIFO stream, one clock, fixed first-beat latency.','No bank conflicts, refresh, priorities or row-hit rules.','No write data or command/address bus traffic.','Direct memory tests expose pipelining.','The cache path waits for full responses and is blocking.','Save next_issue, pending and last_arrival for continuation.'],INK)
f.save('src/service.cpp: demand, MemoryService::submit • include/edge/service.hpp')

f=Figure('05-timing-examples.svg',5,'Read the timing boundaries correctly','Synthetic 32-byte reads: D=18 cycles, I=8 cycles, 100 MHz clock',1310)
f.text(48,191,'A  Direct memory: four requests offered together at tick 0',25,INK,700)
f.text(48,222,'K=4 credits: starts at 0, 8, 16, 24; completes at 18, 26, 34, 42.',20,MUTED)
x0,scale=245,24
for tick in range(0,45,4):
    x=x0+tick*scale
    f.parts.append(f'<path d="M{x} 260 V 493" stroke="#dbe3ec"/>')
    f.text(x-7,254,str(tick),16,MUTED)
for i,s in enumerate([0,8,16,24]):
    y=282+i*51
    f.text(48,y+24,f'Request {i}',20)
    f.bar(x0+s*scale,y,18*scale,31,f'{s} → {s+18}',BLUE)
    # Circles indicate response beats, distinct from the entire in-flight interval.
    for beat in [12,14,16,18]:
        f.parts.append(f'<circle cx="{x0+(s+beat)*scale}" cy="{y+15}" r="5" fill="#f0c36a"/>')
f.text(245,524,'Blue bar = in-flight interval. Gold dots = beat completions. Axis unit = cycles.',19,MUTED)
f.text(48,582,'B  One credit: later requests cannot start until the prior one completes',25,INK,700)
f.lines(48,620,['K=1 → starts 0, 18, 36, 54; last completion 72 cycles for four requests.',
                 'Idle-spaced requests → each has 18-cycle latency, with no queueing.',
                 'Saturated K=4 → asymptotic useful rate = 32 bytes / 8 cycles = 400 MB/s at 100 MHz.'],21,34)
f.rule(733)
f.text(48,780,'C  A cold L1 miss: serial lookup, memory and installation costs',25,INK,700)
f.text(48,816,'The cache example follows configs/hierarchy.ini; the request is 4 bytes, but memory fills 32 bytes.',20,MUTED)
segments=[('L1 lookup',1,BLUE),('L2 lookup',3,PURPLE),('Memory response',18,GREEN),('L2 fill',2,ORANGE),('L1 fill',1,BLUE)]
x=48
for label,d,color in segments:
    w=1344*d/25
    f.bar(x,850,w-3,53,'',color)
    x+=w
f.lines(48,940,['L1 lookup 1  +  L2 lookup 3  +  memory 18  +  L2 fill 2  +  L1 fill 1  =  25 cycles',
                 'L1 hit = 1 cycle. L2 hit with L1 miss = 1 + 3 + 1 = 5 cycles.',
                 'Finite bandwidth uses total useful bytes / (last completion − first offered arrival).',
                 'It includes startup and drain; growing queueing delay is not steady-state latency.'],22,39)
f.save('configs/hierarchy.ini • configs/memory-latency.ini • configs/memory-bandwidth.ini • tests/hierarchy_tests.cpp')

f=Figure('06-analytical-acceleration.svg',6,'Two analytical fast paths, two different proofs','No trace expansion for periodic memory; no extrapolation without cache-state equality',1310)
f.box(48,180,650,231,'periodic  ·  direct memory only',['Cold service; same address and width for every request.','Open-loop arrivals: a_i = a0 + i*P','D, I and K are constant; request index i starts at 0.','s_i = max(a_i, s_(i−1)+I, s_(i−K)+D)','Omit nonexistent predecessor terms.'],GREEN)
f.box(744,180,648,231,'stable_blocks  ·  cache entry only',['Closed-loop generated requests; one fixed address block.','A_i = base + (i mod address_period)*stride','Snapshot at a drained boundary before one full block.','Evaluate the block using the finite hierarchy path.','Compare the complete normalized boundary state.'],BLUE)
f.arrow([(373,411),(373,461)],GREEN)
f.arrow([(1068,411),(1068,461)])
f.box(48,461,650,237,'Closed-form solution',['J = max(P,I); H = max(K*J,D)','s_i = a0 + floor(i/K)*H + (i mod K)*J','d_i = s_i + D','Each group of K admissions needs at least max(K*J,D).','Closed-form quotient/remainder sums yield mean latency.','Counts O(1); pending-state construction O(min(N,K)).'],GREEN)
f.box(744,461,648,237,'Proof obligation: identical state',['All levels, sets, line identities and MRU ordering match.','Residual issue readiness matches relative to the boundary.','No completion outlives the drained boundary.','Configuration and the next stimulus block are identical.','Equal state → equal future deltas by determinism.','Equal miss counts alone are not sufficient.'],BLUE)
f.arrow([(373,698),(373,750)],GREEN)
f.arrow([(1068,698),(1068,750)])
f.box(48,750,650,247,'Periodic output and example',['N=1,000,000,000; D=18; I=8; K=4; P=0','last_done = 18 + (N−1)*8 = 8,000,000,010','No request rows are enumerated or fabricated.','Preserve next issue time, pending and last arrival.','Future service must agree with finite continuation.','Parameters are synthetic, not STM32 flash data.'],GREEN)
f.box(744,750,648,247,'Skip only after proof',['Multiply one-block counter / duration / latency deltas.','Translate readiness to the new boundary.','If equality fails, continue finite work within its budget.','One billion cache requests: 8 evaluated, rest skipped.','A repeating conflict-miss pattern may also be eligible.','Open-loop growth and multi-block cycles are not skipped.'],BLUE)
f.box(48,1045,1344,130,'Evidence boundary',['These are project derivations under the stated contracts, not a full Falcon implementation.','Independent replay checks counts, timing, final state and future continuation.'],ORANGE)
f.save('src/service.cpp: periodic_memory • src/testbench.cpp: run_testbench • src/hierarchy.cpp: boundary_key')

f=Figure('07-configuration.svg',7,'Configuration is a validated model contract','A strict INI dialect with explicit units, provenance and unsupported-policy rejection',1250)
f.box(48,180,390,200,'1  Read and tokenize',['[model], [memory], [stimulus]','[cache.NAME] in level order','key=value; # comments','Unsigned decimal or 0x integers','1 MiB file / 4096-byte line cap'])
f.box(524,180,390,200,'2  Consume the schema',['Every key is required.','Duplicate sections/keys fail.','Unused keys reveal typos.','No implicit physical defaults.','Version must be 1.'],GREEN)
f.box(1000,180,392,200,'3  Validate relationships',['Positive dimensions / timings','Nested cache line sizes','Initial-state capacity / inclusion','Entry / mode / engine contract','Checked address/count arithmetic'],PURPLE)
f.arrow([(438,278),(524,278)])
f.arrow([(914,278),(1000,278)],GREEN)
f.box(48,448,650,231,'Hardware and policy knobs',['model: clock_hz; inclusive or noninclusive','Policies: read_only, no prefetch, blocking caches','cache: line_bytes / sets / ways; LRU replacement','cache: lookup_cycles / fill_cycles / initial_lines','memory: first_beat_cycles / beat_bytes / beat_cycles','memory: issue_cycles / outstanding; FIFO'],BLUE)
f.box(744,448,648,231,'Stimulus and budget knobs',['entry: a configured cache name, or memory','mode: open_loop or closed_loop','engine: finite, periodic or stable_blocks','count / address / bytes / stride / address_period','arrival_cycles / spacing_cycles','max_requests / max_transactions'],GREEN)
f.box(48,735,650,281,'State and provenance are inputs',['initial_lines are line numbers, not byte addresses.','List in MRU→LRU order within each set.','Inclusive initial copies must exist at lower levels.','Module provenance text is mandatory.','Scenario values are explicit hypothetical choices.','The report prints the effective configuration.','It should be possible to reproduce every result.'],PURPLE)
f.box(744,735,648,281,'Reject before executing stimulus',['Unknown keys, missing keys and unsupported versions','Unsupported replacement, prefetch or concurrency','Zero/overflowing geometry; invalid addresses and widths','Invalid entry or engine/mode combinations','Duplicate/overfull/inconsistent initial state','Parser diagnostics identify source, key and/or line.','No includes, environment expansion or external schemas.'],ORANGE)
f.text(48,1080,'Typed output: ModelConfig → CacheConfig[] + ServiceConfig + StimulusConfig',24,INK,700)
f.save('src/config.cpp: parse_config, validate_config, describe_config • include/edge/config.hpp')

f=Figure('08-testbench.svg',8,'Direct-stimulus testbench modes','Inject at L1, another configured cache level, or memory — without a CPU frontend',1350)
f.box(48,180,650,192,'Generated stimulus',['A_i = base + (i mod period)*stride','Open loop: a_i = a0 + i*spacing','Closed loop: next arrival = prior completion + spacing','Changing a timing knob can change closed-loop arrivals.'])
f.box(744,180,648,192,'Explicit CSV stimulus',['Header: arrival_cycles,address,bytes','Replaces generated arrival, address, width and count.','Requires finite + open_loop; equal ticks preserve order.','Valid config, selected entry and budgets still apply.'],GREEN)
f.box(48,440,414,231,'finite',['Cache or direct-memory entry','Generated open / closed loop','CSV only in open loop','Bounded requests / transactions','Emit each evaluated completion.','Use for directed irregular cases.'],BLUE)
f.box(513,440,414,231,'periodic',['Direct memory entry only','Generated open loop only','Same address and request width','Cold service state','Closed-form counters and timing','No individual request replay'],GREEN)
f.box(978,440,414,231,'stable_blocks',['Configured cache entry only','Generated closed loop only','One repeating address block','Compare full boundary state.','Skip only a proven fixed point.','Otherwise bounded fallback'],PURPLE)
f.arrow([(369,372),(369,409),(255,409),(255,440)])
f.arrow([(369,409),(719,409),(719,440)],GREEN)
f.arrow([(719,409),(1185,409),(1185,440)],PURPLE)
f.arrow([(1080,372),(1080,391),(477,391),(477,556),(462,556)],GREEN,True)
f.box(48,736,650,216,'Cache injection',['entry=L1 → blocking cache path → misses to L2 → memory','entry=L2 bypasses L1 lookups, not inclusion effects.','Initial residents or repeated blocks exercise hit behavior.','Conflict patterns exercise misses and back-invalidations.','One fixed entry per run; mixed-source arbitration is absent.'],BLUE)
f.box(744,736,648,216,'Direct-memory injection',['entry=memory bypasses the cache admission gate.','Wide arrival gaps isolate unloaded latency.','Tight arrivals / a burst expose loaded bandwidth.','Vary credits to reveal latency-limited throughput.','Inspect response latency separately from service duration.'],GREEN)
f.box(48,1013,1344,187,'Observe and validate',['Report useful bytes, wire bytes, completion ticks, latency and drain-inclusive bandwidth.','Track per-level hit/miss/eviction/invalidation/fill counts and complete outgoing state.','Keep evaluated and analytically skipped requests separate; never invent skipped request rows.','Compare with the independent timestamp-cache and discrete-tick memory references.'],ORANGE)
f.save('examples/edge_bench.cpp • src/testbench.cpp • configs/*.ini • configs/reads.csv • tests/hierarchy_tests.cpp')

f=Figure('09-roofline-and-evidence.svg',9,'Traffic, roofline bounds and evidence','The bound calculator remains separate from the request-completion model',1170)
f.box(48,180,414,210,'Work F',['Derived from the pinned FC source','Unit: MACs, one MAC per multiply-add','Semantic work is not issue time.','Compute ceiling P uses MAC/s.','Unknown P remains unknown.'],BLUE)
f.box(513,180,414,210,'Traffic Q at a boundary',['Unit: bytes at a named resource','Example: cache full-line fills','Example: SRAM tensor requests','Storage bytes are not traffic.','Use B in bytes/s at that boundary.'],GREEN)
f.box(978,180,414,210,'Independent dependencies',['If derived, supply T_dependency.','Unit: seconds, explicit chain','Do not assume all misses serialize.','Do not charge the same transfer','as both extra latency and bandwidth.'],PURPLE)
f.box(48,465,1344,186,'Necessary bounds, not a predicted execution time',['Intensity I_r = F / Q_r  [MAC/byte] when Q_r > 0','T ≥ max(F/P, max_r(Q_r/B_r), T_dependency)  [seconds]','Taking a maximum permits ideal overlap; the real dependency schedule can take longer.','The largest term is the active modeled constraint, not proof of the silicon bottleneck.'],GREEN)
f.arrow([(255,390),(255,465)])
f.arrow([(720,390),(720,465)],GREEN)
f.arrow([(1185,390),(1185,465)],PURPLE)
f.box(48,715,650,268,'Worked FC bound  ·  hypothetical ceilings',['Tiny FC: F=15 MACs; flash fills Q=32 bytes.','Flash intensity = 15/32 = 0.46875 MAC/byte.','P=100 million MAC/s → compute bound 0.15 μs.','Flash B=20 million bytes/s → bound 1.6 μs.','SRAM Q=13 bytes, B=100 million bytes/s → 0.13 μs.','Combined known lower bound = 1.6 μs.','These input ceilings are not L4R5 measurements.'],ORANGE)
f.box(744,715,648,268,'How to read the classifications',['Exact under abstraction: supported counters / tick rules','Conditional bound: depends on supplied valid ceilings','Unknown: a required physical input has no justified value','Unsupported: the selected mechanism is not implemented','Measured: requires a recorded physical observation','complete_roofline only means supplied ceilings exist.','Unit-test agreement does not establish hardware accuracy.'],INK)
f.save('src/memory.cpp: roofline_bound • examples/memory_example.cpp • docs/memory-models.md')

print(f'Wrote {len(list(OUT.glob("*.svg")))} SVG study diagrams to {OUT}')
