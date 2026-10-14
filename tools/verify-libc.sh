#!/usr/bin/env bash
set -euo pipefail

if [[ "$#" -ne 4 ]]; then
    printf 'usage: %s LOADER LIBC TEST OBJDUMP\n' "$0" >&2
    exit 2
fi

loader="$1"
library="$2"
test_image="$3"
objdump="$4"

for image in "$loader" "$library" "$test_image"; do
    if [[ ! -f "$image" ]]; then
        printf 'libc: missing ELF: %s\n' "$image" >&2
        exit 1
    fi
done
if ! command -v "$objdump" >/dev/null 2>&1; then
    printf 'libc: missing inspection tool: %s\n' "$objdump" >&2
    exit 1
fi

python3 "$(dirname -- "$0")/verify-elf-runtime.py" "$loader" "$library" "$test_image" libsystemc.so.1
library_symbols="$($objdump -T "$library")"
test_relocations="$($objdump -R "$test_image")"

if grep -Fq '*UND*' <<<"$library_symbols"; then
    printf 'libc: shared library has unresolved dynamic symbols\n' >&2
    exit 1
fi
for symbol in \
    __libc_native_syscall6 __libc_linux_syscall6 \
    __system_free __system_realloc __system_malloc_usable_size \
    _Z7readdirP8libc_dir _Z8closedirP8libc_dir \
    _Z6setjmpP14libc_jmp_state _Z7longjmpP14libc_jmp_statei \
    memcpy memmove memset strlen strcpy memmem mempcpy memccpy strchrnul ffs strtok_r strerror strsignal atof strtoimax strtol \
    strtoull malloc calloc realloc aligned_alloc free qsort getenv setenv \
    exit printf snprintf sscanf asprintf fopen fread fwrite fread_unlocked \
    fwrite_unlocked fseeko ftello getline getdelim fstat stat open read write readv writev \
    preadv pwritev rename gmtime_r strftime clock_gettime clock_settime settimeofday opendir readdir \
    closedir scandir mmap munmap mprotect sysconf execve execvp getopt getopt_long \
    fnmatch glob globfree basename dirname mbrtowc mbsrtowcs wcsrtombs wctype \
    setjmp longjmp poll select getaddrinfo freeaddrinfo getnameinfo gai_strerror \
    socket bind connect send accept4 sendmsg recvmsg pipe pipe2 msync madvise \
     recv inet_pton inet_ntop gethostbyname gethostbyname2 gethostbyaddr \
     gethostbyname_r gethostbyaddr_r herror hstrerror getservbyname getservbyport \
     getservbyname_r getservbyport_r getprotobyname getprotobynumber \
     getprotobyname_r getprotobynumber_r pthread_attr_setguardsize \
    pthread_attr_getguardsize pthread_create pthread_join pthread_key_create \
    pthread_rwlock_rdlock pthread_barrier_wait pthread_spin_lock \
    pthread_mutex_lock pthread_mutex_unlock pthread_cond_wait pthread_once \
    fork wait waitpid \
    openat open64 openat64 creat creat64 dup3 lseek64 pread64 pwrite64 \
    ftruncate64 truncate64 getdtablesize fstatat faccessat unlinkat mkdirat renameat fchdir \
    fdopendir readdir_r pathconf fpathconf confstr clock_getres clock_nanosleep secure_getenv getsubopt \
     vdprintf dprintf fopen64 freopen64 __libc_build_exec_fd_map \
     __libc_init_descriptors fgets_unlocked fputs_unlocked \
    fputc_unlocked fwide fgetwc fputwc fgetws fputws getwc putwc getwchar \
     putwchar feclearexcept fegetexceptflag feraiseexcept fesetexceptflag \
     fetestexcept fegetround fesetround fegetenv feholdexcept fesetenv \
     feupdateenv crealf creal creall cimagf cimag cimagl cabsf cabs cabsl \
     cargf carg cargl conjf conj conjl cprojf cproj cprojl cexpf cexp cexpl \
     clogf clog clogl cpowf cpow cpowl csqrtf csqrt csqrtl csinf csin csinl \
     ccosf ccos ccosl ctanf ctan ctanl csinhf csinh csinhl ccoshf ccosh \
     ccoshl ctanhf ctanh ctanhl casinf casin casinl cacosf cacos cacosl \
     catanf catan catanl casinhf casinh casinhl cacoshf cacosh cacoshl \
     catanhf catanh catanhl __mulsc3 __muldc3 __mulxc3 __divsc3 __divdc3 \
     __divxc3 mbrtoc16 c16rtomb mbrtoc32 c32rtomb thrd_create thrd_join \
     mtx_init mtx_lock cnd_wait call_once tss_create; do
    if ! awk -v symbol="$symbol" '$NF == symbol && $0 !~ /\*UND\*/ {found=1} END {exit !found}' <<<"$library_symbols"; then
        printf 'libc: missing exported symbol: %s\n' "$symbol" >&2
        exit 1
    fi
done
if ! grep -Fq 'JUMP_SLOT' <<<"$test_relocations"; then
    printf 'libc: test image has no dynamic libc calls\n' >&2
    exit 1
fi

printf 'libc sanity passed: freestanding CRT, PT_INTERP, libc dependency, exports, and JUMP_SLOT\n'
