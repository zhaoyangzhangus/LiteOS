"""Fresh canonical desktop: large font, minimum height, wheel and full recovery."""
from pathlib import Path
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
from qemu_frame import read_qemu_frame
qmp = QMPClient()
command, move, click, key = qmp.command, qmp.move, qmp.click, qmp.key
assert command('query-status')['running']

def resize_bottom(y, direction):
    move(1100, y)
    command('input-send-event', {'events': [
        {'type': 'btn', 'data': {'button': 'left', 'down': True}}]})
    try:
        for _ in range(7):
            command('input-send-event', {'events': [
                {'type': 'rel', 'data': {'axis': 'y', 'value': direction * 102}}]})
            time.sleep(.05)
    finally:
        command('input-send-event', {'events': [
            {'type': 'btn', 'data': {'button': 'left', 'down': False}}]})
    move(1800, 960)
    time.sleep(.5)

with tempfile.TemporaryDirectory(prefix='system-shell-viewport-') as directory:
    filename = str(Path(directory) / 'frame.ppm')
    def frame():
        command('screendump', {'filename': filename})
        return read_qemu_frame(filename)
    def region(pixels, x, y, w, h):
        return b''.join(pixels[((y + row) * 1920 + x) * 3:
                               ((y + row) * 1920 + x + w) * 3] for row in range(h))
    click(815, 1020)
    time.sleep(.8)
    move(1800, 960)
    normal = frame()
    for _ in range(8):
        key('ctrl', 'equal')  # 16 -> 48; eight inverse steps restore exactly 16.
    time.sleep(.4)
    resize_bottom(944, -1)  # 810 -> 96 client height, no complete text row fits.
    small = frame()
    assert region(small, 1000, 220, 400, 6) == b'\xff' * (400 * 6 * 3)
    command('screendump', {'filename': str(root / 'build/shell-zero-viewport.png'),
                          'format': 'png'})
    click(1100, 220)  # Do not start a selection of an invisible output line.
    for button in ('wheel-up', 'wheel-down'):
        for down in (True, False):
            command('input-send-event', {'events': [
                {'type': 'btn', 'data': {'button': button, 'down': down}}]})
    move(1800, 960)
    time.sleep(.3)
    assert region(frame(), 1000, 220, 400, 6) == region(small, 1000, 220, 400, 6)
    resize_bottom(230, 1)
    for _ in range(8):
        key('ctrl', 'minus')
    time.sleep(.5)
    move(1800, 960)
    restored = frame()
    assert region(restored, 250, 175, 850, 115) == region(normal, 250, 175, 850, 115), \
        'Output or prompt did not recover after the zero-row viewport'
    command('screendump', {'filename': str(root / 'build/shell-viewport-restored.png'),
                          'format': 'png'})
    click(1664, 155)  # Close only the empty shell created by this test.
    assert command('query-status')['running']
qmp.close()
print('Shell viewport: 48px font, 96px client height, empty-body mouse/wheel and exact normal-frame recovery passed')
