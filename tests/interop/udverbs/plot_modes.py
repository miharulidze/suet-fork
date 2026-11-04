#!/usr/bin/env python3
"""Compare matched ROD/RUD latency and bandwidth with one endpoint per node."""
import argparse
import csv
import json
import os
from pathlib import Path
import statistics
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('rod', type=Path)
p.add_argument('rud', type=Path)
p.add_argument('--output', required=True, type=Path)
a = p.parse_args()
datasets = {}
configs = {}
for mode, folder in [('ROD', a.rod), ('RUD', a.rud)]:
    config = json.loads((folder/'configuration.json').read_text())
    assert config.get('mode', 'rod') == mode.lower(), (folder, config)
    assert config.get('endpoints_per_node', 1) == 1
    rows = json.loads((folder/'runs.json').read_text())
    assert rows and all(row['passed'] for row in rows), folder
    datasets[mode] = rows
    configs[mode] = config
for key in ['backend', 'profile', 'fabric', 'payload_mtu', 'suet_payload_mtu',
            'queue_depth', 'window', 'hosts', 'ips']:
    assert configs['ROD'][key] == configs['RUD'][key], key
sizes = sorted(set(row['bytes'] for row in datasets['ROD']) &
               set(row['bytes'] for row in datasets['RUD']))
sizes = [size for size in sizes if size != 4000]
assert sizes
a.output.mkdir(parents=True, exist_ok=True)
providers = [('suet', 'SUET', '#2563eb'), ('ref-off', 'Reference CRC off', '#16a34a')]
groups = {}
table = []
for mode, rows in datasets.items():
    for provider, _, _ in providers:
        for test, metric in [('pingpong', 'usec_per_transfer'), ('bw', 'mb_per_second')]:
            for size in sizes:
                selected = [row for row in rows if (row['provider'], row['test'], row['bytes']) ==
                            (provider, test, size)]
                assert len(selected) == 3, (mode, provider, test, size)
                values = [row['measurements'][0][metric] for row in selected]
                center, low, high = statistics.median(values), min(values), max(values)
                groups[mode, provider, test, size] = center, low, high
                table.append(dict(mode=mode, provider=provider, test=test, bytes=size,
                                  median=center, minimum=low, maximum=high,
                                  unit='us' if test == 'pingpong' else 'MB/s'))
with (a.output/'comparison.csv').open('w') as f:
    writer = csv.DictWriter(f, fieldnames=list(table[0]), lineterminator='\n')
    writer.writeheader(); writer.writerows(table)
lines = ['| Bytes | SUET ROD MB/s | Reference ROD MB/s | SUET RUD MB/s | Reference RUD MB/s |',
         '| ---: | ---: | ---: | ---: | ---: |']
for size in sizes:
    values = [groups[mode, provider, 'bw', size][0]
              for mode in ['ROD', 'RUD'] for provider, _, _ in providers]
    lines.append(f'| {size:,} | ' + ' | '.join(f'{v:,.2f}' for v in values) + ' |')
(a.output/'table.md').write_text('\n'.join(lines)+'\n')
(a.output/'inputs.json').write_text(json.dumps(dict(
    rod=os.path.relpath(a.rod, a.output), rud=os.path.relpath(a.rud, a.output),
    excluded_message_sizes=[4000], endpoints_per_node=1,
    queue_depth=configs['ROD']['queue_depth'], window=configs['ROD']['window'],
    backend=configs['ROD']['backend'], native_reliability='GBN'), indent=2)+'\n')

fig, axes = plt.subplots(1, 2, figsize=(12.8, 4.9), layout='constrained')
for axis, test, ylabel in zip(axes, ['pingpong', 'bw'],
        ['Ping-pong latency, RTT/2 (µs)', 'One-way bandwidth (MB/s)']):
    for provider, label, color in providers:
        for mode, style, marker in [('ROD', '-', 'o'), ('RUD', '--', 's')]:
            values = [groups[mode, provider, test, size] for size in sizes]
            axis.errorbar(sizes, [v[0] for v in values],
                          yerr=([v[0]-v[1] for v in values], [v[2]-v[0] for v in values]),
                          color=color, linestyle=style, marker=marker, markersize=4,
                          linewidth=1.6, capsize=3, label=f'{label} {mode}')
    axis.set_xscale('log', base=2)
    axis.set_xticks([8, 64, 1024, 4096, 65536, 1048576],
                   ['8 B', '64 B', '1 KiB', '4 KiB', '64 KiB', '1 MiB'])
    axis.set_xlabel('Application message size')
    axis.set_ylabel(ylabel)
    axis.grid(True, alpha=.22)
    if test == 'pingpong': axis.set_yscale('log')
    else: axis.set_ylim(bottom=0)
axes[0].legend(fontsize=8, loc='upper left')
fig.suptitle('SUET vs UET reference over verbs DGRAM: ROD and RUD\n'
             f'1 endpoint per node · 4 KiB DGRAM limit · window {configs["ROD"]["window"]} · '
             f'TX/RX depth {configs["ROD"]["queue_depth"]} each · CPU 2\n'
             'Median of 3 samples; bars show observed min–max · SUET uses GBN', fontsize=11)
fig.savefig(a.output/'comparison.png', dpi=180)
fig.savefig(a.output/'comparison.svg')
svg = a.output/'comparison.svg'
svg.write_text('\n'.join(line.rstrip() for line in svg.read_text().splitlines())+'\n')
