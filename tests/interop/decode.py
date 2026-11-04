#!/usr/bin/env python3
"""Decode PDS/standard-SES bytes from shared DGRAM backend captures."""
import argparse
import gzip
import json
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('capture', type=Path)
p.add_argument('--limit', type=int, default=20)
a = p.parse_args()
opener = gzip.open if a.capture.suffix == '.gz' else open
with opener(a.capture, 'rt') as f:
    for index, line in enumerate(f):
        if index >= a.limit:
            break
        direction, length, hexdata = line.split()
        raw = bytes.fromhex(hexdata)
        assert len(raw) == int(length) and len(raw) >= 12
        header = int.from_bytes(raw[:2], 'big')
        row = dict(direction=direction, length=len(raw), type=header >> 11,
                   next_header=(header >> 7) & 15, flags=header & 127,
                   psn=int.from_bytes(raw[4:8], 'big'))
        if row['type'] in (2, 3) and row['next_header'] == 3 and len(raw) >= 56:
            row.update(opcode=raw[12] & 63, ses_flags=raw[13],
                       message_id=int.from_bytes(raw[14:16], 'big'),
                       job=int.from_bytes(raw[17:20], 'big'),
                       pid=int.from_bytes(raw[20:22], 'big'),
                       ri=int.from_bytes(raw[22:24], 'big'),
                       initiator=int.from_bytes(raw[32:36], 'big'),
                       request_length=int.from_bytes(raw[52:56], 'big'),
                       payload_length=len(raw) - 56)
        elif row['type'] in (7, 8, 9) and row['next_header'] == 4:
            offset = {7: 12, 8: 24, 9: 32}[row['type']]
            if len(raw) >= offset + 12:
                row.update(opcode=raw[offset] & 63, rc=raw[offset+1] & 63,
                           message_id=int.from_bytes(raw[offset+2:offset+4], 'big'))
        print(json.dumps(row))
