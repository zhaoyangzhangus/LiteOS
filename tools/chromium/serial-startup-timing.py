#!/usr/bin/env python3
"""Timestamp newly emitted startup markers without changing the guest build."""
import argparse
import json
import pathlib
import re
import time

parser = argparse.ArgumentParser()
parser.add_argument('--seconds', type=float, default=60)
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[2]
pattern = re.compile(rb'(?<![A-Za-z0-9_])OS_(?:CHROME_STARTUP|PROFILE|CONTENT_INIT|BROWSER_PROCESS|RENDER_PROCESS)_[A-Z0-9_]+(?![A-Za-z0-9_])')
events = []
start = time.monotonic()
pending = b''
with (root / 'build/qemu-serial.log').open('rb') as stream:
    stream.seek(0, 2)
    print('Observing new startup markers', flush=True)
    while time.monotonic() - start < args.seconds:
        pending += stream.read()
        lines = pending.split(b'\n')
        pending = lines.pop()
        for line in lines:
            for match in pattern.finditer(line):
                event = {'seconds': round(time.monotonic() - start, 4),
                         'marker': match.group().decode('ascii')}
                events.append(event)
                print(json.dumps(event), flush=True)
        time.sleep(.05)
report = {'clock': 'host monotonic; serial arrival, not guest execution time',
          'poll_seconds': .05, 'events': events}
(root / 'build/chrome-startup-marker-timing.json').write_text(
    json.dumps(report, indent=2) + '\n')
