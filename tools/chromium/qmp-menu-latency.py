"""Measure visible menu response; requires Wikipedia at top, mouse (400,72)."""
import json
import pathlib
import tempfile
import time
import statistics
import argparse

root = pathlib.Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
parser = argparse.ArgumentParser()
parser.add_argument('--move-x', type=int, default=75)
parser.add_argument('--move-y', type=int, default=645)
parser.add_argument('--probe', choices=('menu', 'button', 'scroll'), default='menu')
args = parser.parse_args()
region = (120,775,820,1000) if args.probe == 'menu' else (600,700,642,735)
threshold = 15000 if args.probe == 'menu' else 30
if args.probe == 'scroll':
    region = (200,300,450,600)
    threshold = 1500
qmp = QMPClient()
def command(name, arguments=None):
    return qmp.command(name, arguments)

command('input-send-event', {'events': [
    {'type': 'rel', 'data': {'axis': 'x', 'value': args.move_x}},
    {'type': 'rel', 'data': {'axis': 'y', 'value': args.move_y}}]})
latencies = []
first_changes = []
with tempfile.TemporaryDirectory(prefix='system-menu-latency-') as directory:
    filename = str(pathlib.Path(directory) / 'frame.ppm')
    def frame():
        command('screendump', {'filename': filename})
        with open(filename, 'rb') as image:
            assert image.readline().strip() == b'P6'
            width, height = map(int, image.readline().split())
            assert image.readline().strip() == b'255' and height >= 1000
            pixels = image.read()
        left,top,right,bottom = region
        return b''.join(pixels[(y*width+left)*3:(y*width+right)*3]
                        for y in range(top,bottom))
    capture_times = []
    for _ in range(10):
        capture_start = time.monotonic()
        frame()
        capture_times.append((time.monotonic()-capture_start)*1000)
    for sample in range(20):
        before = frame()
        start = time.monotonic()
        first_change = None
        button = ('wheel-down' if sample % 2 == 0 else 'wheel-up') if args.probe == 'scroll' else 'left'
        for down in (True, False):
            command('input-send-event', {'events': [
                {'type': 'btn', 'data': {'button': button, 'down': down}}]})
            if down:
                time.sleep(.02)
        while time.monotonic()-start < 10:
            after = frame()
            changed_channels = sum(abs(a-b)>20 for a,b in zip(before,after))
            if changed_channels > min(150,threshold) and first_change is None:
                first_change = (time.monotonic()-start)*1000
            if changed_channels > threshold:
                latency = (time.monotonic()-start)*1000
                latencies.append(latency)
                first_changes.append(first_change)
                print(f'{sample+1}: first={first_change:.1f}, substantial={latency:.1f} ms', flush=True)
                break
            time.sleep(.02)
        else:
            raise RuntimeError(f'No visible menu response on sample {sample+1}')
        time.sleep(.35)
report = {'samples': len(latencies), 'median_ms': statistics.median(latencies),
          'probe': args.probe,
          'max_ms': max(latencies), 'measurements_ms': latencies,
          'capture_median_ms': statistics.median(capture_times),
          'first_change_median_ms': statistics.median(first_changes),
          'first_change_max_ms': max(first_changes),
          'first_change_measurements_ms': first_changes,
          'includes': '20ms button hold, QMP input, PPM capture and image comparison'}
(root / f'build/chrome-{args.probe}-latency.json').write_text(json.dumps(report, indent=2)+'\n')
print(json.dumps(report))
