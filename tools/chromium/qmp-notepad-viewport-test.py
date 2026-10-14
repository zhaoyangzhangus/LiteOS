"""Fresh canonical desktop: zero-row editor hit testing and exact recovery."""
from pathlib import Path
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
from qemu_frame import read_qemu_frame

with QMPClient() as qmp, tempfile.TemporaryDirectory(prefix='system-editor-viewport-') as directory:
    filename = str(Path(directory) / 'frame.ppm')
    def frame():
        qmp.move(1800, 960)
        qmp.command('screendump', {'filename': filename})
        return read_qemu_frame(filename)
    def region(pixels, x, y, width, height):
        return b''.join(pixels[((y + row) * 1920 + x) * 3:
                               ((y + row) * 1920 + x + width) * 3] for row in range(height))
    def resize_bottom(old_y, new_y):
        qmp.move(1100, old_y)
        qmp.command('input-send-event', {'events': [
            {'type': 'btn', 'data': {'button': 'left', 'down': True}}]})
        try:
            remaining = new_y - old_y
            while remaining:
                step = max(-100, min(100, remaining))
                qmp.command('input-send-event', {'events': [
                    {'type': 'rel', 'data': {'axis': 'y', 'value': step}}]})
                remaining -= step
                time.sleep(.05)
        finally:
            qmp.command('input-send-event', {'events': [
                {'type': 'btn', 'data': {'button': 'left', 'down': False}}]})
        time.sleep(.3)

    assert qmp.command('query-status')['running']
    qmp.click(815, 1020)
    time.sleep(.5)
    codes = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus'}
    for character in 'notepad /tmp/viewport-qa.txt':
        qmp.key(codes.get(character, character))
    qmp.key('ret')
    time.sleep(.8)
    for character in 'abc':
        qmp.key(character)
    qmp.key('ret')
    for character in 'def':
        qmp.key(character)
    resize_bottom(944, 231)  # Server minimum is 96px: two complete rows and a partial row.
    qmp.click(250, 210)
    last_complete_row = frame()
    qmp.click(250, 225)
    assert frame() == last_complete_row, 'Partial bottom row mapped to an invisible line'
    resize_bottom(231, 944)
    qmp.key('ctrl', 'home')
    qmp.key('ctrl', 'end')
    normal = frame()
    for _ in range(8):
        qmp.key('ctrl', 'equal')
    resize_bottom(944, 255)  # 120px client: less than a complete 48px-font text row.
    small = frame()
    assert region(small, 1000, 235, 300, 4) == b'\xff' * (300 * 4 * 3)
    qmp.click(250, 235)  # Would move the cursor to zero if a partial row were clickable.
    for button in ('wheel-up', 'wheel-down'):
        qmp.move(1000, 235)
        qmp.command('input-send-event', {'events': [
            {'type': 'btn', 'data': {'button': button, 'down': True}},
            {'type': 'btn', 'data': {'button': button, 'down': False}}]})
    assert frame() == small, 'Invisible-row mouse/wheel changed the frame'
    qmp.screenshot(root / 'build/notepad-zero-viewport.png')
    resize_bottom(255, 944)
    for _ in range(8):
        qmp.key('ctrl', 'minus')
    restored = frame()
    assert region(restored, 250, 175, 850, 100) == region(normal, 250, 175, 850, 100), \
        'Text or cursor changed while no complete row was visible'
    qmp.screenshot(root / 'build/notepad-viewport-restored.png')
    qmp.key('ctrl', 'q')  # Discard only this temporary unsaved document.
    qmp.key('left')
    qmp.key('ret')
    time.sleep(.5)
    qmp.click(1664, 155)
    assert qmp.command('query-status')['running']
print('Editor viewport: partial-row mouse clamp, 48px font, zero-row click/wheel and exact text/cursor recovery passed')
