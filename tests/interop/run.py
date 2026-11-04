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
    p.add_argument('--root', default='/root/suet-interop-20261104/dgram')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--backends', default='verbs,udp')
    p.add_argument('--fabric', default='/root/suet-interop-20261104/native-gap/build')
    p.add_argument('--control-port', type=int, default=35000)
    p.add_argument('--pairs', default='suet-suet,ref-ref,suet-ref,ref-suet', help='server-client providers')
    p.add_argument('--modes', default='rod,rud')
    p.add_argument('--sizes', default='16,1024,1025,4000,4001,65537,1048576')
    p.add_argument('--rounds', type=int, default=10)
    p.add_argument('--capture', action='store_true', help='Enable per-packet text capture; disabled during normal tests and benchmarks')
    p.add_argument('--sr', type=int, choices=(0, 1), default=0)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    results = []
    (a.output/'configuration.json').write_text(json.dumps({k:str(v) if isinstance(v,Path) else v for k,v in vars(a).items()},indent=2)+'\n')
    for backend in a.backends.split(','):
        assert backend in ('verbs','udp')
        payload_mtu=4000 if backend=='verbs' else 1024
        for mode in a.modes.split(','):
            assert mode in ('rod', 'rud')
            for pair in a.pairs.split(','):
                providers = pair.split('-')
                assert len(providers) == 2 and all(x in ('suet', 'ref') for x in providers)
                for size in map(int, a.sizes.split(',')):
                    case = f'{backend}-{pair}-{mode}-{size}-sr{a.sr}'
                    remote = f'{a.root}/results/{a.output.name}/{case}'
                    handles, processes, commands = [], [], []
                    try:
                        for side, (host, ip, provider) in enumerate(zip(
                                (a.server_host, a.client_host), (a.server_ip, a.client_ip), providers)):
                            role = 'server' if side == 0 else 'client'
                            args = ['env', 'LD_LIBRARY_PATH='+a.fabric+'/lib',
                                'FI_PROVIDER_PATH='+a.root+'/build/provider',
                                f'FI_OFI_SUET_SELECTIVE_REPEAT={a.sr}',
                                f'FI_OFI_SUET_PAYLOAD_MTU={0 if backend=="verbs" else payload_mtu}',
                                'FI_VERBS_IFACE=ens785f0np0','FI_VERBS_TX_SIZE=384','FI_VERBS_RX_SIZE=384',
                                'FI_LOG_LEVEL=info', 'FI_LOG_PROV=ofi_suet', 'FI_LOG_SUBSYS=domain',
                                'FI_OFI_SUET_PRINT_DOMAIN_COUNTERS=1',
                                'UET_PDS=pds','UET_BENCH_DISABLE_CRC=1','UET_DGRAM_PROVIDER='+backend,
                                'UET_BENCH_PAYLOAD_MTU='+str(payload_mtu),'UET_BENCH_MODE='+mode,
                                'UET_PDS_TX_TIMEOUT=100', 'timeout', '-k', '3s', '40s',
                                f'{a.root}/build/pingpong', provider, role, ip, a.server_ip,
                                str(a.control_port+len(results)),mode,str(size),str(a.rounds),f'{remote}/{role}.packets' if a.capture else '-',backend]
                            command = f'mkdir -p {shlex.quote(remote)} && exec {shlex.join(args)}'
                            stream = (a.output / f'{case}-{role}.log').open('wb')
                            handles.append(stream)
                            commands.append(ssh(host,command))
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
                                codes.append(proc.wait(timeout=50))
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
                        if provider == 'ref':
                            success = success and f'UD_BACKEND provider={backend} ' in text and 'wire=pds-ses' in text
                            success = success and bool(re.search(r'UD_BACKEND tx=[1-9]\d* rx=[1-9]\d*',text))
                            counters = re.findall(r'pdc_close_in_err_cnt\s*:\s*(\d+)', text)
                            success = success and bool(counters) and all(int(n) == 0 for n in counters)
                        if provider == 'suet':
                            counters = re.findall(r'pdc_close_in_err=(\d+)', text)
                            success = success and bool(counters) and all(int(n) == 0 for n in counters)
                    row = dict(case=case, passed=success, exit_codes=codes, rounds=a.rounds,
                               payload_mtu=payload_mtu, backend=backend, commands=commands,
                               remote_capture_directory=remote, server_host=a.server_host, client_host=a.client_host)
                    results.append(row)
                    print(json.dumps({k:v for k,v in row.items() if k!='commands'}),flush=True)
                    (a.output / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
                    if not success: return 1
    return not all(r['passed'] for r in results)


if __name__ == '__main__':
    raise SystemExit(main())
