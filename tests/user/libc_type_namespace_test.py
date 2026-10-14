"""Neutral public types keep their layout without retired C++ exports."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
LIBC = ROOT/'user/libc'
TYPES = {
    'include/dirent.h': ('libc_dir',),
    'include/locale.h': ('libc_locale',),
    'include/pthread.h': ('libc_pthread',),
    'include/setjmp.h': ('libc_jmp_state', 'libc_jmp_state_t'),
    'include/signal.h': ('libc_siginfo',),
    'include/sys/wait.h': ('libc_siginfo',),
    'include/libc/libc.h': ('libc_file',),
}
for name, types in TYPES.items():
    source = (LIBC/name).read_text()
    for typename in types:
        assert typename in source and typename.replace('libc_', 'liteos_') not in source, name
for name in ('pthread.c', 'posix_compat.c'):
    source = (LIBC/name).read_text()
    assert 'struct liteos_' not in source, name

headers = '''
#include <dirent.h>
#include <setjmp.h>
#include <pthread.h>
#include <locale.h>
#include <signal.h>
#include <sys/wait.h>
'''
checks = '''
_Static_assert(sizeof(FILE) == 80, "FILE layout");
_Static_assert(sizeof(DIR) == 536, "DIR layout");
_Static_assert(sizeof(jmp_buf) == 64, "jump buffer layout");
_Static_assert(sizeof(siginfo_t) == 32, "signal info layout");
_Static_assert(sizeof(pthread_t) == 8 && sizeof(locale_t) == 8, "opaque pointers");
_Static_assert(offsetof(DIR, entry) == 264, "directory entry offset");
_Static_assert(offsetof(siginfo_t, si_addr) == 24, "signal address offset");
_Static_assert(offsetof(libc_jmp_state_t, rip) == 56, "jump address offset");
void type_identity(DIR *d, FILE *f, pthread_t p, locale_t l, siginfo_t *s) {
    struct libc_dir *directory = d;
    struct libc_file *file = f;
    struct libc_pthread *thread = p;
    struct libc_locale *locale = l;
    struct libc_siginfo *signal = s;
    (void)directory; (void)file; (void)thread; (void)locale; (void)signal;
}
'''
aliases = (
    ('_Z7readdirP8libc_dir', '_Z7readdirP10liteos_dir'),
    ('_Z8closedirP8libc_dir', '_Z8closedirP10liteos_dir'),
    ('_Z6setjmpP14libc_jmp_state', '_Z6setjmpP16liteos_jmp_state'),
    ('_Z7longjmpP14libc_jmp_statei', '_Z7longjmpP16liteos_jmp_statei'),
)
with tempfile.TemporaryDirectory(prefix='system-libc-types-') as directory:
    path = Path(directory)
    include = ['-I', str(LIBC/'include'), '-I', str(ROOT/'include')]
    for language, compiler, standard in (('c', 'cc', 'gnu11'), ('cc', 'c++', 'c++17')):
        source = path/('types.'+language)
        source.write_text(headers + (checks if language == 'c' else checks.replace('_Static_assert', 'static_assert')))
        subprocess.run([compiler, '-std='+standard, '-Wall', '-Wextra', '-Werror',
                        '-ffreestanding', *include, '-c', str(source),
                        '-o', str(path/'types.o')], check=True)
    caller = path/'caller.cc'
    caller.write_text('''#include <dirent.h>
#include <setjmp.h>
void caller(DIR *directory, jmp_buf environment) {
    (void)readdir(directory);
    (void)closedir(directory);
    if (setjmp(environment)) longjmp(environment, 1);
}
''')
    subprocess.run(['c++', '-std=c++17', '-ffreestanding', *include, '-c', str(caller),
                    '-o', str(path/'caller.o')], check=True)
    undefined = subprocess.check_output(['nm', '-u', str(path/'caller.o')], text=True)
    for current, legacy in aliases:
        assert current in undefined and legacy not in undefined, (current, undefined)
    subprocess.run(['cc', '-c', str(LIBC/'posix_compat.S'), '-o', str(path/'compat.o')], check=True)
    defined = subprocess.check_output(['nm', '-g', '--defined-only', str(path/'compat.o')], text=True)
    symbols = {row.split()[2]: row.split()[0] for row in defined.splitlines()}
    for current, legacy in aliases:
        assert current in symbols and legacy not in symbols, (current, legacy)
print('Neutral libc types: C/C++ layout, forward declarations, actual compiler mangling and retired exports absent passed')
