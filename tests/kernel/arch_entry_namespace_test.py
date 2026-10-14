"""Renaming architecture entries must not change interrupt machine code."""
from pathlib import Path
import hashlib
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
cases = (
    ('kernel/arch/x86_64/irq/exception.S', 4270,
     '324caa0e82544c1b20120c4618613ad92859a40667c4dda1a96fdc09936fcc17'),
    ('kernel/arch/x86_64/time/deadline.S', 265,
     '5f843ce4972e6a0a2620750b3dde174b355cb282772a53403a9c116435842757'),
    ('kernel/arch/x86_64/mm/uaccess.S', 241,
     'f5780ccc15e41fd6966d76c90a89ac859185cf575daad2d502b6175d1d8faece'),
)
with tempfile.TemporaryDirectory(prefix='arch-entry-') as directory:
    for index, (source, length, digest) in enumerate(cases):
        obj = Path(directory) / f'{index}.o'
        subprocess.run(['cc', '-m64', '-c', str(ROOT / source), '-o', str(obj)], check=True)
        code = subprocess.check_output(['objcopy', '-O', 'binary', '-j', '.text',
                                        str(obj), '/dev/stdout'])
        assert len(code) == length, source
        assert hashlib.sha256(code).hexdigest() == digest, source
        symbols = subprocess.check_output(['nm', '--defined-only', str(obj)], text=True)
        assert 'liteos_' not in symbols, source
        if index == 0:
            for family, numbers in (('exception', range(32)), ('irq', range(33, 255))):
                found = {int(n) for n in re.findall(r'\barch_' + family + r'_(\d+)\b', symbols)}
                assert found == set(numbers), family
            assert 'arch_exception_stub_table' in symbols
            assert 'arch_irq_stub_table' in symbols
        elif index == 1:
            for name in ('lapic_timer', 'lapic_spurious', 'smp_ipi', 'tlb_ipi'):
                assert f'arch_{name}_entry' in symbols, name
        else:
            boundaries = {name: int(address, 16) for address, name in
                          re.findall(r'^([0-9a-f]+)\s+\w\s+(arch_uaccess_table_\w+)$', symbols, re.M)}
            assert boundaries['arch_uaccess_table_end'] - boundaries['arch_uaccess_table_start'] == 64
cpu = (ROOT / 'kernel/arch/x86_64/cpu/cpu.c').read_text()
for name, value in (('OS_GDT_CODE_SELECTOR', '0x08U'), ('OS_IDT_ENTRIES', '256U'),
                    ('OS_IST_STACK_SIZE', '(32U * 1024U)'),
                    ('OS_RING0_STACK_RESERVE', '(64U * 1024U)')):
    assert re.search(r'#define\s+' + name + r'\s+' + re.escape(value), cpu), name
assert not re.search(r'\bliteos_\w+|\bLITEOS_\w+', cpu)
print('Architecture entries: unchanged machine code, 254 vector stubs, IPI entries and uaccess table bounds passed')
