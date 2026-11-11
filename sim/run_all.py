"""Run every scenario + Monte Carlo, write plots and out/summary.md.   usage: python3 run_all.py [--mc N]"""
import argparse
import math
import os
import random
import statistics
import time

import matplotlib

matplotlib.use('Agg')
import matplotlib.pyplot as plt

import params as P
from scenarios import SCENARIOS, LOAD_CYCLE
from twin import Scenario, Twin

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out')

# reference palette (dataviz skill, light mode): categorical slots 1-4 in fixed order, text inks, surface
CELL_COLORS = ['#2a78d6', '#eb6834', '#1baf7a', '#eda100']
INK, INK2, MUTED, GRID, SURFACE = '#0b0b0b', '#52514e', '#8a8984', '#e6e5e0', '#fcfcfb'
plt.rcParams.update({'font.size': 9, 'axes.edgecolor': MUTED, 'axes.labelcolor': INK2, 'xtick.color': INK2,
                     'ytick.color': INK2, 'axes.facecolor': SURFACE, 'figure.facecolor': SURFACE,
                     'axes.grid': True, 'grid.color': GRID, 'grid.linewidth': 0.6, 'axes.spines.top': False,
                     'axes.spines.right': False, 'text.color': INK})
LW = 1.5  # ~2 px at 100 dpi


def end_labels(ax, t, series, names):
    """direct labels at the line ends, nudged apart so they never overlap; text in ink, not series colour"""
    ends = []
    for k, ys in enumerate(series):
        y = next((v for v in reversed(ys) if v == v), None)
        if y is not None:
            ends.append([y, names[k]])
    if not ends:
        return
    lo, hi = ax.get_ylim()
    gap = (hi - lo) * 0.075
    ends.sort()
    for i in range(1, len(ends)):
        if ends[i][0] - ends[i - 1][0] < gap:
            ends[i][0] = ends[i - 1][0] + gap
    for y, name in ends:
        ax.text(t[-1] + (t[-1] - t[0]) * 0.008, y, name, color=INK2, va='center', fontsize=8, clip_on=False)


def plot(sc, r, path):
    rec = r['rec']
    t = rec['t']
    names = ['Cell %d' % (k + 1) for k in range(P.N)]
    if sc.dummy is not None:
        names[sc.dummy] = 'Seg %d (dummy)' % (sc.dummy + 1)
    cols = lambda key: [[row[k] for row in rec[key]] for k in range(P.N)]
    fig, axes = plt.subplots(5, 1, figsize=(10, 10.5), sharex=True, gridspec_kw={'height_ratios': [1.2, 1.2, 1.1, 0.8, 0.7]})
    fig.subplots_adjust(left=0.08, right=0.88, top=0.885, bottom=0.06, hspace=0.28)
    fig.suptitle(sc.title, x=0.08, ha='left', fontsize=12, color=INK, fontweight='bold')
    fig.text(0.08, 0.925, 'VentSense digital twin: real firmware safety core in the loop. Synthetic fault profile, '
             'ASSUMED model values (sim/params.py).', fontsize=8.5, color=INK2)

    gas = [[v * 100 for v in s] for s in cols('gas')]
    temp = cols('temp')
    volt = [[(v if v == v else float('nan')) for v in s] for s in cols('cell_v')]
    if sc.dummy is not None:  # a dummy segment has no cell: leave it out of the voltage panel
        volt[sc.dummy] = [float('nan')] * len(t)
    panels = [(axes[0], gas, 'Gas rise above baseline (%)\nlarger of MQ-7 / MQ-8'),
              (axes[1], temp, 'DS18B20 temperature (°C)'),
              (axes[2], volt, 'Cell voltage (V)\nfrom ADS1115 taps')]
    for ax, data, lab in panels:
        for k in range(P.N):
            ax.plot(t, data[k], color=CELL_COLORS[k], lw=LW, label=names[k])
        ax.set_ylabel(lab)
    axes[0].axhline(30, color=MUTED, lw=1, ls='--')
    axes[0].text(t[0], 31.5, 'gas threshold 30 % (placeholder)', color=INK2, fontsize=7.5)
    axes[1].axhline(50, color=MUTED, lw=1, ls='--')
    axes[1].text(t[0], 51, 'temperature threshold 50 °C (placeholder)', color=INK2, fontsize=7.5)
    if sc.heater:
        axes[1].plot(t, rec['block'], color=MUTED, lw=1.2, label='heater block (model)')
        axes[1].axhline(55, color=INK2, lw=0.8, ls=':')
        axes[1].axhline(70, color=INK2, lw=0.8, ls=':')
        axes[1].text(t[-1], 55.5, '55 °C firmware cutoff ', ha='right', color=INK2, fontsize=7.5)
        axes[1].text(t[-1], 70.5, 'KSD301 70 °C ', ha='right', color=INK2, fontsize=7.5)
    vmin = min((v for s in volt for v in s if v == v), default=3.0)
    axes[2].set_ylim(max(0.0, vmin - 0.1), 4.25)
    for i, (ax, data, _) in enumerate(panels):
        if i == 1 and sc.heater:
            end_labels(ax, t, data + [rec['block']], names + ['heater block (model)'])
        else:
            end_labels(ax, t, data, names)

    axes[3].plot(t, rec['current'], color=INK, lw=LW)
    axes[3].set_ylabel('Pack current (A)')
    axes[3].set_ylim(-0.05, max(1.8, max(rec['current']) * 1.1))
    axes[4].step(t, rec['level'], where='post', color=INK, lw=LW)
    axes[4].set_ylim(-0.3, 3.4)
    axes[4].set_yticks([0, 1, 2, 3])
    axes[4].set_yticklabels(['Normal', 'L1 alert', 'L2 cell', 'L3 pack'])
    axes[4].set_ylabel('Level')
    axes[4].set_xlabel('Time (s)')

    for k, tt in sorted(r['trips'].items()):
        for ax in axes:
            ax.axvline(tt, color=CELL_COLORS[k], lw=1, ls='--')
        lat = r['latency'].get(k)
        txt = '%s isolated at %.0f s\n(%s)' % (names[k], tt, ' + '.join(r['trip_signals'][k]))
        if lat is not None:
            txt += ', %.1f s after 2nd signal' % lat
        axes[4].annotate(txt, (tt, 2), xytext=(6, 4 + 22 * list(sorted(r['trips'])).index(k)), textcoords='offset points',
                         fontsize=7.5, color=INK2)
    for tt in r['l3']:
        for ax in axes:
            ax.axvline(tt, color=INK, lw=1.2)
        axes[4].annotate('Level 3: pack relay open at %.0f s' % tt, (tt, 3), xytext=(6, -12), textcoords='offset points',
                         fontsize=7.5, color=INK2)
    axes[0].legend(loc='upper left', ncol=5, frameon=False, fontsize=8, bbox_to_anchor=(0, 1.22))
    fig.savefig(path, dpi=150)
    plt.close(fig)


