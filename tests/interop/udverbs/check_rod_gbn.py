#!/usr/bin/env python3
"""Test the patched reference replay functions with transport/backpressure mocks.

This portable unit test executes the actual C functions without Linux NIC/API
setup. It complements live ping-pong; it does not validate the complete core.
Pass the private reference-source directory produced by build.sh.
"""
import argparse
from pathlib import Path
import os
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('reference_source', type=Path)
a = p.parse_args()
s = (a.reference_source / 'uet_pds.c').read_text()
start = s.index('/* Submit ROD GoBackN in PSN order')
end = s.index('/* returns 0, -EIO, or -EAGAIN */', start)
with tempfile.TemporaryDirectory(prefix='uet-rod-gbn-') as directory:
    d = Path(directory)
    (d / 'reference_gbn_functions.h').write_text(s[start:end])
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-g',
                    '-Wall', '-Wextra', '-Werror', '-I'+str(d),
                    str(Path(__file__).with_name('rod_gbn_probe.c')),
                    '-o', str(d/'probe')], check=True)
    subprocess.run([str(d/'probe')], check=True)
