"""Verify neutral adapter paths and their real compiler include resolution."""
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]
make = (ROOT / 'makefile').read_text()
assert 'NASM_SMOKE_IMAGE' not in make
assert '/esp/tmp/nasm-smoke.asm' not in make
assert (ROOT / 'tests/user/nasm_smoke.asm').is_file()
assert not (ROOT / 'tools/nasm-liteos').exists()
assert not (ROOT / 'third_party/blend2d/liteos').exists()
config = ROOT / 'tools/nasm-platform/config/config.h'
text = config.read_text()
assert '#define NASM_PLATFORM_CONFIG_H 1' in text
assert 'LITEOS' not in text and 'LiteOS' not in text
assert '#define PACKAGE_VERSION "3.02"' in text
assert '#define WORDS_LITTLEENDIAN 1' in text
for feature in ('MMAP', 'FSEEKO', 'FTRUNCATE', 'SYS_STAT_H', 'UINTPTR_T'):
    assert f'#define HAVE_{feature} 1' in text, feature
for file in (ROOT / 'third_party/blend2d/platform/include').rglob('*.h'):
    for relative in re.findall(r'#include\s+"([^"]+)"', file.read_text()):
        assert (file.parent / relative).is_file(), (file, relative)
targets = (
    ('build/nasm/asm/nasm.o', '-Itools/nasm-platform'),
    ('build/elf/blend2d-cxx-runtime.o', '-Ithird_party/blend2d/platform/cxx'),
)
for target, include in targets:
    recipe = subprocess.check_output(['make', '-s', '-n', '-B', '-f', 'GNUmakefile',
        target, 'BUILD=build', 'OS_NVIDIA=1', 'OS_DEBUG_SERIAL=1',
        'OS_INTERACTIVE_DESKTOP=1'], cwd=ROOT, text=True)
    line = next(line for line in recipe.splitlines() if ' -c ' in line and ' -o ' + target in line)
    args = shlex.split(line)
    assert include in args, (target, include)
    assert not any('nasm-liteos' in arg or '/blend2d/liteos/' in arg for arg in args)
    index = args.index('-o')
    del args[index:index + 2]
    args = [arg for arg in args if arg not in ('-c', '-MMD', '-MP')] + ['-E', '-P']
    result = subprocess.check_output(args, cwd=ROOT)
    assert result, target
print('Adapter paths: NASM features, all header forwarders and actual NASM/C++ preprocessing passed')
