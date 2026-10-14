"""Unused window extensions must not perturb every syscall user's line markers."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
header = (root/'include/uapi/linux_abi.h').read_text()
extension = ('/* VISIBILITY: width = visible (0/1), height = inactive (0/1, show only). */\n'
             '#define LINUX_WINDOW_VISIBILITY_INACTIVE 1U\n')
assert header.count(extension) == 1
legacy = header.replace(extension, '')
marker = '#define LINUX_WINDOW_IOCTL_BASE  0x50U\n'
assert legacy.count(marker) == 1
shifted = legacy.replace(marker, marker+extension)
with tempfile.TemporaryDirectory(prefix='system-abi-cache-') as directory:
    path = Path(directory)
    source = path/'probe.c'
    source.write_text('#include "linux_abi.h"\n'
                      'int layout_probe(void) { return sizeof(linux_window_command_t) + LINUX_WINDOW_OPERATION_RESIZE; }\n')
    preprocessed, assembled = {}, {}
    for label, text in (('legacy', legacy), ('current', header), ('shifted', shifted)):
        folder = path/label
        folder.mkdir()
        (folder/'linux_abi.h').write_text(text)
        command = ['cc', '-std=c11', '-I', str(folder), str(source)]
        output = subprocess.check_output(command+['-E'], text=True)
        preprocessed[label] = output.replace(str(folder/'linux_abi.h'), 'HEADER')
        assembled[label] = subprocess.check_output(command+['-S', '-O2', '-o', '-'], text=True)
    assert preprocessed['current'] == preprocessed['legacy'], 'Extension shifted unrelated preprocessor output'
    assert preprocessed['shifted'] != preprocessed['legacy'], 'Fixture must reproduce the old cache invalidation'
    assert assembled['legacy'] == assembled['current'] == assembled['shifted'], 'ABI or generated code changed'
print('ABI cache: original line markers retained; old insertion changes cache input without changing assembly')
