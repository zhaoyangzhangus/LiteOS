param(
    [Parameter(Mandatory = $true)][string]$Loader,
    [Parameter(Mandatory = $true)][string]$Library,
    [Parameter(Mandatory = $true)][string]$Test,
    [Parameter(Mandatory = $true)][string]$Objdump
)

$ErrorActionPreference = 'Stop'

foreach ($image in @($Loader, $Library, $Test)) {
    if (-not (Test-Path -LiteralPath $image -PathType Leaf)) {
        throw "libc: missing ELF: $image"
    }
}
if (-not (Get-Command $Objdump -ErrorAction SilentlyContinue)) {
    throw "libc: missing inspection tool: $Objdump"
}

function Read-Objdump([string[]]$Arguments) {
    return (& $Objdump @Arguments | Out-String)
}

$loaderHeaders = Read-Objdump @('-p', $Loader)
$libraryHeaders = Read-Objdump @('-p', $Library)
$testHeaders = Read-Objdump @('-p', $Test)
$testInterpreter = Read-Objdump @('-s', '-j', '.interp', $Test)
$librarySymbols = Read-Objdump @('-T', $Library)
$testRelocations = Read-Objdump @('-R', $Test)

if ($loaderHeaders -match 'NEEDED' -or $libraryHeaders -match 'NEEDED') {
    throw 'libc: loader or libc has an external dependency'
}
if ($testHeaders -notmatch 'INTERP' -or
    $testInterpreter -notmatch '2f6c6962' -or
    $testInterpreter -notmatch '6c697465' -or
    $testInterpreter -notmatch '6f732e73' -or
    $testInterpreter -notmatch '6f2e3100') {
    throw 'libc: test image has no LiteOS interpreter'
}
if ($testHeaders -notmatch 'NEEDED' -or $testHeaders -notmatch 'libliteosc\.so\.1') {
    throw 'libc: test image has no libc dependency'
}
if ($librarySymbols -match '\*UND\*') {
    throw 'libc: shared library has unresolved dynamic symbols'
}
foreach ($symbol in @(
        'memcpy', 'memmove', 'memset', 'strlen', 'strcpy', 'memmem', 'mempcpy',
        'memccpy', 'strchrnul',
        'ffs', 'strtok_r',
        'strerror', 'strsignal', 'atof', 'strtoimax', 'strtol', 'strtoull', 'malloc', 'calloc',
        'realloc', 'aligned_alloc', 'free', 'qsort', 'getenv', 'setenv',
        'exit', 'printf', 'snprintf', 'sscanf', 'asprintf', 'fopen',
        'fread', 'fwrite', 'fread_unlocked', 'fwrite_unlocked', 'fseeko', 'ftello', 'getline',
        'getdelim', 'fstat', 'stat', 'open', 'read', 'write', 'readv',
        'writev', 'preadv', 'pwritev', 'rename', 'gmtime_r', 'strftime',
        'clock_gettime', 'clock_settime', 'settimeofday', 'opendir', 'readdir', 'closedir', 'scandir',
        'mmap', 'munmap', 'mprotect', 'sysconf', 'execve', 'execvp',
        'getopt', 'getopt_long', 'fnmatch', 'glob', 'globfree', 'basename', 'dirname', 'mbrtowc',
        'mbsrtowcs', 'wcsrtombs', 'wctype',
        'setjmp', 'longjmp', 'poll', 'select', 'getaddrinfo', 'freeaddrinfo',
        'getnameinfo', 'gai_strerror', 'socket', 'bind', 'connect', 'send', 'recv',
        'accept4', 'sendmsg', 'recvmsg', 'pipe', 'pipe2',
        'openat', 'open64', 'openat64', 'creat', 'creat64', 'dup3', 'lseek64',
        'pread64', 'pwrite64', 'ftruncate64', 'truncate64', 'getdtablesize', 'fstatat', 'faccessat', 'unlinkat', 'mkdirat',
        'renameat', 'fchdir', 'fdopendir', 'readdir_r',
        'pathconf', 'fpathconf', 'confstr',
        'clock_getres', 'clock_nanosleep', 'secure_getenv', 'getsubopt',
        'vdprintf', 'dprintf', 'fopen64', 'freopen64',
        '__libc_build_exec_fd_map', '__libc_init_descriptors',
        'fgets_unlocked', 'fputs_unlocked', 'fputc_unlocked',
        'fwide', 'fgetwc', 'fputwc', 'fgetws', 'fputws', 'getwc', 'putwc',
        'getwchar', 'putwchar',
        'inet_pton', 'inet_ntop', 'pthread_attr_setguardsize',
        'pthread_attr_getguardsize', 'pthread_create', 'pthread_join',
        'gethostbyname', 'gethostbyname2', 'gethostbyaddr', 'gethostbyname_r',
        'gethostbyaddr_r', 'herror', 'hstrerror', 'getservbyname', 'getservbyport',
        'getservbyname_r', 'getservbyport_r', 'getprotobyname',
        'getprotobynumber', 'getprotobyname_r', 'getprotobynumber_r',
        'pthread_key_create', 'pthread_rwlock_rdlock',
        'pthread_barrier_wait', 'pthread_spin_lock', 'pthread_mutex_lock',
        'pthread_mutex_unlock', 'pthread_cond_wait', 'pthread_once',
        'fork', 'wait', 'waitpid',
        'feclearexcept', 'fegetexceptflag', 'feraiseexcept', 'fesetexceptflag',
        'fetestexcept', 'fegetround', 'fesetround', 'fegetenv', 'feholdexcept',
        'fesetenv', 'feupdateenv',
        'crealf', 'creal', 'creall', 'cimagf', 'cimag', 'cimagl',
        'cabsf', 'cabs', 'cabsl', 'cargf', 'carg', 'cargl',
        'conjf', 'conj', 'conjl', 'cprojf', 'cproj', 'cprojl',
        'cexpf', 'cexp', 'cexpl', 'clogf', 'clog', 'clogl',
        'cpowf', 'cpow', 'cpowl', 'csqrtf', 'csqrt', 'csqrtl',
        'csinf', 'csin', 'csinl', 'ccosf', 'ccos', 'ccosl',
        'ctanf', 'ctan', 'ctanl', 'csinhf', 'csinh', 'csinhl',
        'ccoshf', 'ccosh', 'ccoshl', 'ctanhf', 'ctanh', 'ctanhl',
        'casinf', 'casin', 'casinl', 'cacosf', 'cacos', 'cacosl',
        'catanf', 'catan', 'catanl', 'casinhf', 'casinh', 'casinhl',
        'cacoshf', 'cacosh', 'cacoshl', 'catanhf', 'catanh', 'catanhl',
        '__mulsc3', '__muldc3', '__mulxc3', '__divsc3', '__divdc3', '__divxc3',
        'mbrtoc16', 'c16rtomb', 'mbrtoc32', 'c32rtomb', 'thrd_create', 'thrd_join',
        'mtx_init', 'mtx_lock', 'cnd_wait', 'call_once', 'tss_create')) {
    if ($librarySymbols -notmatch [regex]::Escape($symbol)) {
        throw "libc: missing exported symbol: $symbol"
    }
}
if ($testRelocations -notmatch 'JUMP_SLOT') {
    throw 'libc: test image has no dynamic libc calls'
}

Write-Output 'libc sanity passed: freestanding CRT, PT_INTERP, libc dependency, exports, and JUMP_SLOT'