def monte_carlo(n):
    rng = random.Random(42)
    normal = Scenario('mc_normal', '', 900, load=LOAD_CYCLE)
    false_l2, l1_runs = 0, 0
    for i in range(n):
        r = Twin(normal, seed=1000 + i).run()
        false_l2 += 1 if r['trips'] else 0
        l1_runs += 1 if r['max_level'] >= 1 else 0
    det, lat, wrong, missed = [], [], 0, 0
    for i in range(n):
        cell = rng.randrange(P.N)
        ramp_s, scale = rng.uniform(120, 600), rng.uniform(0.7, 1.3)
        onset = 300.0
        sc = Scenario('mc_fault', '', 1200, faults=[(cell, onset, ramp_s, scale)])
        r = Twin(sc, seed=2000 + i).run()
        if cell in r['trips']:
            det.append(r['trips'][cell] - onset)
            if cell in r['latency']:
                lat.append(r['latency'][cell])
        else:
            missed += 1
        if set(r['trips']) - {cell}:
            wrong += 1
    return dict(n=n, false_l2=false_l2, l1_runs=l1_runs, det=det, lat=lat, wrong=wrong, missed=missed)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mc', type=int, default=40, help='Monte Carlo runs per set (0 = skip)')
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    t0 = time.time()
    rows, total, passed = [], 0, 0
    for sc in SCENARIOS:
        r = Twin(sc, seed=1).run()
        plot(sc, r, os.path.join(OUT, sc.name + '.png'))
        for name, ok, detail in sc.expect(r):
            total += 1
            passed += ok
            rows.append((sc.name, name, ok, detail))
    lines = ['# VentSense digital twin: results', '',
             'Generated by `sim/run_all.py`. The **real firmware safety core** (`firmware/lib/safety_core`) runs in the loop;',
             'the plant, sensors and fault profiles are models with ASSUMED values (`sim/params.py`).',
             'These results show the firmware logic and timing behave as designed against modelled faults.',
             'They do not prove detection on real cells. Thresholds are placeholders to be tuned on the rig.', '',
             '## Scenario checks: %d / %d passed' % (passed, total), '',
             '| Scenario | Check | Result | Detail |', '|---|---|---|---|']
    for sc_name, name, ok, detail in rows:
        lines.append('| %s | %s | %s | %s |' % (sc_name, name, 'PASS' if ok else '**FAIL**', detail))
    lines += ['', 'Plots: `out/<scenario>.png`.', '']
    if a.mc:
        m = monte_carlo(a.mc)
        q = lambda xs, p: sorted(xs)[min(len(xs) - 1, int(p * len(xs)))] if xs else float('nan')
        lines += ['## Monte Carlo (%d runs each, random sensor offsets, baselines, noise, cell spread)' % m['n'], '',
                  '| Metric | Result |', '|---|---|',
                  '| Normal operation: runs with a false Level 2 trip | %d / %d |' % (m['false_l2'], m['n']),
                  '| Normal operation: runs with any Level 1 alert | %d / %d |' % (m['l1_runs'], m['n']),
                  '| Failing cell (random cell, ramp 120-600 s, severity 0.7-1.3): detected and isolated | %d / %d |'
                  % (m['n'] - m['missed'], m['n']),
                  '| Wrong cell isolated | %d / %d |' % (m['wrong'], m['n']),
                  '| Detection time from fault onset: median / 95th percentile | %.0f s / %.0f s |'
                  % (statistics.median(m['det']) if m['det'] else float('nan'), q(m['det'], 0.95)),
                  '| Isolation latency after the 2nd signal: max | %.2f s (target < 1 s) |' % (max(m['lat']) if m['lat'] else float('nan')),
                  '', 'Detection time depends entirely on the ASSUMED fault profile; quote it only as a model result.', '']
    lines.append('Run time: %.0f s.' % (time.time() - t0))
    with open(os.path.join(OUT, 'summary.md'), 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print('\n'.join(lines))


if __name__ == '__main__':
    main()
