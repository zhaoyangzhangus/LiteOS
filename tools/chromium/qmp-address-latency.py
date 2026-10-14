"""Measure address-field focus painting in the existing guest, without navigation."""
import json
import argparse
from pathlib import Path
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--samples', type=int, default=5)
parser.add_argument('--reload-before-click', action='store_true',
                    help='Measure input while the current online page reloads')
args = parser.parse_args()
if not 1 <= args.samples <= 100:
    parser.error('samples must be between 1 and 100')
def toolbar_extent(width, pixels):
    # Separate disconnected pale tooltip/desktop pixels from the toolbar.
    # Small gaps are toolbar glyphs; a large gap cannot be part of its frame.
    runs = []
    for x in range(width):
        if sum(min(pixels[(y*width+x)*3:(y*width+x)*3+3]) >= 200
               for y in (84,86,88,90,92)) < 3:
            continue
        if not runs or x-runs[-1][-1] > 32:
            runs.append([])
        runs[-1].append(x)
    candidates = [r for r in runs if r[-1]-r[0]+1 >= 600 and
                  len(r)/(r[-1]-r[0]+1) >= .9]
    if len(candidates) != 1:
        return None
    return candidates[0][0], candidates[0][-1]+1

def address_is_focused(width, pixels, left):
    # Navigation can change URL text while the page reloads. Require the
    # actual blue focus ring, not just a text/spinner pixel difference.
    blue = 0
    for y in (56,57,89,90):
        for x in range(left+140,left+340):
            at=(y*width+x)*3
            r,g,b=pixels[at:at+3]
            blue += b>180 and r<110 and g<170
    if blue > 300:
        return True
    # A populated omnibox opens its suggestion popup instead of retaining
    # the empty-field ring. The selected URL is the other focus indicator.
    selected = 0
    for y in range(65,83):
        for x in range(left+175,left+340):
            at=(y*width+x)*3
            r,g,b=pixels[at:at+3]
            selected += b>180 and r<110 and g<170
    return selected > 300

root = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
qmp = QMPClient()

def command(name, arguments=None):
    deadline = time.monotonic() + 10
    while True:
        try:
            return qmp.command(name, arguments)
        except RuntimeError as error:
            response = error.args[0] if error.args else {}
            # GDB-paused input was rejected, not delivered; retry only that case.
            if (name == 'input-send-event' and isinstance(response, dict) and
                    response.get('error', {}).get('desc') == 'VM not running' and
                    time.monotonic() < deadline):
                print('Diagnostic pause: input rejected; retrying', flush=True)
                time.sleep(.1)
                continue
            raise

position = [0, 0]

def relative(dx, dy):
    # Keep each packet within boot HID's signed 8-bit movement range.
    while dx or dy:
        x = max(-100, min(100, dx))
        y = max(-100, min(100, dy))
        command('input-send-event', {'events': [
            {'type': 'rel', 'data': {'axis': 'x', 'value': x}},
            {'type': 'rel', 'data': {'axis': 'y', 'value': y}}]})
        dx -= x
        dy -= y
        time.sleep(.1)

def move(x, y):
    relative(x - position[0], y - position[1])
    position[:] = [x, y]

class MouseHold:
    """Observe while held; always release, including partial-delivery errors."""
    def __init__(self):
        self.down = False
        self.release_due = 0.0
        self.release_started = None
    def __enter__(self):
        return self
    def press(self):
        self.down = True
        self.release_due = time.monotonic() + .02
        self.release_started = None
        command('input-send-event', {'events': [
            {'type': 'btn', 'data': {'button': 'left', 'down': True}}]})
    def release(self, wait=False):
        if not self.down:
            return
        remaining = self.release_due - time.monotonic()
        if remaining > 0:
            if not wait:
                return
            time.sleep(remaining)
        self.release_started = time.monotonic()
        command('input-send-event', {'events': [
            {'type': 'btn', 'data': {'button': 'left', 'down': False}}]})
        self.down = False
    def __exit__(self, kind, value, traceback):
        self.release(wait=True)

def key_chord(*codes, hold_ms):
    # send-key delays each chord member, not just the final release. Do not
    # click while Ctrl is still held or let one shortcut modify the next.
    try:
        command('input-send-event', {'events': [
            {'type': 'key', 'data': {'down': True, 'key': {'type': 'qcode', 'data': code}}}
            for code in codes]})
        time.sleep(hold_ms/1000)
    finally:
        command('input-send-event', {'events': [
            {'type': 'key', 'data': {'down': False, 'key': {'type': 'qcode', 'data': code}}}
            for code in reversed(codes)]})


