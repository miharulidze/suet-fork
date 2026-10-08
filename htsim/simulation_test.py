#!/usr/bin/env python3
"""End-to-end checks use real SUET, the dynamic DGRAM provider and htsim queues."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve()
env = dict(os.environ, FI_PROVIDER_PATH=str(binary.parent))


def run(name, body, options=(), expected=0):
    path = Path(tmp) / (name + '.cm')
    path.write_text(body)
    result = subprocess.run([str(binary), '-tm', str(path), '-end', '20000', *options],
                            env=env, capture_output=True, text=True, timeout=60)
    assert result.returncode == expected, result.stdout + result.stderr
    flows = [line for line in result.stdout.splitlines() if line.startswith('Flow ')]
    summary = next(line for line in result.stdout.splitlines() if line.startswith('SUET result:'))
    if not expected:
        assert len(flows) == int(re.search(r'Connections (\d+)', body)[1]), result.stdout
        assert all('verified' in line for line in flows)
        assert 'receive_drops 0' in summary, result.stdout
    print(name + ': PASS', flush=True)
    return flows, summary


with tempfile.TemporaryDirectory(prefix='suet-htsim-') as tmp:
    one = 'Nodes 16\nConnections 1\n0->13 start 0 size 65537\n'
    first = run('segmented', one)
    assert run('deterministic', one) == first
    baseline_fct = float(re.search(r'fct_us ([\d.]+)', first[0][0])[1])
    slow = run('slower_link', one, ['-linkspeed', '10'])
    assert float(re.search(r'fct_us ([\d.]+)', slow[0][0])[1]) > baseline_fct
    run('empty', 'Nodes 16\nConnections 1\n0->13 start 0 size 0\n')
    lost = run('loss_recovery', 'Nodes 16\nConnections 1\n0->13 start 0 size 1024\n',
               ['-drop_first', '1'])
    assert float(re.search(r'fct_us ([\d.]+)', lost[0][0])[1]) >= 1000, lost
    assert 'network_drops 1' in lost[1], lost
    run('incast', 'Nodes 16\nConnections 4\n' + ''.join(
        f'{src}->15 start 0 size 65537\n' for src in range(4)), ['-dgram_queue', '16'])
    trigger = run('triggers', '''Nodes 16
Connections 3
Triggers 2
0->15 id 1 start 1000000 size 4097 recv_done_trigger 1
15->0 id 2 trigger 1 size 2048 send_done_trigger 2
1->15 id 3 trigger 2 size 0
trigger id 1 oneshot
trigger id 2 oneshot
''')
    assert float(re.search(r'start_us ([\d.]+)', trigger[0][0])[1]) >= 1
    topology = Path(sys.argv[2]).resolve()
    run('topology_file', 'Nodes 32\nConnections 1\n0->31 start 0 size 65537\n',
        ['-topo', str(topology)])
    for pdc in ('rod', 'rud'):
        traffic = 'Nodes 16\nConnections 3\n' + ''.join(
            f'0->13 start 0 size {size}\n' for size in (65537, 32769, 8193))
        for condition, opts in (
            ('normal', []),
            ('reorder', ['-reorder_every', '7']),
            ('random_loss', ['-drop_per_mille', '25']),
            ('burst_loss', ['-burst_every', '19', '-burst_length', '2']),
        ):
            for sr in ('0', '1'):
                env['FI_OFI_SUET_SELECTIVE_REPEAT'] = sr
                result = run(pdc + '_sr' + sr + '_' + condition, traffic, ['-pdc', pdc, *opts])
                if pdc == 'rud' and sr == '1' and condition == 'reorder':
                    assert int(re.search(r'sacks (\d+)', result[1])[1]) > 0, result
    env.pop('FI_OFI_SUET_SELECTIVE_REPEAT', None)
    run('timeout', one, ['-end', '0.01'], expected=2)
print('All simulator integration checks passed', flush=True)
