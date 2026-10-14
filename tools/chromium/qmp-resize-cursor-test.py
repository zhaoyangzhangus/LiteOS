"""Fresh canonical desktop: eight enlarged hit areas and native Windows pixels."""
import argparse
from pathlib import Path
import struct
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--windows-cursors', type=Path, required=True)
args = parser.parse_args()
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
from qemu_frame import read_qemu_frame

def native_cursor(name):
    data = (args.windows_cursors / (name + '.cur')).read_bytes()
    assert struct.unpack_from('<HH', data) == (0, 2)
    count = struct.unpack_from('<H', data, 4)[0]
    entries = [struct.unpack_from('<BBBBHHII', data, 6 + 16 * i) for i in range(count)]
    _, _, _, _, hx, hy, size, offset = next(e for e in entries if e[:2] == (32, 32))
    assert offset + size <= len(data)
    assert struct.unpack_from('<IiiHHI', data, offset) == (40, 32, 64, 1, 32, 0)
    pixels = [struct.unpack_from('4B', data, offset + 40 + ((31-y)*32+x)*4)
              for y in range(24) for x in range(24)]
    return hx, hy, pixels

with QMPClient() as qmp, tempfile.TemporaryDirectory(prefix='system-windows-cursor-') as directory:
    filename = str(Path(directory) / 'frame.ppm')
    def frame():
        time.sleep(.2)
        qmp.command('screendump', {'filename': filename})
        return read_qemu_frame(filename)
    assert qmp.command('query-status')['running']
    qmp.click(912, 1020)  # Open an editor for hovering only; never modify or save it.
    time.sleep(.8)
    qmp.move(1800, 960)
    baseline = frame()
    routes = [('left',245,800,'aero_ew'), ('right',1674,800,'aero_ew'),
              ('top',1100,140,'aero_ns'), ('bottom',1100,939,'aero_ns'),
              ('top-left',245,140,'aero_nwse'), ('top-right',1674,140,'aero_nesw'),
              ('bottom-left',245,939,'aero_nesw'), ('bottom-right',1674,939,'aero_nwse')]
    for label, x, y, name in routes:
        hx, hy, pixels = native_cursor(name)
        qmp.move(x, y)
        observed = frame()
        for index, (blue, green, red, alpha) in enumerate(pixels):
            offset = ((y-hy+index//24)*1920+x-hx+index%24)*3
            original = baseline[offset:offset+3]
            expected = bytes((old*(255-alpha)+value*alpha+127)//255
                             for old, value in zip(original,(red,green,blue)))
            assert observed[offset:offset+3] == expected, f'{label}: native cursor pixel {index} differs'
        qmp.screenshot(root / f'build/windows-resize-cursor-{label}.png')
        print(f'{label}: 8px hit area, Windows pixels and hotspot {hx},{hy} matched', flush=True)
    qmp.move(1800, 960)
    assert frame() == baseline, 'Resize cursor left stale pixels after leaving the border'
    qmp.click(1664, 155)  # The editor has no changes; close only this test window.
    assert qmp.command('query-status')['running']
