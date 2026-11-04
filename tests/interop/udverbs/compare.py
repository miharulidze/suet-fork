#!/usr/bin/env python3
"""Run the same stock fabtests binaries over SUET and the reference test adapter."""
import argparse
import json
from pathlib import Path
import re
import shlex
import statistics
import subprocess
import time


def ssh(host, command):
    return ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=15', '-J',
            'mkhalilov@blue.inf.ethz.ch', 'root@' + host, shlex.join(command)]


def pair(a, provider, test, size, iterations, warmup, repeat):
    reference = provider.startswith('ref')
    crc = 'off' if provider == 'ref-off' else 'on'
    name = f'{provider}-{test}-{size}-r{repeat}'
    commands, processes, files = [], [], []
    try:
        for side, (host, ip) in enumerate(zip(a.hosts, a.ips)):
            env = ['env', 'LD_LIBRARY_PATH=' + a.fabric + '/lib',
                   'FI_PROVIDER_PATH=' + a.root + '/build/provider',
                   'FI_VERBS_IFACE=ens785f0np0', f'FI_VERBS_TX_SIZE={a.queue_depth}', f'FI_VERBS_RX_SIZE={a.queue_depth}',
                   'FI_LOG_LEVEL=warn',
                   f'FI_OFI_SUET_PAYLOAD_MTU={a.suet_payload_mtu}', 'FI_OFI_SUET_SELECTIVE_REPEAT=0',
                   f'UET_BENCH_PAYLOAD_MTU={a.payload_mtu}', 'UET_DGRAM_PROVIDER=' + a.backend,
                   'UET_BENCH_DISABLE_CRC=' + ('1' if crc == 'off' else '0'),
                   'UET_PDS=pds', 'UET_UD_SOURCE=' + ip, 'UET_BENCH_MODE=rod']
            args = ['timeout', '-k', '3s', str(a.timeout) + 's', 'taskset', '-c', '2',
                    a.fabric + '/bin/fi_rdm_' + test, '-p',
                    'uet_ref' if reference else a.backend + ';ofi_suet',
                    '-b=' + str(a.control_port + a.case_index), '-j', '0', '-S', str(size), '-I', str(iterations),
                    '-w', str(warmup), '-W', str(a.window), '-s', ip]
            if provider == 'suet' and a.backend == 'verbs':
                args += ['-d', 'mlx5_0-dgram']
            if a.stage == 'correctness':
                args += ['-v']
            if side:
                args += [a.ips[0]]
            command = ssh(host, env + args)
            commands.append(command)
            out = (a.output / (name + ('-client.log' if side else '-server.log'))).open('wb')
            files.append(out)
            processes.append(subprocess.Popen(command, stdout=out, stderr=subprocess.STDOUT))
            if side == 0:
                time.sleep(.8)
        codes = [p.wait(timeout=a.timeout + 30) for p in processes]
    finally:
        for p in processes:
            if p.poll() is None:
                p.kill(); p.wait()
        for f in files:
            f.close()
    text = (a.output / (name + '-client.log')).read_text(errors='replace')
    rates = []
    for line in text.splitlines():
        fields = line.split()
        if fields and re.fullmatch(r'\d+(?:\.\d+)?[kmg]?', fields[0]) and len(fields) >= 7:
            try:
                rates.append(dict(mb_per_second=float(fields[4]), usec_per_transfer=float(fields[5])))
            except ValueError:
                pass
    valid = codes == [0, 0] and len(rates) == 1
    if reference:
        for side in ('server', 'client'):
            body = (a.output / f'{name}-{side}.log').read_text(errors='replace')
            valid &= (f'UD_BACKEND provider={a.backend} ' in body and 'type=FI_EP_DGRAM' in body and 'wire=pds-ses' in body and
                      f'UET_BENCH_CRC mode={crc}' in body and
                      bool(re.search(r'UD_BACKEND tx=[1-9]\d* rx=[1-9]\d*', body)))
    return dict(name=name, provider=provider, test=test, bytes=size, iterations=iterations,
                crc=crc if reference else None,
                warmup=warmup, repeat=repeat, exit_codes=codes, passed=bool(valid),
                measurements=rates, commands=commands)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root', default='/root/suet-interop-20261104/dgram')
    p.add_argument('--backend', choices=('verbs','udp'), default='verbs')
    p.add_argument('--profile', choices=('native4k', 'matched1k'), default='native4k')
    p.add_argument('--fabric', default='/root/suet-interop-20261104/native-gap/build')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--stage', choices=('correctness', 'performance'), default='correctness')
    p.add_argument('--providers', default='suet,ref-off')
    p.add_argument('--tests', default='pingpong,bw')
    p.add_argument('--sizes', default='8,64,256,1024,4000,4096,16384,65536,262144,1048576')
    p.add_argument('--samples', type=int, default=3)
    p.add_argument('--window', type=int, default=64)
    p.add_argument('--control-port', type=int, default=30600)
    p.add_argument('--bandwidth-bytes', type=int, default=268435456)
    p.add_argument('--timeout', type=int, default=300)
    p.add_argument('--iterations', type=int, help='Override the automatic iteration count')
    p.add_argument('--warmup', type=int, help='Override automatic warmups for controlled experiments')
    p.add_argument('--server-ip', default='10.203.44.1')
    p.add_argument('--client-ip', default='10.203.44.2')
    p.add_argument('--reverse-hosts', action='store_true')
    a = p.parse_args()
    if any(x not in ('suet','ref-off') for x in a.providers.split(',')):
        p.error('The shared PDS/SES DGRAM backend uses reference CRC-off; CRC-on results are archived at b4c3e14')
    a.payload_mtu = 4000 if a.profile == "native4k" and a.backend == "verbs" else 1024
    a.suet_payload_mtu = 0 if a.profile == "native4k" and a.backend == "verbs" else 1024
    a.queue_depth = 384 if a.profile == "native4k" else 128
    a.hosts = ['roce00.cscs.ch', 'roce01.cscs.ch']
    a.ips = [a.server_ip, a.client_ip]
    if a.reverse_hosts:
        a.hosts.reverse(); a.ips.reverse()
    a.output.mkdir(parents=True, exist_ok=False)
    (a.output / 'configuration.json').write_text(json.dumps(
        {k: str(v) if isinstance(v, Path) else v for k, v in vars(a).items()}, indent=2) + '\n')
    results = []
    for test in a.tests.split(','):
        assert test in ('pingpong', 'bw')
        for size in map(int, a.sizes.split(',')):
            for repeat in range(a.samples):
                # Alternate which implementation goes first, retaining equal iteration counts.
                providers = a.providers.split(',')
                if repeat % 2:
                    providers.reverse()
                for provider in providers:
                    assert provider in ('suet', 'ref', 'ref-on', 'ref-off')
                    if a.stage == 'correctness':
                        iterations, warmup = 128, 16
                    elif test == 'pingpong':
                        iterations = max(128, min(10000, 8388608 // size))
                        warmup = max(16, min(1000, iterations // 10))
                    else:
                        iterations = max(10000 if a.profile == "native4k" else 256,
                                         min(262144, a.bandwidth_bytes // size))
                        warmup = (10000 if a.profile == "native4k" else
                                  max(2 * a.window, min(10000, iterations // 10)))
                    if a.iterations is not None:
                        iterations = a.iterations
                    if a.warmup is not None:
                        warmup = a.warmup
                    a.case_index = len(results)
                    row = pair(a, provider, test, size, iterations, warmup, repeat)
                    results.append(row)
                    (a.output / 'runs.json').write_text(json.dumps(results, indent=2) + '\n')
                    print(json.dumps({k: v for k, v in row.items() if k != 'commands'}), flush=True)
                    if not row['passed']:
                        return 1
    medians = []
    for test in a.tests.split(','):
        for size in map(int, a.sizes.split(',')):
            for provider in a.providers.split(','):
                rows = [r['measurements'][0] for r in results if r['test'] == test and
                        r['bytes'] == size and r['provider'] == provider]
                medians.append(dict(test=test, bytes=size, provider=provider,
                    mb_per_second=statistics.median(r['mb_per_second'] for r in rows),
                    usec_per_transfer=statistics.median(r['usec_per_transfer'] for r in rows)))
    (a.output / 'medians.json').write_text(json.dumps(medians, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
