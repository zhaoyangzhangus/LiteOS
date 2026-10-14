"""Exercise the actual Chrome three-dot menu in the canonical running guest.

Requires the inspected unmaximized Chrome geometry (origin 10,10; width 945).
Pixel checks cover menu painting/dismissal, not menu command semantics.
"""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import tempfile
import time

def menu_is_painted(width, pixels):
    # White menus over white pages can have very little pixel difference.
    # Identify the full white panel and its text, not the old broken black
    # alpha border. A small tooltip/blank surface cannot meet this area gate.
    ink = white = panel = 0
    for y in range(200, 650, 5):
        for x in range(600, 900, 10):
            at = (y * width + x) * 3
            white += min(pixels[at:at+3]) > 250
            ink += max(pixels[at:at+3]) < 120
            panel += 1
    return ink > 20 and white > panel * .7

def menu_has_black_border(width, pixels):
    # Inspected normal menu over a bright page: the old alpha bug produced
    # an almost solid black strip for the full height, not a soft shadow.
    samples = [max(pixels[(y*width+x)*3:(y*width+x)*3+3])
               for y in range(200,650,5) for x in range(538,550,3)]
    return sum(value < 20 for value in samples) > len(samples)*.9

parser = argparse.ArgumentParser()
parser.add_argument('--cycles', type=int, default=5)
parser.add_argument('--capture-stall-after-ms', type=int, default=0,
                    help='One bounded read-only GDB snapshot on a slow menu; 0 disables')
args = parser.parse_args()
if not 1 <= args.cycles <= 100:
    parser.error('cycles must be between 1 and 100')
if args.capture_stall_after_ms and not 100 <= args.capture_stall_after_ms <= 3000:
    parser.error('stall capture threshold must be 100..3000 ms, or 0')
root = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
qmp = QMPClient()

def command(name, arguments=None):
    return qmp.command(name, arguments)

def relative(dx, dy):
    while dx or dy:
        x, y = max(-100,min(100,dx)), max(-100,min(100,dy))
        command('input-send-event', {'events':[
            {'type':'rel','data':{'axis':'x','value':x}},
            {'type':'rel','data':{'axis':'y','value':y}}]})
        dx -= x
        dy -= y
        time.sleep(.05)

def escape():
    command('send-key', {'keys':[{'type':'qcode','data':'esc'}], 'hold-time':40})

def capture(label):
    command('screendump', {'filename':str(root/f'build/chrome-app-menu-{label}.png'),
                          'format':'png'})


assert command('query-status')['running'], 'Guest must already be running'
escape()
time.sleep(.2)
relative(-2000,-1200)
relative(928,72)
results = []
diagnostic_taken = False
diagnostic_pause_ms = 0.0
with tempfile.TemporaryDirectory(prefix='system-app-menu-') as directory:
    filename = str(Path(directory)/'frame.ppm')
    def frame():
        command('screendump', {'filename':filename})
        with open(filename,'rb') as image:
            assert image.readline().strip() == b'P6'
            width,height = map(int,image.readline().split())
            assert image.readline().strip() == b'255'
            assert width >= 950 and height >= 1000
            pixels = image.read()
        # Exclude the pointer, toolbar and page scrollbar.
        painted = menu_is_painted(width,pixels)
        if painted and menu_has_black_border(width,pixels):
            capture('black-border-failure')
            raise RuntimeError('Menu has an opaque black alpha border')
        return (b''.join(pixels[(y*width+660)*3:(y*width+900)*3]
                        for y in range(150,650)), painted)

    def wait_for(reference, predicate, start, stage):
        global diagnostic_taken, diagnostic_pause_ms
        paused = 0.0
        first_visual_change_ms = None
        while time.monotonic()-start-paused < 5:
            current, painted = frame()
            changed = sum(abs(a-b)>20 for a,b in zip(reference,current))
            # The full-panel gate below requires a nearly opaque menu.
            # Its fade-in can take much longer than the first visible paint;
            # do not label animation completion as first-response latency.
            if stage == 'open' and changed > 2000 and first_visual_change_ms is None:
                first_visual_change_ms = (time.monotonic()-start-paused)*1000
            if predicate(changed,painted):
                return (time.monotonic()-start-paused)*1000, changed, first_visual_change_ms
            if (args.capture_stall_after_ms and not diagnostic_taken and
                    (time.monotonic()-start)*1000 >= args.capture_stall_after_ms):
                diagnostic_taken = True
                pause_start = time.monotonic()
                capture('stall')
                try:
                    command('human-monitor-command', {'command-line':
                        'gdbserver unix:/tmp/system-waits.sock,server=on,wait=off'})
                    with (root/'build/chrome-app-menu-stall.log').open('w') as log:
                        subprocess.run(['gdb', '-q', '-nx', '-batch',
                            str(root/'build/qemu-running-kernel.elf'), '-x',
                            str(root/'tools/chromium/inspect-menu-stall.gdb')],
                            cwd=root, stdout=log, stderr=subprocess.STDOUT,
                            timeout=10, check=True)
                finally:
                    command('human-monitor-command', {'command-line':'gdbserver none'})
                    command('cont')
                    assert command('query-status')['running']
                    elapsed = time.monotonic()-pause_start
                    paused += elapsed
                    diagnostic_pause_ms += elapsed*1000
            time.sleep(.02)
        capture('failure')
        capture(f'{stage}-failure')
        raise RuntimeError(f'Menu {stage} did not meet pixel gate within 5 seconds; '
                           f'last changed_channels={changed}')

    for cycle in range(args.cycles):
        before, painted = frame()
        if painted:
            capture('baseline-failure')
            raise RuntimeError('Menu already visible before click; baseline is invalid')
        start = time.monotonic()
        for down in (True,False):
            command('input-send-event', {'events':[
                {'type':'btn','data':{'button':'left','down':down}}]})
            if down:
                time.sleep(.02)
        opened,changed,first_change = wait_for(before,lambda count,painted:painted,start,'open')
        capture('open')
        start = time.monotonic()
        escape()
        closed,remaining,_ = wait_for(before,lambda count,painted:not painted and count<2000,start,'dismiss')
        capture('closed')
        result = {'cycle':cycle+1,'open_ms':opened,'dismiss_ms':closed,
                  'first_visual_change_ms':first_change,
                  'open_changed_channels':changed,'remaining_changed_channels':remaining}
        results.append(result)
        print(json.dumps(result),flush=True)
        time.sleep(.2)
report = {'measurements':results,
          'open_median_ms':statistics.median(r['open_ms'] for r in results),
          'first_visual_change_median_ms':statistics.median(r['first_visual_change_ms'] for r in results),
          'dismiss_median_ms':statistics.median(r['dismiss_ms'] for r in results),
          'diagnostic_taken':diagnostic_taken,
          'diagnostic_pause_ms':diagnostic_pause_ms,
          'diagnostic_limitation':'Captured runs are perturbed; reported waits exclude snapshot wall time',
          'includes':'Input hold, QMP delivery, screenshot capture and pixel comparison',
          'scope':'First visible ROI change, nearly opaque menu completion and Escape dismissal; inspect screenshots, not exact event latency'}
(root/'build/chrome-app-menu-test.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report))
