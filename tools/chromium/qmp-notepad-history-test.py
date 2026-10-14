"""Fresh canonical desktop: actual eight-level editor undo/redo and branching.

Only a temporary unsaved document is edited. Leave the desktop fresh for the
next test; never save, overwrite notes, or stop the virtual machine.
"""
from pathlib import Path
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
from qemu_frame import read_qemu_frame

with QMPClient() as qmp, tempfile.TemporaryDirectory(prefix='system-editor-history-') as directory:
    filename = str(Path(directory) / 'frame.ppm')
    def frame():
        time.sleep(.15)
        qmp.command('screendump', {'filename': filename})
        pixels = read_qemu_frame(filename)
        return b''.join(pixels[((175 + row) * 1920 + 250) * 3:
                               ((175 + row) * 1920 + 1100) * 3] for row in range(100))

    assert qmp.command('query-status')['running']
    qmp.click(815, 1020)
    time.sleep(.5)
    codes = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus'}
    for character in 'notepad /tmp/history-qa.txt':
        qmp.key(codes.get(character, character))
    qmp.key('ret')
    time.sleep(.8)
    qmp.move(1800, 960)
    frames = [frame()]
    for character in 'abcdefghijkl':
        qmp.key(character)
        frames.append(frame())
        assert frames[-1] != frames[-2], 'Typed character did not change the editor'

    for number in range(8):
        qmp.key('ctrl', 'z')
        assert frame() == frames[11 - number], f'Undo {number + 1} restored the wrong document'
    qmp.key('ctrl', 'z')
    assert frame() == frames[4], 'Undo exceeded the eight-entry history limit'
    for number in range(8):
        qmp.key('ctrl', 'y')
        assert frame() == frames[5 + number], f'Redo {number + 1} restored the wrong document'
    qmp.key('ctrl', 'y')
    assert frame() == frames[12], 'Empty redo changed the document'
    print('Editor history: eight undos/redos, eviction limit and exact text/caret frames passed', flush=True)

    for _ in range(3):
        qmp.key('ctrl', 'z')
    assert frame() == frames[9]
    qmp.key('z')
    branch = frame()
    assert branch != frames[9] and branch != frames[10]
    qmp.key('ctrl', 'y')
    assert frame() == branch, 'Editing did not invalidate the previous redo branch'
    qmp.key('ctrl', 'z')
    assert frame() == frames[9]
    qmp.key('ctrl', 'y')
    assert frame() == branch
    qmp.screenshot(root / 'build/notepad-history-verified.png')
    print('Editor history: new edit invalidates redo; branch undo/redo restores exact pixels', flush=True)

    qmp.key('ctrl', 'q')  # Discard only this temporary unsaved test document.
    qmp.key('left')  # Confirmation defaults to Cancel; choose Discard.
    qmp.key('ret')
    time.sleep(.5)
    qmp.click(1664, 155)  # Close only our now-idle test shell.
    assert qmp.command('query-status')['running']
