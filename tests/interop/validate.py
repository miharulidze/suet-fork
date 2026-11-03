#!/usr/bin/env python3
"""Per-revision UDP correctness suite plus two-node SUET/UD-verbs bandwidth."""
import argparse
import json
import re
import statistics
from pathlib import Path
import shlex
import subprocess
import time
from run import ssh


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root', required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--skip-suite', action='store_true', help='Reuse a recorded suite result for the same build')
    p.add_argument('--bandwidth-iterations', type=int, default=2000)
    p.add_argument('--bandwidth-warmup', type=int, default=100)
    p.add_argument('--bandwidth-samples', type=int, default=3)
    a = p.parse_args()
    if a.bandwidth_iterations <= 0 or a.bandwidth_warmup < 0 or a.bandwidth_samples <= 0:
        p.error('bandwidth iterations/samples must be positive and warmup nonnegative')
    a.output.mkdir(parents=True, exist_ok=False)
    (a.output / 'configuration.json').write_text(json.dumps(dict(
        root=a.root, provider='verbs;ofi_suet', domain='mlx5_0-dgram', cpu=2,
        payload_mtu='legacy/default', iterations=a.bandwidth_iterations,
        warmup=a.bandwidth_warmup, samples=a.bandwidth_samples, window=64,
        skip_suite=a.skip_suite), indent=2) + '\n')
    bin_dir = a.root + '/work/build/bin'
    results = []
    for host in ('roce00.cscs.ch', 'roce01.cscs.ch'):
        command = shlex.join(['ldd', bin_dir + '/fi_rdm_bw'])
        loaded = subprocess.run(ssh(host, command), capture_output=True, text=True, check=True).stdout
        (a.output / f'{host}-loader.log').write_text(loaded)
        expected = a.root + '/work/build/lib/libfabric.so.1'
        if expected not in loaded:
            raise SystemExit(f'{host}: fabtests does not load {expected}; relink before validating')
    # Run the existing complete suite, retaining original iteration counts.
    cmd = shlex.join(['env', 'FI_LOG_LEVEL=warn', 'timeout', '-k', '3s', '600s', 'python3',
                     a.root + '/source/test_suet.py', '--bin-dir', bin_dir, '--timeout', '120', '--verbose'])
    if not a.skip_suite:
        with (a.output / 'udp-fabtests.log').open('wb') as stream:
            code = subprocess.run(ssh('roce00.cscs.ch', cmd), stdout=stream, stderr=subprocess.STDOUT).returncode
        results.append(dict(test='udp-fabtests', exit=code))
    for test, extra in [('fi_rdm_pingpong', []), ('fi_rdm_tagged_pingpong', []),
                        ('fi_rma_bw', ['-o', 'write'])]:
        name = 'verbs-' + test + ('-' + extra[-1] if extra else '')
        base = ['timeout', '-k', '3s', '60s', bin_dir + '/' + test, '-p', 'verbs;ofi_suet',
                '-d', 'mlx5_0-dgram', '-b', '-S', '8192', '-I', '100', '-v'] + extra
        with (a.output / f'{name}-server.log').open('wb') as server, (a.output / f'{name}-client.log').open('wb') as client:
            proc = subprocess.Popen(ssh('roce00.cscs.ch', shlex.join(base + ['-s', '10.203.44.1'])), stdout=server, stderr=subprocess.STDOUT)
            time.sleep(.7)
            rc = subprocess.run(ssh('roce01.cscs.ch', shlex.join(base + ['-s', '10.203.44.2', '10.203.44.1'])), stdout=client, stderr=subprocess.STDOUT).returncode
            sr = proc.wait(timeout=70)
        results.append(dict(test=name, server_exit=sr, client_exit=rc))
    for size in (65536, 1048576):
        for repeat in range(a.bandwidth_samples):
            name = f'verbs-bw-{size}-{repeat}'
            base = ['env', 'FI_VERBS_IFACE=ens785f0np0', 'timeout', '-k', '3s', '60s',
                    'taskset', '-c', '2', bin_dir + '/fi_rdm_bw', '-p', 'verbs;ofi_suet',
                    '-d', 'mlx5_0-dgram', '-b', '-S', str(size),
                    '-I', str(a.bandwidth_iterations), '-w', str(a.bandwidth_warmup), '-W', '64']
            with (a.output / f'{name}-server.log').open('wb') as server, (a.output / f'{name}-client.log').open('wb') as client:
                proc = subprocess.Popen(ssh('roce00.cscs.ch', shlex.join(base + ['-s', '10.203.44.1'])), stdout=server, stderr=subprocess.STDOUT)
                time.sleep(.7)
                rc = subprocess.run(ssh('roce01.cscs.ch', shlex.join(base + ['-s', '10.203.44.2', '10.203.44.1'])), stdout=client, stderr=subprocess.STDOUT).returncode
                sr = proc.wait(timeout=70)
            body = (a.output / f'{name}-client.log').read_text(errors='replace')
            rates = [float(line.split()[4]) for line in body.splitlines()
                     if re.match(r'^(?:64k|1m)\s+\S+\s+', line)]
            row = dict(test=name, server_exit=sr, client_exit=rc, megabytes_per_second=rates)
            results.append(row)
            print(json.dumps(row), flush=True)
    (a.output / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
    medians = {str(size): statistics.median([r['megabytes_per_second'][0] for r in results
               if r['test'].startswith(f'verbs-bw-{size}-') and r['megabytes_per_second']])
               for size in (65536, 1048576)}
    (a.output / 'bandwidth.json').write_text(json.dumps(medians, indent=2) + '\n')
    return any(r.get('exit', 0) or r.get('server_exit', 0) or r.get('client_exit', 0) for r in results)


if __name__ == '__main__':
    raise SystemExit(main())
