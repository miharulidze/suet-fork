#!/usr/bin/env python3
"""Check CRC switch initialization through libfabric, without socket setup."""
import argparse
import concurrent.futures
import json
from pathlib import Path
import shlex
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--root', default='/root/suet-interop-20261103/udcompare4k-crc')
p.add_argument('--fabric', default='/root/suet-interop-20261103/fix6/work/build')
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
source = Path(__file__).with_name('crc_switch_probe.c').read_bytes()

def check(host, ip):
    ssh = ['ssh', '-o', 'BatchMode=yes', '-J', 'mkhalilov@blue.inf.ethz.ch', 'root@' + host]
    binary = a.root + '/crc_switch_probe'
    compile_args = ['cc', '-O2', '-Wall', '-Wextra', '-x', 'c', '-',
                    '-I' + a.fabric + '/include', '-L' + a.fabric + '/lib',
                    '-Wl,-rpath,' + a.fabric + '/lib', '-lfabric', '-o', binary]
    built = subprocess.run(ssh + [shlex.join(compile_args)], input=source,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (a.output / (host + '-build.log')).write_bytes(built.stdout)
    assert built.returncode == 0, host + ': probe compile failed'
    rows = []
    for mode in ('unset', '0', '1', 'invalid'):
        env = ['env', '-u', 'UET_BENCH_DISABLE_CRC',
               'LD_LIBRARY_PATH=' + a.fabric + '/lib',
               'FI_PROVIDER_PATH=' + a.root + '/build/provider',
               'FI_VERBS_IFACE=ens785f0np0', 'UET_UD_SOURCE=' + ip, 'UET_PDS=pds']
        if mode != 'unset':
            env.append('UET_BENCH_DISABLE_CRC=' + mode)
        command = ssh + [shlex.join(env + ['timeout', '-k', '2s', '15s', binary])]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        body = result.stdout.decode()
        (a.output / (host + '-' + mode + '.log')).write_bytes(result.stdout)
        if mode == 'invalid':
            passed = (result.returncode == 1 and 'DOMAIN_RESULT=-22' in body and
                      'UET_BENCH_DISABLE_CRC must be 0 or 1' in body)
        else:
            passed = (result.returncode == 0 and 'DOMAIN_RESULT=0' in body and
                      'UET_BENCH_CRC mode=' + ('off' if mode == '1' else 'on') in body)
        rows.append(dict(host=host, mode=mode, command=command, exit_code=result.returncode, passed=passed))
    return rows

with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
    results = list(pool.map(lambda x: check(*x), [('roce00.cscs.ch', '10.203.44.1'),
                                                ('roce01.cscs.ch', '10.203.44.2')]))
rows = [r for result in results for r in result]
(a.output / 'summary.json').write_text(json.dumps(rows, indent=2) + '\n')
assert all(r['passed'] for r in rows), rows
print(str(len(rows)) + ' CRC switch initialization checks passed')