relative(-2000, -1200)
results = []
with tempfile.TemporaryDirectory(prefix='system-address-latency-') as directory, MouseHold() as mouse:
    path = str(Path(directory) / 'frame.ppm')

    def frame():
        capture_start = time.monotonic()
        command('screendump', {'filename': path})
        with open(path, 'rb') as image:
            assert image.readline().strip() == b'P6'
            width, height = map(int, image.readline().split())
            assert image.readline().strip() == b'255'
            assert width >= 950 and height >= 1000
            pixels = image.read()
        capture_end = time.monotonic()
        extent = toolbar_extent(width, pixels)
        if extent is None or extent[1]-extent[0] < 600:
            raise RuntimeError('No visible browser toolbar; do not click blindly')
        if ('expected_extent' in frame_state and
            any(abs(a-b) > 2 for a,b in zip(extent, frame_state['expected_extent']))):
            raise RuntimeError(f'Browser extent changed: {frame_state["expected_extent"]} -> {extent}; invalid sample, not a latency failure')
        frame_state.setdefault('expected_extent', extent)
        left = extent[0]
        frame_state['address_focused'] = address_is_focused(width,pixels,left)
        frame_state['capture_start'] = capture_start
        frame_state['capture_end'] = capture_end
        frame_state['capture_ms'] = (capture_end-capture_start)*1000
        # Exclude the mouse cursor at x=400 from the comparison region.
        return b''.join(pixels[(y * width + left+129) * 3:(y * width + left+339) * 3]
                        for y in range(56, 145))

    frame_state = {}
    frame()
    address_x = frame_state['expected_extent'][0]+389
    for index in range(args.samples):
        for down in (True, False):
            command('input-send-event', {'events': [{'type': 'key', 'data': {
                'down': down, 'key': {'type': 'qcode', 'data': 'esc'}}}]})
            time.sleep(.05)
        # Blur with keyboard focus traversal, not an assumed blank web-page
        # pixel (which can be a link when the user moves the window).
        for codes in (('ctrl', 'l'), ('f6',)):
            key_chord(*codes, hold_ms=80)
            time.sleep(.15)
        time.sleep(.5)
        move(address_x, 72)
        time.sleep(.1)
        if args.reload_before_click:
            key_chord('ctrl', 'r', hold_ms=40)
            # Release Ctrl before the ordinary click. Do not wait for load
            # completion: this probe intentionally overlaps reload work.
            time.sleep(.02)
        before = frame()
        start = time.monotonic()
        mouse.press()
        last_negative_start = start
        captures = []
        while time.monotonic() - start < 5:
            mouse.release()
            after = frame()
            captures.append(frame_state['capture_ms'])
            mouse.release()
            changed = sum(abs(a - b) > 20 for a, b in zip(before, after))
            if changed > 200 and frame_state['address_focused']:
                result = {'sample': index + 1,
                          'reload_before_click':args.reload_before_click,
                          'focus_paint_ms':
                          (time.monotonic() - start) * 1000, 'changed_channels': changed,
                          'visibility_interval_ms': [
                              (last_negative_start-start)*1000,
                              (frame_state['capture_end']-start)*1000],
                          'frame_capture_ms': captures}
                break
            last_negative_start = frame_state['capture_start']
            time.sleep(.005)
        else:
            result = {'sample': index + 1, 'timeout_seconds': 5}
        results.append(result)
        mouse.release(wait=True)
        result['mouse_release_ms'] = (mouse.release_started-start)*1000
        print(json.dumps(result), flush=True)
        time.sleep(.2)
    command('screendump', {'filename': str(root / 'build/chrome-address-focus.png'),
                          'format': 'png'})
(root / 'build/chrome-address-latency.json').write_text(json.dumps({
    'measurements': results,
    'includes': 'QMP input delivery, screenshot capture and pixel comparison; 20ms hold overlaps observation',
    'visibility_interval': 'Conservative last-negative-capture start to first-positive-capture end; not exact guest latency',
    'limitation': 'Focus paint only; does not prove suggestions or renderer responsiveness'
}, indent=2) + '\n')
