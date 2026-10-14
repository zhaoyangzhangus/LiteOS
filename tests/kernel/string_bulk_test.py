"""Compile production string primitives without SIMD and exercise exact bounds."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='liteos-bulk-string-') as directory:
    obj = Path(directory) / 'string.o'
    exe = Path(directory) / 'test'
    flags = ['-O2', '-Wall', '-Wextra', '-Werror', '-fno-builtin',
             '-mno-sse', '-mno-mmx', '-fno-tree-vectorize',
             '-DOS_KERNEL_BUILD', '-I' + str(root/'include')]
    subprocess.run(['cc', *flags, '-Dmemcpy=liteos_memcpy',
                    '-Dmemset=liteos_memset', '-Dmemmove=liteos_memmove',
                    '-Dmemcmp=liteos_memcmp', '-c', str(root/'kernel/string.c'),
                    '-o', str(obj)], check=True)
    assembly = subprocess.check_output(['objdump', '-d', str(obj)], text=True)
    assert 'rep movsq' in assembly and 'rep stos' in assembly
    assert '\tstd' not in assembly, 'Backward copies must not expose DF to interrupts'
    assert not any(register in assembly for register in ('%xmm', '%ymm', '%zmm'))
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', '-fno-builtin',
                    '-fno-tree-vectorize', str(root/'tests/kernel/string_bulk_test.c'),
                    str(obj), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    # Deliberately route backward overlap into the forward helper: this
    # must compile and fail the original-data oracle, not a source check.
    original = (root/'kernel/string.c').read_text()
    negative = original.replace('    if (to < from) {', '    if (to != from) {')
    assert negative != original
    broken = Path(directory)/'broken.c'
    broken.write_text(negative)
    subprocess.run(['cc', *flags, '-Dmemcpy=liteos_memcpy',
                    '-Dmemset=liteos_memset', '-Dmemmove=liteos_memmove',
                    '-Dmemcmp=liteos_memcmp', '-c', str(broken), '-o', str(obj)], check=True)
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', '-fno-builtin',
                    '-fno-tree-vectorize', str(root/'tests/kernel/string_bulk_test.c'),
                    str(obj), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    assert result.returncode != 0 and 'overlap[i]==expected[i]' in result.stderr, result.stderr
    print('Negative control rejected: forward copying corrupts backward overlap')
    # A broken word offset must fail the complete-buffer oracle, rather
    # than merely satisfying the no-SIMD assembly checks.
    negative = original.replace('length -= sizeof(word);', 'length = 0;')
    assert negative != original
    broken.write_text(negative)
    subprocess.run(['cc', *flags, '-Dmemcpy=liteos_memcpy',
                    '-Dmemset=liteos_memset', '-Dmemmove=liteos_memmove',
                    '-Dmemcmp=liteos_memcmp', '-c', str(broken), '-o', str(obj)], check=True)
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', '-fno-builtin',
                    '-fno-tree-vectorize', str(root/'tests/kernel/string_bulk_test.c'),
                    str(obj), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    assert result.returncode != 0 and 'overlap[i]==expected[i]' in result.stderr, result.stderr
    print('Negative control rejected: lost backward word offset corrupts the move')
