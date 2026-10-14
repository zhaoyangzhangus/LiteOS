"""ISO scanner names must preserve the exact native implementation and code."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT/'user/libc/stdio.c').read_text()
baseline = re.sub(r'^extern __typeof__\([^\n]+__isoc99_[^\n]+\n', '', source, flags=re.M)
with tempfile.TemporaryDirectory(prefix='scan-exports-') as directory:
    base = Path(directory)
    flags = ['cc', '-std=gnu11', '-O2', '-ffreestanding', '-fno-builtin',
             '-I'+str(ROOT/'user/libc/include'), '-I'+str(ROOT/'include')]
    for name, text in (('before', baseline), ('after', source)):
        c, obj = base/(name+'.c'), base/(name+'.o')
        c.write_text(text)
        subprocess.run([*flags, '-c', str(c), '-o', str(obj)], check=True)
        subprocess.run(['objcopy', '-O', 'binary', '--only-section=.text',
                        str(obj), str(base/(name+'.text'))], check=True)
    assert (base/'before.text').read_bytes() == (base/'after.text').read_bytes()
    defined = subprocess.check_output(['nm', '-g', '--defined-only', str(base/'after.o')], text=True)
    symbols = {row.split()[2]: row.split()[0] for row in defined.splitlines()}
    for name in ('sscanf', 'vsscanf', 'fscanf', 'vfscanf', 'scanf', 'vscanf'):
        assert symbols[name] == symbols['__isoc99_'+name], name
print('Native ISO scanner exports: six same-address entry points, unchanged full scanner '
      'machine instructions; glibc layout/version compatibility not asserted')
