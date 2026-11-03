#!/usr/bin/env python3
"""Run provider client/server pairs over two SSH-accessible machines."""
import argparse
import json
import re
from pathlib import Path
import shlex
import subprocess
import time


def ssh(host, command):
    return ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=15', '-J',
            'mkhalilov@blue.inf.ethz.ch', f'root@{host}', command]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--server-host', default='roce00.cscs.ch')
    p.add_argument('--client-host', default='roce01.cscs.ch')
    p.add_argument('--server-ip', default='10.203.44.1')
    p.add_argument('--client-ip', default='10.203.44.2')
    p.add_argument('--root', default='/root/suet-interop-20261103/base')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--pairs', default='suet-suet,ref-ref,suet-ref,ref-suet', help='server-client providers')
    p.add_argument('--modes', default='rod,rud')
    p.add_argument('--sizes', default='16,1024,8192,65537')
    p.add_argument('--rounds', type=int, default=10)
    p.add_argument('--sr', type=int, choices=(0, 1), default=0)
    p.add_argument('--payload-mtu', type=int, choices=(0, 1024), default=1024,
                   help='1024 matches the reference; 0 probes legacy SUET transport sizing')
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    results = []
    for mode in a.modes.split(','):
        assert mode in ('rod', 'rud')
        for pair in a.pairs.split(','):
            providers = pair.split('-')
            assert len(providers) == 2 and all(x in ('suet', 'ref') for x in providers)
            for size in map(int, a.sizes.split(',')):
                case = f'{pair}-{mode}-{size}-sr{a.sr}'
                remote = f'{a.root}/results/{a.output.name}/{case}'
                handles, processes = [], []
                try:
                    for side, (host, ip, provider) in enumerate(zip(
                            (a.server_host, a.client_host), (a.server_ip, a.client_ip), providers)):
                        role = 'server' if side == 0 else 'client'
                        args = ['env', 'FI_PROVIDER=udp;ofi_suet', f'FI_OFI_SUET_SELECTIVE_REPEAT={a.sr}',
                                f'FI_OFI_SUET_PAYLOAD_MTU={a.payload_mtu}',
                                'FI_LOG_LEVEL=info', 'FI_LOG_PROV=ofi_suet', 'FI_LOG_SUBSYS=domain',
                                'FI_OFI_SUET_PRINT_DOMAIN_COUNTERS=1',
                                'UET_PDS=pds', 'UET_PDS_TX_TIMEOUT=100', 'timeout', '-k', '2s', '18s',
                                f'{a.root}/interop/pingpong', provider, role, ip, a.server_ip,
                                '60401', mode, str(size), str(a.rounds), f'{remote}/{role}.packets']
                        command = f'mkdir -p {shlex.quote(remote)} && exec {shlex.join(args)}'
                        stream = (a.output / f'{case}-{role}.log').open('wb')
                        handles.append(stream)
                        processes.append(subprocess.Popen(ssh(host, command), stdout=stream, stderr=subprocess.STDOUT))
                        if side == 0:
                            deadline = time.monotonic() + 12
                            while processes[0].poll() is None:
                                if 'CONTROL_LISTENING' in (a.output / f'{case}-server.log').read_text(errors='replace'):
                                    break
                                if time.monotonic() >= deadline:
                                    break
                                time.sleep(.02)
                    codes = []
                    for proc in processes:
                        try:
                            codes.append(proc.wait(timeout=35))
                        except subprocess.TimeoutExpired:
                            proc.kill(); codes.append(proc.wait())
                finally:
                    for proc in processes:
                        if proc.poll() is None:
                            proc.kill(); proc.wait()
                    for stream in handles:
                        stream.close()
                texts = [(a.output / f'{case}-{role}.log').read_text(errors='replace') for role in ('server', 'client')]
                success = (codes == [0, 0] and all('RESULT ' in text for text in texts) and
                           texts[0].count('server RX ping ') == a.rounds and
                           texts[0].count('server TX pong ') == a.rounds and
                           texts[1].count('client TX ping ') == a.rounds and
                           texts[1].count('client RX pong ') == a.rounds)
                for provider, text in zip(providers, texts):
                    if provider == 'suet':
                        counters = re.findall(r'pdc_close_in_err=(\d+)', text)
                        success = success and bool(counters) and all(int(n) == 0 for n in counters)
                row = dict(case=case, passed=success, exit_codes=codes, rounds=a.rounds,
                           payload_mtu=a.payload_mtu,
                           remote_capture_directory=remote, server_host=a.server_host, client_host=a.client_host)
                results.append(row)
                print(json.dumps(row), flush=True)
                (a.output / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
    return not all(r['passed'] for r in results)


if __name__ == '__main__':
    raise SystemExit(main())
