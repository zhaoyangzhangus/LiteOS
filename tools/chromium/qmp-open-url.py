"""Navigate the existing guest Chrome using guest keys; save visible evidence."""
import argparse
import json
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('url')
parser.add_argument('--label', default='online-probe')
parser.add_argument('--observe-seconds', type=int, default=30)
parser.add_argument('--save-samples', action='store_true',
                    help='Retain timestamped screenshots, not only the final frame')
parser.add_argument('--network-snapshot-after', type=int, default=0,
                    help='Capture a bounded guest network snapshot during loading; 0 disables')
args = parser.parse_args()
if not args.label.replace('-', '').isalnum() or not 1 <= args.observe_seconds <= 120:
    parser.error('invalid label or observation interval')
if not 0 <= args.network_snapshot_after < args.observe_seconds:
    parser.error('network-snapshot-after must be 0 or less than observe-seconds')
punctuation = {':': ('shift', 'semicolon'), '/': ('slash',), '.': ('dot',),
               '-': ('minus',), '_': ('shift', 'minus'), '?': ('shift', 'slash'),
               '=': ('equal',), '&': ('shift', '7'), '%': ('shift', '5'),
               ',': ('comma',), '#': ('shift', '3'), '!': ('shift', '1')}
keys = []
for char in args.url:
    if char in punctuation:
        keys.append(punctuation[char])
    elif char.isascii() and char.isalnum():
        keys.append(('shift', char.lower()) if char.isupper() else (char,))
    else:
        parser.error(f'unsupported URL character: {char!r}')
root = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
if args.network_snapshot_after and not (root/'build/qemu-running-kernel.elf').is_file():
    parser.error('Missing symbols for the running guest kernel')
qmp = QMPClient()

def command(name, arguments=None):
    return qmp.command(name, arguments)

def key(*codes, hold=40):
    # Complete the whole chord before typing the next character. send-key
    # schedules members separately and can leave Ctrl/Shift held here.
    try:
        command('input-send-event', {'events': [
            {'type': 'key', 'data': {'down': True,
             'key': {'type': 'qcode', 'data': code}}} for code in codes]})
        time.sleep(hold / 1000)
    finally:
        command('input-send-event', {'events': [
            {'type': 'key', 'data': {'down': False,
             'key': {'type': 'qcode', 'data': code}}} for code in reversed(codes)]})
    time.sleep(.05)


assert command('query-status')['running'], 'Guest already stopped; leave debugger state alone'
key('ctrl', 'l', hold=100)
for codes in keys:
    key(*codes)
# Omnibox inline completion selects an untyped suffix (e.g. the previous
# /search?q=... when navigating to the bare origin). Forward Delete removes
# that selection and suppresses inline completion; at an unselected end it is
# a no-op. Enter must navigate the requested URL, not the history completion.
key('delete')
command('screendump', {'filename': str(root / f'build/chrome-{args.label}-typed.png'),
                      'format': 'png'})
key('ret')
start = time.monotonic()
snapshot_done = False
snapshot_pause_seconds = 0.0
samples = []
sample_directory = root/f'build/chrome-{args.label}-samples'
if args.save_samples:
    sample_directory.mkdir(parents=True, exist_ok=True)
for sample in range(args.observe_seconds):
    if (args.network_snapshot_after and not snapshot_done and
            time.monotonic()-start >= args.network_snapshot_after):
        assert command('query-status')['running'], 'Guest stopped before snapshot'
        snapshot_done = True
        snapshot_start = time.monotonic()
        try:
            command('human-monitor-command', {'command-line':
                'gdbserver unix:/tmp/system-network.sock,server=on,wait=off'})
            with (root/f'build/chrome-{args.label}-network.log').open('w') as log:
                result = subprocess.run(['gdb','-q','-nx','-batch',
                    str(root/'build/qemu-running-kernel.elf'), '-x',
                    str(root/'tools/chromium/inspect-network-reuse.gdb')],
                    cwd=root,stdout=log,stderr=subprocess.STDOUT,timeout=10)
                print(f'Network snapshot GDB exit: {result.returncode}',flush=True)
        finally:
            command('human-monitor-command', {'command-line':'gdbserver none'})
            command('cont')
            assert command('query-status')['running'], 'Guest did not resume'
            snapshot_pause_seconds += time.monotonic()-snapshot_start
    command('screendump', {'filename': str(root / f'build/chrome-{args.label}.png'),
                          'format': 'png'})
    if args.save_samples:
        filename = sample_directory/f'{sample:03d}.png'
        before = time.monotonic()-start
        command('screendump', {'filename':str(filename),'format':'png'})
        samples.append({'file':str(filename.relative_to(root)),
                        'capture_start_seconds':before,
                        'capture_end_seconds':time.monotonic()-start})
    if sample % 5 == 0:
        print(f'observing navigation: {time.monotonic() - start:.1f}s', flush=True)
    time.sleep(1)
report = {'url': args.url, 'observation_seconds': time.monotonic() - start,
                  'diagnostic_pause_seconds': snapshot_pause_seconds,
                  'verification': 'Inspect typed URL and screenshots; command delivery alone is not a pass'}
if args.save_samples:
    report['samples'] = samples
    (sample_directory/'timing.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({key:value for key,value in report.items() if key != 'samples'}))
