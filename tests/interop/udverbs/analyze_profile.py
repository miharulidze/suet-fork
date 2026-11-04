import csv,json,re,statistics
from collections import defaultdict
from pathlib import Path
import argparse
parser=argparse.ArgumentParser(description='Summarize matched perf counters and steady-state flat samples')
parser.add_argument('results',type=Path)
p=parser.parse_args().results
rows=json.loads((p/'runs.json').read_text())
summary={}
def category(symbol,dso):
 if re.search(r'memcpy|memmove|memset',symbol):return 'Copies and zeroing'
 if re.search(r'pthread_.*(lock|unlock)|spin_lock|spin_unlock',symbol):return 'Locking'
 if re.search(r'malloc|calloc|realloc|(^|_)free($|_)|tcache',symbol):return 'Heap management'
 if 'vdso' in dso or re.search(r'gettime|clock_gettime',symbol):return 'Timekeeping'
 if re.search(r'^bm_|bitmap',symbol):return 'Reliability bitmaps'
 if symbol.startswith(('suet_','uet_')):return 'Protocol/API'
 if 'libfabric' in dso or 'libmlx5' in dso or 'libibverbs' in dso:return 'DGRAM/driver/CQ'
 if 'libuet_ref' in dso:return 'Reference NIC adapter/other'
 return 'Other/application/runtime'
for provider in ('suet','ref-off'):
 data={}
 stats=[r for r in rows if r['provider']==provider and r['phase']=='stat']
 data['bandwidth_MBps']=statistics.median(r['MBps'][0] for r in stats)
 for side in (0,1):
  filename=p/f'{provider}-record-r0-{side}-flat.txt';text=filename.read_text()
  parsed=[];totals=defaultdict(float)
  for line in text.splitlines():
   m=re.match(r'\s*([\d.]+)%\s+\[.\]\s+(\S+)\s+(\S+)',line)
   if m:
    percent,symbol,dso=float(m[1]),m[2],m[3]
    parsed.append(dict(percent=percent,symbol=symbol,dso=dso))
    totals[category(symbol,dso)]+=percent
  assert parsed and 98<sum(totals.values())<102,(filename,totals)
  cycles=[];instructions=[];taskms=[]
  for r in stats:
   values={}
   with (p/f'{r["name"]}-{side}.stat').open() as f:
    for x in csv.reader(f,delimiter=';'):
     if len(x)>2 and x[0].strip() and not x[0].startswith('#'):
      try:values[x[2]]=float(x[0])
      except ValueError:pass
   cycles.append(values['cycles:u']);instructions.append(values['instructions:u']);taskms.append(values['task-clock'])
  bytes_total=(stats[0]['iterations']+stats[0]['warmup'])*stats[0].get('bytes',1048576)
  data['receiver' if side==0 else 'sender']=dict(categories_percent=dict(totals),top_symbols=parsed[:20],
     median_user_cycles_per_application_byte=statistics.median(cycles)/bytes_total,
     median_instructions_per_application_byte=statistics.median(instructions)/bytes_total,
     median_task_ms=statistics.median(taskms),
     stat_scope='Entire invocation including setup/warmup/teardown',
     sample_scope=f"User cycles, identical {stats[0].get('sample_start_seconds',6):g}..{stats[0].get('sample_start_seconds',6)+stats[0].get('sample_seconds',6):g} second steady-state window")
 summary[provider]=data
(p/'analysis.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2))
