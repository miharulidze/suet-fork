#!/usr/bin/env python3
"""Profile unchanged SUET/reference CRC-off binaries on the same UD workload."""
import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--provider-root', help='Override only the reference FI_PROVIDER_PATH for a shared-backend build')
p.add_argument('--fabric', help='Override libfabric prefix and fabtests binaries together')
p.add_argument('--baseline', type=Path, default=Path(__file__).parent/'results/crc-switch-performance/runs.json')
p.add_argument('--output', type=Path, required=True)
p.add_argument('--log-dir', type=Path, help='Raw perf/log directory; defaults to a new directory under the system temporary directory')
p.add_argument('--remote', default='/root/suet-interop-20261103/profiles-20261104')
p.add_argument('--bytes', type=int, default=1048576)
p.add_argument('--sample-start', type=float, default=6)
p.add_argument('--sample-seconds', type=float, default=6)
p.add_argument('--iterations', type=int, default=100000)
p.add_argument('--warmup', type=int, default=10000)
p.add_argument('--samples', type=int, default=3)
p.add_argument('--control-port', type=int, default=34000)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
if a.log_dir is None:
    a.log_dir = Path(tempfile.mkdtemp(prefix='suet-perf-'))
else:
    a.log_dir.mkdir(parents=True, exist_ok=False)
baseline = json.loads(a.baseline.read_text())
records = []
for phase in ('stat', 'record'):
    for repeat in range(a.samples if phase == 'stat' else 1):
        for provider in (('suet', 'ref-off') if repeat % 2 == 0 else ('ref-off', 'suet')):
            row = next(r for r in baseline if r['provider'] == provider and r['test'] == 'bw' and r['bytes'] == a.bytes)
            name = f'{provider}-{phase}-r{repeat}'
            commands, processes, streams = [], [], []
            for side, original in enumerate(row['commands']):
                command = original.copy()
                args = shlex.split(command[-1])
                if a.provider_root:
                    args = ['FI_PROVIDER_PATH='+a.provider_root+'/build/provider' if s.startswith('FI_PROVIDER_PATH=') else s for s in args]
                if a.fabric:
                    args = ['LD_LIBRARY_PATH='+a.fabric+'/lib' if s.startswith('LD_LIBRARY_PATH=') else
                            a.fabric+'/bin/'+Path(s).name if '/bin/fi_rdm_' in s else s for s in args]
                for flag, value in (('-I', a.iterations), ('-w', a.warmup)):
                    args[args.index(flag)+1] = str(value)
                args = [f'-b={a.control_port+len(records)}' if s.startswith('-b=') else s for s in args]
                index = args.index('timeout')
                env, app = args[:index], args[index:]
                app = [s for i,s in enumerate(app) if i not in (4,5,6)]  # taskset -c 2 moves outside perf
                target = f'{a.remote}/{name}-{side}'
                if phase == 'stat':
                    perf = ['perf','stat','-x',';','-e','task-clock,cycles:u,instructions:u,cache-misses:u,context-switches',
                            '-o', target+'.stat','--']
                else:
                    perf = ['perf','record','-F','199','-e','cycles:u','--call-graph','dwarf,8192','-o',target+'.data','--']
                command[-1] = 'mkdir -p '+shlex.quote(a.remote)+' && exec '+shlex.join(env+['taskset','-c','2']+perf+app)
                commands.append(command)
                stream = (a.log_dir/f'{name}-{side}.log').open('wb');streams.append(stream)
                processes.append(subprocess.Popen(command,stdout=stream,stderr=subprocess.STDOUT))
                if side == 0: time.sleep(.8)
            try:
                codes = [proc.wait(timeout=330) for proc in processes]
            finally:
                for proc in processes:
                    if proc.poll() is None: proc.kill();proc.wait()
                for stream in streams: stream.close()
            text = (a.log_dir/f'{name}-1.log').read_text()
            rates = []
            for line in text.splitlines():
                fields = line.split()
                if len(fields) >= 7 and re.fullmatch(r'\d+(?:\.\d+)?[kmg]?', fields[0]):
                    rates.append(float(fields[4]))
            result = dict(name=name,provider=provider,phase=phase,repeat=repeat,commands=commands,
                          exit_codes=codes,MBps=rates,iterations=a.iterations,warmup=a.warmup,bytes=a.bytes,
                          sample_start_seconds=a.sample_start,sample_seconds=a.sample_seconds,
                          log_directory=str(a.log_dir))
            result['passed'] = codes == [0,0] and len(rates) == 1
            records.append(result)
            (a.output/'runs.json').write_text(json.dumps(records,indent=2)+'\n')
            assert result['passed'], result
            print(json.dumps({k:v for k,v in result.items() if k!='commands'}),flush=True)
            for side, command in enumerate(commands):
                target=f'{a.remote}/{name}-{side}'
                if phase == 'stat':
                    output=subprocess.check_output(command[:-1]+['cat '+shlex.quote(target+'.stat')])
                    (a.log_dir/f'{name}-{side}.stat').write_bytes(output)
                else:
                    # Use an identical steady-state interval after both warmups,
                    # excluding startup and the reference's close-wait loop.
                    code = """import json,re,subprocess,sys
data=sys.argv[1]
s=subprocess.check_output(['perf','script','-i',data,'--max-stack','1','-F','time'],stderr=subprocess.DEVNULL,text=True)
times=[float(x) for x in re.findall(r'(?m)^\\s*(\\d+\\.\\d+):',s)]
first=min(times);start=float(sys.argv[2]);seconds=float(sys.argv[3])
assert max(times)>=first+start+seconds,'Steady-state sample interval exceeds run duration'
window=f'{first+start:.9f},{first+start+seconds:.9f}'
for kind,extra in [('flat',['--no-children','-g','none','--percent-limit','0']),('callers',['--children','-g','graph,0.5,caller'])]:
 out=subprocess.check_output(['perf','report','-i',data,'--stdio','--no-inline','--time',window,'--sort','symbol,dso']+extra,stderr=subprocess.STDOUT,text=True)
 print(json.dumps({'kind':kind,'window':window,'text':out}))
"""
                    output=subprocess.check_output(command[:-1]+['python3 -c '+shlex.quote(code)+' '+shlex.quote(target+'.data')+' '+str(a.sample_start)+' '+str(a.sample_seconds)])
                    for line in output.decode().splitlines():
                        item=json.loads(line)
                        (a.log_dir/f'{name}-{side}-{item["kind"]}.txt').write_text('\n'.join(s.rstrip() for s in item['text'].splitlines())+'\n')
                        (a.log_dir/f'{name}-{side}-window.txt').write_text(item['window']+'\n')
