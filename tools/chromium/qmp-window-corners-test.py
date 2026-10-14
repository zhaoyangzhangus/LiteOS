"""Canonical desktop: compositor-owned four-corner alpha and maximize/restore."""
from pathlib import Path
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
from qemu_frame import read_qemu_frame

with QMPClient() as qmp, tempfile.TemporaryDirectory(prefix='system-window-corners-') as directory:
    filename = str(Path(directory) / 'frame.ppm')
    def frame():
        qmp.move(1800, 960)
        time.sleep(.2)
        qmp.command('screendump', {'filename': filename})
        return read_qemu_frame(filename)
    def pixel(pixels, x, y):
        offset = (y * 1920 + x) * 3
        return pixels[offset:offset + 3]
    def verify(pixels):
        header = bytes.fromhex('d3e3fd')
        border = bytes((c * 217 + 127) // 255 for c in header)
        for right in (False, True):
            for bottom in (False, True):
                x0, y0 = (1679 if right else 240), (944 if bottom else 135)
                assert pixel(pixels, x0, y0) == pixel(background, x0, y0), 'Transparent corner overwrote wallpaper'
                x, y = x0 + (-6 if right else 6), y0
                under = pixel(background, x, y)
                expected = bytes((a * 215 + b * 40 + 127) // 255 for a, b in zip(border, under))
                assert pixel(pixels, x, y) == expected, f'Corner alpha mismatch: right={right}, bottom={bottom}'
    assert qmp.command('query-status')['running']
    background = frame()
    qmp.click(912, 1020)
    time.sleep(.8)
    verify(frame())
    qmp.screenshot(root / 'build/compositor-window-four-corners.png')
    qmp.click(1632, 155)
    time.sleep(.3)
    maximized = frame()
    assert pixel(maximized, 0, 0) == bytes.fromhex('d3e3fd')
    assert pixel(maximized, 1919, 1079) == b'\xff\xff\xff'
    qmp.click(1872, 20)
    time.sleep(.3)
    verify(frame())
    qmp.click(1664, 155)  # Close only this unchanged QA editor.
    assert qmp.command('query-status')['running']
print('Guest compositor: four transparent corners preserve wallpaper, exact 215/255 edge coverage on all four corners, bottom rounding, square maximized surface and restored geometry passed')
