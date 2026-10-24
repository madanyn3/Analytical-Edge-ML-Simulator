#!/usr/bin/env python3
"""Draw the A4 approximation figure from fc_cache_study CSV, as a dependency-free SVG.

usage: plot_fc_study.py study.csv out.svg
Panels: W = 1 and W = 2, cold-call misses against n. Shaded columns mark n where the
exact analysis finds cursor re-misses (the paired-row resonance of Theorem 3.3).
"""
import csv
import sys

rows = [r for r in csv.DictReader(l for l in open(sys.argv[1]) if not l.startswith('#'))]
L, S, m = rows[0]['L'], rows[0]['S'], rows[0]['m']
WIDTH, PANEL_H, TOP, LEFT, RIGHT = 1440, 380, 190, 110, 60
COLORS = {'exact': '#142b43', 'fa': '#2465a8', 'rs': '#a75616'}
out = []
add = out.append
height = TOP + 2 * (PANEL_H + 70) + 60
add(f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{height}" viewBox="0 0 {WIDTH} {height}" role="img" aria-labelledby="title desc">')
add('<title id="title">Exact FC cache misses against statistical approximations</title>')
add('<desc id="desc">Cold-call flash misses of the CMSIS-NN FC source schedule under the synthetic '
    'set-associative LRU contract, exact (Theorem A1) versus a fully associative LRU of equal '
    'capacity and the random-set-mapping model. Not an L4R5 ART claim.</desc>')
add('<style>text{font-family:Arial,Helvetica,sans-serif} .mono{font-family:Consolas,monospace}</style>')
add(f'<rect width="{WIDTH}" height="{height}" fill="#fbfcfe"/>')
add(f'<rect width="{WIDTH}" height="128" fill="#142b43"/>')
add('<text x="48" y="52" font-size="30" fill="#ffffff" font-weight="bold">Paired-row resonance: exact misses vs. statistical models</text>')
add(f'<text x="48" y="88" font-size="17" fill="#c9d6e6">FC flash stream, one cold call, m = {m} rows, L = {L} B lines, S = {S} sets, '
    f'n swept over two S·L periods. Synthetic contract; not an L4R5 ART measurement.</text>')
for panel, W in enumerate(('1', '2')):
    pr = [r for r in rows if r['W'] == W]
    y0 = TOP + panel * (PANEL_H + 70)
    xs = [int(r['n']) for r in pr]
    series = {'exact': [float(r['exact_cold']) for r in pr],
              'fa': [float(r['fa_cold']) for r in pr],
              'rs': [float(r['rs_cold']) for r in pr]}
    ymax = max(max(v) for v in series.values()) * 1.08
    x_lo, x_hi = min(xs), max(xs)
    def px(n): return LEFT + (n - x_lo) / (x_hi - x_lo) * (WIDTH - LEFT - RIGHT)
    def py(v): return y0 + PANEL_H - v / ymax * PANEL_H
    add(f'<text x="{LEFT}" y="{y0 - 14}" font-size="19" font-weight="bold" fill="#142b43">W = {W} '
        f'({"direct-mapped" if W == "1" else "2-way"})</text>')
    step = (WIDTH - LEFT - RIGHT) / (x_hi - x_lo)
    for r in pr:
        if int(r['cursor_re_misses']):
            add(f'<rect x="{px(int(r["n"])) - step / 2:.1f}" y="{y0}" width="{step:.1f}" height="{PANEL_H}" fill="#f3dfcf"/>')
    add(f'<rect x="{LEFT}" y="{y0}" width="{WIDTH - LEFT - RIGHT}" height="{PANEL_H}" fill="none" stroke="#9aa8b8"/>')
    tick = 10 ** max(0, len(str(int(ymax))) - 1)
    if ymax / tick < 4: tick //= 2 if tick > 1 else 1
    v = 0
    while v <= ymax:
        add(f'<line x1="{LEFT}" x2="{WIDTH - RIGHT}" y1="{py(v):.1f}" y2="{py(v):.1f}" stroke="#e3e8ef"/>')
        add(f'<text x="{LEFT - 10}" y="{py(v) + 5:.1f}" font-size="14" text-anchor="end" fill="#3b4a5c">{v}</text>')
        v += tick
    for n in range(x_lo - x_lo % 32 + 32, x_hi + 1, 32):
        add(f'<line x1="{px(n):.1f}" x2="{px(n):.1f}" y1="{y0 + PANEL_H}" y2="{y0 + PANEL_H + 6}" stroke="#3b4a5c"/>')
        add(f'<text x="{px(n):.1f}" y="{y0 + PANEL_H + 24}" font-size="14" text-anchor="middle" fill="#3b4a5c">{n}</text>')
    for key, dash, width in (('rs', '7 5', 2.2), ('fa', '3 4', 2.2), ('exact', '', 2.6)):
        pts = ' '.join(f'{px(n):.1f},{py(y):.1f}' for n, y in zip(xs, series[key]))
        add(f'<polyline points="{pts}" fill="none" stroke="{COLORS[key]}" stroke-width="{width}"'
            + (f' stroke-dasharray="{dash}"' if dash else '') + '/>')
    add(f'<text x="{LEFT - 70}" y="{y0 + PANEL_H / 2}" font-size="15" fill="#3b4a5c" transform="rotate(-90 {LEFT - 70} {y0 + PANEL_H / 2})" text-anchor="middle">misses per call</text>')
    add(f'<text x="{(LEFT + WIDTH - RIGHT) / 2}" y="{y0 + PANEL_H + 48}" font-size="15" text-anchor="middle" fill="#3b4a5c">row length n (bytes)</text>')
ly = height - 34
legend = [('exact', '', 'exact (Theorem A1, set-aware)', 320), ('fa', '3 4', 'fully associative LRU, S·W lines', 330),
          ('rs', '7 5', 'random set mapping, P(Bin(D, 1/S) &lt; W)', 370)]
x = LEFT
for key, dash, label, advance in legend:
    add(f'<line x1="{x}" x2="{x + 40}" y1="{ly}" y2="{ly}" stroke="{COLORS[key]}" stroke-width="3"' + (f' stroke-dasharray="{dash}"' if dash else '') + '/>')
    add(f'<text x="{x + 50}" y="{ly + 5}" font-size="15" fill="#142b43">{label}</text>')
    x += advance
add(f'<rect x="{x}" y="{ly - 9}" width="18" height="18" fill="#f3dfcf"/><text x="{x + 26}" y="{ly + 5}" font-size="15" fill="#142b43">cursor re-misses (resonance)</text>')
add('</svg>')
open(sys.argv[2], 'w').write('\n'.join(out) + '\n')
