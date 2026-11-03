#!/usr/bin/env python3
"""Export the completed matched fabtests comparison to CSV, Markdown and plots."""
import argparse
import csv
import json
from pathlib import Path
import statistics
import os
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('results', type=Path)
p.add_argument('--replace', type=Path, action='append', default=[], help='Replace complete size/provider/test groups with a corrected dataset')
p.add_argument('--output', type=Path, help='Write a combined report in a fresh directory')
p.add_argument('--profile', choices=('native4k','matched1k'), help='Override profile inference for figure labels')
a = p.parse_args()
source = a.results
config_file = source / "configuration.json"
config = json.loads(config_file.read_text()) if config_file.exists() else {}
profile = a.profile or config.get("profile", "matched1k")
payload = 4000 if profile == "native4k" else 1024
queues = 384 if profile == "native4k" else 128
output = a.output or source
if a.output:
    output.mkdir(parents=True, exist_ok=False)
def load(folder):
    return [dict(r, source_directory=os.path.relpath(folder, output))
            for r in json.loads((folder / 'runs.json').read_text())]
runs = load(source)
for replacement in a.replace:
    rows = load(replacement)
    keys = {(r['test'], r['provider'], r['bytes']) for r in rows}
    runs = [r for r in runs if (r['test'], r['provider'], r['bytes']) not in keys] + rows
if a.output:
    (output / 'runs.json').write_text(json.dumps(runs, indent=2) + '\n')
    (output / 'inputs.json').write_text(json.dumps(dict(base=str(source), replacements=[str(x) for x in a.replace], selection='Replace complete test/provider/size groups, retaining all three samples'), indent=2) + '\n')
a.results = output
assert runs and all(r['passed'] for r in runs)
sizes = sorted({r['bytes'] for r in runs})
providers = [x for x in ('suet', 'ref', 'ref-on', 'ref-off')
             if any(r['provider'] == x for r in runs)]
names = {'suet': 'SUET', 'ref': 'Reference',
         'ref-on': 'Reference CRC on', 'ref-off': 'Reference CRC off'}
colors = {'suet': '#2563eb', 'ref': '#d97706',
          'ref-on': '#d97706', 'ref-off': '#16a34a'}
groups = {}
for test in ('pingpong', 'bw'):
    for provider in providers:
        for size in sizes:
            rows = [r['measurements'][0] for r in runs if
                    (r['test'], r['provider'], r['bytes']) == (test, provider, size)]
            assert len(rows) == 3, (test, provider, size, len(rows))
            groups[test, provider, size] = rows

def values(test, provider, size, metric):
    v = [r[metric] for r in groups[test, provider, size]]
    return statistics.median(v), min(v), max(v)

table = []
for size in sizes:
    row = dict(bytes=size)
    for provider in providers:
        for metric, test, key in [('latency', 'pingpong', 'usec_per_transfer'), ('bandwidth', 'bw', 'mb_per_second')]:
            unit = 'us' if metric == 'latency' else 'MBps'
            center, low, high = values(test, provider, size, key)
            row[f'{provider}_{metric}_{unit}'] = center
            row[f'{provider}_{metric}_min_{unit}'] = low
            row[f'{provider}_{metric}_max_{unit}'] = high
    for provider in providers:
        if provider != 'suet' and 'suet' in providers:
            row[f'latency_ratio_{provider}_over_suet'] = row[f'{provider}_latency_us'] / row['suet_latency_us']
            row[f'bandwidth_ratio_suet_over_{provider}'] = row['suet_bandwidth_MBps'] / row[f'{provider}_bandwidth_MBps']
    if 'ref-on' in providers and 'ref-off' in providers:
        row['crc_off_bandwidth_speedup'] = row['ref-off_bandwidth_MBps'] / row['ref-on_bandwidth_MBps']
    table.append(row)
with (a.results / 'comparison.csv').open('w') as f:
    writer = csv.DictWriter(f, fieldnames=list(table[0]), lineterminator="\n"); writer.writeheader(); writer.writerows(table)

columns = [(provider, metric, unit) for metric, unit in
           [('latency', 'us'), ('bandwidth', 'MBps')] for provider in providers]
headers = [f'{names[provider]} {metric}, ' + ('µs' if unit == 'us' else 'MB/s')
           for provider, metric, unit in columns]
lines = ['| Bytes | ' + ' | '.join(headers) + ' |',
         '| ' + ' | '.join(['---:'] * (len(columns) + 1)) + ' |']
for r in table:
    lines.append(f"| {r['bytes']:,} | " + ' | '.join(
        f"{r[f'{provider}_{metric}_{unit}']:,.2f}" for provider, metric, unit in columns) + ' |')
(a.results / 'table.md').write_text('\n'.join(lines) + '\n')

fig, axes = plt.subplots(1, 2, figsize=(12, 4.7), layout='constrained')
for axis, test, metric, label in zip(axes, ('pingpong', 'bw'),
        ('usec_per_transfer', 'mb_per_second'), ('Ping-pong latency, RTT/2 (µs)', 'One-way bandwidth (MB/s)')):
    for provider in providers:
        v = [values(test, provider, s, metric) for s in sizes]
        centers = [x[0] for x in v]
        axis.errorbar(sizes, centers, yerr=([x[0]-x[1] for x in v], [x[2]-x[0] for x in v]),
                      color=colors[provider], marker='o', markersize=4, capsize=3, linewidth=1.6, label=names[provider])
    axis.set_xscale('log', base=2)
    axis.set_xticks([8, 64, 1024, 4096, 65536, 1048576] if profile == 'native4k' else [8, 64, 1024, 16384, 262144, 1048576],
                   ['8 B', '64 B', '1 KiB', '4 KiB', '64 KiB', '1 MiB'] if profile == 'native4k' else ['8 B', '64 B', '1 KiB', '16 KiB', '256 KiB', '1 MiB'])
    axis.set_xlabel('Application message size')
    axis.set_ylabel(label)
    axis.grid(True, alpha=.22)
    axis.axvline(payload, color='#6b7280', linestyle=':', linewidth=1)
    if test == 'pingpong': axis.set_yscale('log')
    else: axis.set_ylim(bottom=0)
axes[0].legend(fontsize=8, loc='upper left')
if profile == 'native4k' and 4000 in sizes and 4096 in sizes:
    for size, text, offset in ((4000, '4,000 B: 1 packet', (-75, 35)),
                               (4096, '4,096 B\n2 packets', (25, -10))):
        rate = values('bw', 'suet', size, 'mb_per_second')[0]
        axes[1].annotate(text, xy=(size, rate), xytext=offset,
                         textcoords='offset points', fontsize=8,
                         arrowprops=dict(arrowstyle='->', color='#4b5563'),
                         bbox=dict(boxstyle='round,pad=.25', fc='white', ec='#d1d5db'))
fig.suptitle('SUET vs UET reference over libfabric FI_EP_DGRAM (verbs)\n'
             f'ROD · {payload:,}-byte application payload · window 64 · queues {queues} · CPU 2\n'
             'Median of 3 samples; bars show the observed min–max', fontsize=11)
fig.savefig(a.results / 'comparison.png', dpi=180)
fig.savefig(a.results / 'comparison.svg')

# Normalize generated XML whitespace for repository checks.
svg = a.results / 'comparison.svg'
svg.write_text('\n'.join(line.rstrip() for line in svg.read_text().splitlines()) + '\n')
