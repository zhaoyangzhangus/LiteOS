"""Verify scrolling a loaded article in the single existing QEMU."""
import argparse
import json
from pathlib import Path
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--cycles', type=int, default=10)
args = parser.parse_args()
if not 1 <= args.cycles <= 100:
    parser.error('cycles must be 1..100')
root = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
qmp = QMPClient()

def command(name, arguments=None):
    return qmp.command(name, arguments)

def relative(dx, dy):
    while dx or dy:
        x, y = max(-100, min(100, dx)), max(-100, min(100, dy))
        command('input-send-event', {'events': [
            {'type': 'rel', 'data': {'axis': 'x', 'value': x}},
            {'type': 'rel', 'data': {'axis': 'y', 'value': y}}]})
        dx, dy = dx-x, dy-y
        time.sleep(.1)

def wheel(button):
    for tick in range(3):
        for down in (True, False):
            command('input-send-event', {'events': [
                {'type': 'btn', 'data': {'button': button, 'down': down}}]})
        time.sleep(.04)


assert command('query-status')['running'], 'Guest not running'
relative(-2000, -1200)
relative(920, 400)
time.sleep(.5)
results = []
with tempfile.TemporaryDirectory(prefix='system-scroll-') as directory:
    filename = str(Path(directory) / 'frame.ppm')

    def frame():
        command('screendump', {'filename': filename})
        with open(filename, 'rb') as image:
            assert image.readline().strip() == b'P6'
            width, height = map(int, image.readline().split())
            assert width >= 950 and height >= 900
            assert image.readline().strip() == b'255'
            pixels = image.read()
        return b''.join(pixels[(y*width+x)*3:(y*width+x)*3+3]
                        for y in range(200, 550, 4) for x in range(100, 850, 4))

    def difference(a, b):
        return sum(abs(x-y) > 20 for x, y in zip(a, b)) / len(a)

    baseline = frame()
    command('screendump', {'filename': str(root / 'build/chrome-scroll-baseline.png'), 'format': 'png'})
    for cycle in range(args.cycles):
        for button in ('wheel-down', 'wheel-up'):
            started = time.monotonic()
            wheel(button)
            while time.monotonic()-started < 5:
                mismatch = difference(baseline, frame())
                if (button == 'wheel-down' and mismatch > .05) or (
                        button == 'wheel-up' and mismatch < .02):
                    results.append({'cycle': cycle+1, 'button': button,
                                    'seconds': time.monotonic()-started,
                                    'baseline_mismatch': mismatch})
                    break
                time.sleep(.05)
            else:
                command('screendump', {'filename': str(root / 'build/chrome-scroll-failure.png'), 'format': 'png'})
                raise RuntimeError(f'{button}: article did not change/restore, mismatch={mismatch}')
            if cycle == 0 and button == 'wheel-down':
                command('screendump', {'filename': str(root / 'build/chrome-scroll-down.png'), 'format': 'png'})
            print(json.dumps(results[-1]), flush=True)
            time.sleep(.5)
    command('screendump', {'filename': str(root / 'build/chrome-scroll-final.png'), 'format': 'png'})
(root / 'build/chrome-scroll-soak.json').write_text(json.dumps({
    'results': results,
    'includes': 'Three wheel ticks, delivery spacing and screenshot polling; not pure renderer latency',
    'verification': 'Sampled article pixels change and restore; manually inspect baseline/down/final screenshots'
}, indent=2) + '\n')
