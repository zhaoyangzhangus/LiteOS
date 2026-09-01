#include <stdint.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <complex.h>
#include <dirent.h>
#include <fenv.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <getopt.h>
#include <glob.h>
#include <inttypes.h>
#include <limits.h>
#include <liteos/libc.h>
#include <signal.h>
#include <locale.h>
#include <math.h>
#include <netdb.h>
#include <pthread.h>
#include <poll.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <tgmath.h>
#include <threads.h>
#include <uchar.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include <blend2d/blend2d.h>

#include "liteos_gfx.h"
#include "blend2d_api_test.h"
#include "liteos_text.h"

static int libc_test_fail(const char *stage) {
    printf("LITEOS_LIBC_TEST_FAIL stage=%s errno=%d\n", stage, errno);
    return 1;
}

static int libc_test_detail_fail(const char *stage) {
    printf("LITEOS_LIBC_TEST_DETAIL_FAIL stage=%s errno=%d\n", stage, errno);
    return 0;
}

static int test_blend2d(void) {
    enum { width = 64, height = 64 };
    static uint32_t pixels[width * height];
    uint32_t gradient_top;
    uint32_t gradient_bottom;
    bool runtime_ready = false;
    bool success = false;

    if (bl_runtime_init() != BL_SUCCESS) return 0;
    runtime_ready = true;

    liteos_gfx_clear(pixels, width, width, height, 0x00112233U);
    if (pixels[0] != 0xFF112233U) goto cleanup;

    liteos_gfx_fill_rect(pixels, width, width, height, 4, 4, 8, 8,
                         0x0000FF00U);
    if (pixels[4U + 4U * width] != 0xFF00FF00U ||
        pixels[3U + 3U * width] != 0xFF112233U) goto cleanup;

    liteos_gfx_gradient_rect(pixels, width, width, height, 16, 16, 16, 16,
                             0x000000FFU, 0x00FF0000U);
    gradient_top = pixels[16U + 16U * width];
    gradient_bottom = pixels[16U + 31U * width];
    if ((gradient_top & 0x0000FF00U) != 0U ||
        gradient_top == 0xFF112233U || gradient_top == gradient_bottom ||
        gradient_bottom != 0xFFFF0000U) goto cleanup;

    liteos_gfx_frame(pixels, width, width, height, 2U, 0x00FFFFFFU);
    success = pixels[0] == 0xFFFFFFFFU &&
              pixels[1U + width] == 0xFFFFFFFFU &&
              pixels[2U + 2U * width] == 0xFF112233U;

cleanup:
    if (runtime_ready) (void)bl_runtime_shutdown();
    return success;
}

static int test_blend2d_font_zoom(void) {
    uint32_t base_size;
    uint32_t base_width;
    uint32_t base_height;
    int success = 0;

    if (!liteos_text_init(LITEOS_TEXT_DEFAULT_SIZE)) return 0;
    base_size = liteos_text_size();
    base_width = liteos_text_measure("MMMM");
    base_height = liteos_text_line_height();
    if (base_size != LITEOS_TEXT_DEFAULT_SIZE || base_width == 0U ||
        base_height == 0U || !liteos_text_adjust(1) ||
        liteos_text_size() != base_size + LITEOS_TEXT_SIZE_STEP ||
        liteos_text_measure("MMMM") <= base_width ||
        liteos_text_line_height() <= base_height) {
        goto done;
    }
    if (!liteos_text_adjust(-1) || liteos_text_size() != base_size) goto done;
    success = 1;

done:
    liteos_text_shutdown();
    return success;
}

static volatile sig_atomic_t g_signal_hits;

static void signal_test_handler(int signal_number) {
    if (signal_number == SIGUSR1) ++g_signal_hits;
}

static int test_signals(void) {
    struct sigaction action = {0};
    struct sigaction previous = {0};
    sigset_t mask;
    int wait_status = 0;
    action.sa_handler = signal_test_handler;
    if (strcmp(strsignal(SIGUSR1), "User defined signal 1") != 0 ||
        strcmp(strsignal(SIGTERM), "Terminated") != 0 ||
        strcmp(strsignal(0), "Unknown signal 0") != 0) return 0;
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGUSR1, &action, &previous) != 0 ||
        previous.sa_handler != SIG_DFL) return 0;
    if (sigemptyset(&mask) != 0 || sigaddset(&mask, SIGUSR1) != 0 ||
        sigismember(&mask, SIGUSR1) != 1 ||
        sigprocmask(SIG_BLOCK, &mask, 0) != 0 ||
        raise(SIGUSR1) != 0 || g_signal_hits != 0 ||
        sigprocmask(SIG_UNBLOCK, &mask, 0) != 0 || g_signal_hits != 1) {
        return 0;
    }
    if (kill((pid_t)getpid(), 0) != 0 || raise(SIGUSR1) != 0 ||
        g_signal_hits != 2 || signal(SIGUSR1, SIG_IGN) != signal_test_handler ||
        raise(SIGUSR1) != 0 || g_signal_hits != 2) return 0;
    if (signal(SIGUSR1, SIG_DFL) != SIG_IGN) return 0;
    pid_t child = fork();
    if (child < 0) return 0;
    if (child == 0) {
        (void)raise(SIGTERM);
        _exit(99);
    }
    return waitpid(child, &wait_status, 0) == child &&
           WIFSIGNALED(wait_status) && WTERMSIG(wait_status) == SIGTERM;
}

static int integer_compare(const void *left, const void *right) {
    int a = *(const int *)left;
    int b = *(const int *)right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static int test_strings(void) {
    char text[64] = "liteos";
    char separator_text[] = "abc:def";
    char tokens[32] = "one,two,,three";
    char basename_path[] = "/tmp/image.png";
    char dirname_path[] = "/tmp/image.png";
    char copied[8] = {0};
    char stopped[8] = {0};
    char *state = 0;
    char *token;
    char error_text[64];

    strcat(text, "-runtime");
    strncat(text, "-ignored", 3U);
    if (strcmp(text, "liteos-runtime-ig") != 0 ||
        memrchr(text, 'i', strlen(text)) != text + 15U ||
        strspn("123abc", "0123456789") != 3U ||
        strcspn(separator_text, ":") != 3U ||
        strpbrk(separator_text, ":;") != separator_text + 3U) {
        return 0;
    }
    if (strcasecmp("LiTeOs", "liteos") != 0 ||
        strncasecmp("runtime", "RUNTimes", 7U) != 0 ||
        strcmp(strerror(ENOENT), "No such file or directory") != 0 ||
        strerror_r(EINVAL, error_text, sizeof(error_text)) != 0 ||
        strcmp(error_text, "Invalid argument") != 0) {
        return 0;
    }
    token = strtok_r(tokens, ",", &state);
    if (token == 0 || strcmp(token, "one") != 0) {
        return 0;
    }
    token = strtok_r(0, ",", &state);
    if (token == 0 || strcmp(token, "two") != 0) {
        return 0;
    }
    token = strtok_r(0, ",", &state);
    if (token == 0 || strcmp(token, "three") != 0 || strtok_r(0, ",", &state) != 0) {
        return 0;
    }
    if (ffs(0x10) != 5 || ffsl(0x100L) != 9 || ffsll(0x1000LL) != 13 ||
        index(text, '-') != text + 6 || rindex(text, 'e') != text + 13 ||
        memmem("abcdef", 6U, "cd", 2U) != (const void *)"abcdef" + 2U ||
        mempcpy(copied, "copy", 5U) != copied + 5U ||
        memccpy(stopped, "abcdef", 'c', sizeof(stopped)) != stopped + 3U ||
        strcmp(stopped, "abc") != 0 ||
        strchrnul(text, '!') != text + strlen(text) ||
        strcmp(copied, "copy") != 0 || strcmp(basename(basename_path), "image.png") != 0 ||
        strcmp(dirname(dirname_path), "/tmp") != 0) {
        return 0;
    }
    if (!isalpha('A') || !isdigit('7') || !isspace('\n') ||
        toupper('a') != 'A' || tolower('Z') != 'z') {
        return 0;
    }
    return 1;
}

static int test_standard_utilities(void) {
    char *end = 0;
    char *duplicate;
    char *bounded;
    void *aligned = 0;
    int values[] = {5, 1, 4, 2, 3};
    int key = 4;
    intmax_t signed_value;
    uintmax_t unsigned_value;
    div_t quotient;
    char suboptions[] = "mode=fast,verbose";
    char *suboption_cursor = suboptions;
    char *suboption_value = 0;
    char *suboption_tokens[] = {(char *)"mode", (char *)"verbose", 0};

    signed_value = strtoimax("-42tail", &end, 10);
    if (signed_value != -42) {
        return 0;
    }
    if (end == 0) {
        return 0;
    }
    if (end[0] != 't' || end[1] != 'a' || end[2] != 'i' ||
        end[3] != 'l' || end[4] != '\0') {
        return 0;
    }
    if (atof(" -12.5tail") != -12.5 || atof("0x1.8p+2") != 6.0) {
        return 0;
    }
    unsigned_value = strtoumax("ff", &end, 16);
    if (unsigned_value != 255U || end == 0 || *end != '\0') return 0;
    errno = 0;
    if (strtol("999999999999999999999999", &end, 10) != LONG_MAX ||
        errno != ERANGE) return 0;
    errno = 0;
    if (strtoul("-18446744073709551616", &end, 10) != ULONG_MAX ||
        errno != ERANGE || end == 0 || *end != '\0') return 0;
    errno = 0;
    if (strtoll("9223372036854775808", &end, 10) != LLONG_MAX ||
        errno != ERANGE || end == 0 || *end != '\0') return 0;
    quotient = div(17, 5);
    if (quotient.quot != 3 || quotient.rem != 2 || abs(-7) != 7 ||
        abs(INT_MIN) != INT_MIN || labs(LONG_MIN) != LONG_MIN ||
        llabs(LLONG_MIN) != LLONG_MIN) return 0;

    duplicate = strdup("duplicate");
    bounded = strndup("bounded-value", 7U);
    if (duplicate == 0 || bounded == 0 || strcmp(duplicate, "duplicate") != 0 ||
        strcmp(bounded, "bounded") != 0) {
        free(duplicate);
        free(bounded);
        return 0;
    }
    free(duplicate);
    free(bounded);
    if (posix_memalign(&aligned, 64U, 128U) != 0 ||
        ((uintptr_t)aligned & 63U) != 0U ||
        reallocarray(0, SIZE_MAX, 2U) != 0 || errno != EOVERFLOW) {
        free(aligned);
        return 0;
    }
    free(aligned);
    qsort(values, sizeof(values) / sizeof(values[0]), sizeof(values[0]),
          integer_compare);
    if (values[0] != 1 || values[4] != 5 ||
        bsearch(&key, values, sizeof(values) / sizeof(values[0]),
                sizeof(values[0]), integer_compare) == 0 ||
        getsubopt(&suboption_cursor, suboption_tokens, &suboption_value) != 0 ||
        suboption_value == 0 || strcmp(suboption_value, "fast") != 0 ||
        getsubopt(&suboption_cursor, suboption_tokens, &suboption_value) != 1 ||
        suboption_value != 0) return 0;
    return 1;
}

static int test_entropy(void) {
    uint8_t first[64];
    uint8_t second[64];
    if (getentropy(first, sizeof(first)) != 0 ||
        getentropy(second, sizeof(second)) != 0) return 0;
    return memcmp(first, second, sizeof(first)) != 0;
}

static int test_environment_and_time(void) {
    char assignment[] = "LITEOS_LIBC_TEST=putenv";
    char formatted[64];
    char cwd[PATH_MAX];
    char configured_path[16];
    struct lconv *conventions;
    struct timeval current;
    struct timespec current_spec;
    struct timespec resolution;
    struct timespec past = {0, 0};
    struct timespec realtime_before;
    struct timespec monotonic_before;
    struct timespec monotonic_after;
    struct timespec adjusted_realtime = {1234567890, 123456789};
    struct tm broken_down;
    struct tm date = {0};
    struct tm normalized = {0};
    time_t epoch = 0;
    int clock_ok = 1;
    int realtime_saved = 0;

    if (setenv("LITEOS_LIBC_TEST", "setenv", 1) != 0 ||
        strcmp(getenv("LITEOS_LIBC_TEST"), "setenv") != 0 ||
        strcmp(secure_getenv("LITEOS_LIBC_TEST"), "setenv") != 0 ||
        setenv("LITEOS_LIBC_TEST", "ignored", 0) != 0 ||
        strcmp(getenv("LITEOS_LIBC_TEST"), "setenv") != 0 ||
        putenv(assignment) != 0 ||
        strcmp(getenv("LITEOS_LIBC_TEST"), "putenv") != 0 ||
        unsetenv("LITEOS_LIBC_TEST") != 0 || getenv("LITEOS_LIBC_TEST") != 0) {
        return 0;
    }
    if (gmtime_r(&epoch, &broken_down) == 0 || broken_down.tm_year != 70 ||
        broken_down.tm_mon != 0 || broken_down.tm_mday != 1 ||
        broken_down.tm_wday != 4 || strftime(formatted, sizeof(formatted),
                                             "%Y-%m-%d %H:%M", &broken_down) != 16U ||
        strcmp(formatted, "1970-01-01 00:00") != 0) return 0;
    date.tm_year = 70;
    date.tm_mon = 0;
    date.tm_mday = 2;
    if (mktime(&date) != 86400 || date.tm_yday != 1) return 0;
    normalized.tm_year = 70;
    normalized.tm_mon = 12;
    normalized.tm_mday = 1;
    normalized.tm_hour = 24;
    normalized.tm_min = 0;
    normalized.tm_sec = 0;
    if (mktime(&normalized) != 31622400 ||
        normalized.tm_year != 71 || normalized.tm_mon != 0 ||
        normalized.tm_mday != 2 || normalized.tm_hour != 0 ||
        normalized.tm_min != 0 || normalized.tm_sec != 0) {
        return 0;
    }
    normalized.tm_year = 70;
    normalized.tm_mon = 0;
    normalized.tm_mday = 1;
    normalized.tm_hour = 0;
    normalized.tm_min = 60;
    normalized.tm_sec = 60;
    if (mktime(&normalized) != 3660 || normalized.tm_hour != 1 ||
        normalized.tm_min != 1 || normalized.tm_sec != 0) return 0;
    normalized.tm_year = 70;
    normalized.tm_mon = -1;
    normalized.tm_mday = 1;
    normalized.tm_hour = 0;
    normalized.tm_min = 0;
    normalized.tm_sec = 0;
    if (mktime(&normalized) != -2678400 || normalized.tm_year != 69 ||
        normalized.tm_mon != 11 || normalized.tm_mday != 1 ||
        clock_gettime(CLOCK_REALTIME, &realtime_before) != 0 ||
        clock_gettime(CLOCK_MONOTONIC, &monotonic_before) != 0) return 0;
    realtime_saved = 1;
    errno = 0;
    if (clock_getres(CLOCK_MONOTONIC, &resolution) != 0 ||
        resolution.tv_sec != 0 || resolution.tv_nsec != 1 ||
        clock_settime(CLOCK_MONOTONIC, &adjusted_realtime) != -1 ||
        errno != EINVAL ||
        clock_settime(CLOCK_REALTIME, &adjusted_realtime) != 0 ||
        gettimeofday(&current, 0) != 0 ||
        current.tv_sec != adjusted_realtime.tv_sec ||
        current.tv_usec < adjusted_realtime.tv_nsec / 1000L ||
        clock_gettime(CLOCK_MONOTONIC, &monotonic_after) != 0 ||
        monotonic_after.tv_sec < monotonic_before.tv_sec ||
        (monotonic_after.tv_sec == monotonic_before.tv_sec &&
         monotonic_after.tv_nsec < monotonic_before.tv_nsec) ||
        clock_getres(CLOCK_REALTIME, &resolution) != 0 ||
        resolution.tv_sec != 0 || resolution.tv_nsec != 1 ||
        clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &past, 0) != 0 ||
        timespec_get(&current_spec, TIME_UTC) != TIME_UTC) {
        clock_ok = 0;
    }
    if (realtime_saved && clock_settime(CLOCK_REALTIME, &realtime_before) != 0) {
        clock_ok = 0;
    }
    if (!clock_ok) return 0;
    conventions = localeconv();
    return conventions != 0 && strcmp(conventions->decimal_point, ".") == 0 &&
           confstr(_CS_PATH, configured_path, sizeof(configured_path)) == 10U &&
           strcmp(configured_path, "/sbin:/bin") == 0 &&
           pathconf("/", _PC_PATH_MAX) == 255L && getdtablesize() == 256 &&
           getcwd(cwd, sizeof(cwd)) != 0 && strcmp(cwd, "/") == 0;
}

static int test_virtual_memory(void) {
    unsigned char *mapping;
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size != 4096U || getpagesize() != 4096) return 0;
    mapping = (unsigned char *)mmap(0, page_size + 1U,
                                    PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return 0;
    mapping[0] = 0x5A;
    mapping[page_size] = 0xA5;
    int msync_result = msync(mapping, page_size + 1U, MS_SYNC);
    int normal_result = madvise(mapping, page_size + 1U, MADV_NORMAL);
    int dontneed_result = madvise(mapping, page_size + 1U, MADV_DONTNEED);
    if (mapping[0] != 0x5A || mapping[page_size] != 0xA5 ||
        msync_result != 0 || normal_result != 0 || dontneed_result != 0) {
        (void)munmap(mapping, page_size + 1U);
        return 0;
    }
    errno = 0;
    msync_result = msync((void *)((uintptr_t)mapping + 1U), page_size,
                         MS_ASYNC);
    if (msync_result != -1 || errno != EINVAL) {
        (void)munmap(mapping, page_size + 1U);
        return 0;
    }
    errno = 0;
    int invalid_madvise = madvise(mapping, page_size, MADV_DONTNEED + 1);
    if (invalid_madvise != -1 || errno != EINVAL) {
        (void)munmap(mapping, page_size + 1U);
        return 0;
    }
    int protect_result = mprotect(mapping, page_size + 1U, PROT_READ);
    if (protect_result != 0) {
        (void)munmap(mapping, page_size + 1U);
        return 0;
    }
    if (mapping[0] != 0x5A || munmap(mapping, page_size + 1U) != 0) {
        (void)munmap(mapping, page_size + 1U);
        return 0;
    }
    errno = EIO;
    mapping = (unsigned char *)mmap(0, 1U, PROT_READ, MAP_PRIVATE, -1, 0);
    if (mapping != MAP_FAILED || errno != EBADF) return 0;

    {
        char template_name[] = "/tmp/libc-msync-XXXXXX";
        char expected[] = "sync";
        char observed[sizeof(expected)] = {0};
        unsigned char *file_mapping = MAP_FAILED;
        int descriptor = -1;
        bool success = false;
        descriptor = mkstemp(template_name);
        if (descriptor >= 0 &&
            write(descriptor, expected, sizeof(expected) - 1U) ==
                (ssize_t)(sizeof(expected) - 1U) &&
            lseek(descriptor, 0, SEEK_SET) == 0) {
            file_mapping = (unsigned char *)mmap(
                0, page_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                descriptor, 0);
            if (file_mapping != MAP_FAILED && file_mapping[0] == 's') {
                file_mapping[0] = 'S';
                success = msync(file_mapping, page_size, MS_SYNC) == 0 &&
                          munmap(file_mapping, page_size) == 0;
                file_mapping = MAP_FAILED;
            }
        }
        if (success && lseek(descriptor, 0, SEEK_SET) == 0 &&
            read(descriptor, observed, sizeof(expected) - 1U) ==
                (ssize_t)(sizeof(expected) - 1U) && observed[0] == 'S') {
            success = true;
        } else {
            success = false;
        }
        if (file_mapping != MAP_FAILED) (void)munmap(file_mapping, page_size);
        if (descriptor >= 0) (void)close(descriptor);
        (void)unlink(template_name);
        if (!success) return 0;
    }
    return 1;
}

static int test_network_addresses(void) {
    struct in_addr address;
    struct in_addr round_trip;
    struct in6_addr address6;
    struct in6_addr round_trip6;
    char text[INET_ADDRSTRLEN];
    char text6[INET6_ADDRSTRLEN];
    if (htons(0x1234U) != 0x3412U || ntohs(0x3412U) != 0x1234U ||
        htonl(0x12345678U) != 0x78563412U ||
        inet_pton(AF_INET, "127.0.0.1", &address) != 1 ||
        ntohl(address.s_addr) != 0x7F000001U ||
        inet_ntop(AF_INET, &address, text, sizeof(text)) == 0 ||
        strcmp(text, "127.0.0.1") != 0 ||
        inet_aton(text, &round_trip) != 1 ||
        round_trip.s_addr != address.s_addr) return 0;
    if (inet_pton(AF_INET6, "2001:db8::1", &address6) != 1 ||
        inet_ntop(AF_INET6, &address6, text6, sizeof(text6)) == 0 ||
        strcmp(text6, "2001:db8::1") != 0 ||
        inet_pton(AF_INET6, text6, &round_trip6) != 1 ||
        memcmp(address6.s6_addr, round_trip6.s6_addr, 16U) != 0 ||
        inet_pton(AF_INET6, "::ffff:192.0.2.1", &round_trip6) != 1 ||
        inet_pton(AF_INET, "127.0.0", &round_trip) != 0) return 0;
    memset(&address6, 0, sizeof(address6));
    if (inet_ntop(AF_INET6, &address6, text6, sizeof(text6)) == 0 ||
        strcmp(text6, "::") != 0 ||
        inet_pton(AF_INET6, "1:0:0:2:0:0:3:4", &address6) != 1 ||
        inet_ntop(AF_INET6, &address6, text6, sizeof(text6)) == 0 ||
        strcmp(text6, "1::2:0:0:3:4") != 0) return 0;
    errno = 0;
    if (inet_ntop(AF_INET6, &address6, text6, 5U) != 0 || errno != ENOSPC) {
        return 0;
    }
    return 1;
}

static int test_poll_and_name_resolution(void) {
    struct pollfd event;
    struct addrinfo hints = {0};
    struct addrinfo *result = 0;
    struct hostent *legacy_host;
    struct hostent legacy_host_storage;
    struct hostent *legacy_host_result = 0;
    struct hostent *legacy_reverse;
    struct servent *service_entry;
    struct protoent *protocol_entry;
    char legacy_host_buffer[512];
    int legacy_host_error = 0;
    struct sockaddr_in *address;
    struct iovec message_vector;
    struct msghdr message;
    struct timeval timeout = {0, 0};
    fd_set read_set;
    char host[INET6_ADDRSTRLEN];
    char service[16];
    char message_byte;
    int descriptor;
    int ready;
    char packet;
    int socket_option = 1;
    socklen_t socket_option_length = sizeof(socket_option);
    struct sockaddr_in socket_address = {0};
    socklen_t socket_address_length = sizeof(socket_address);

    descriptor = open("/etc/vfsmap.tst", O_RDONLY);
    if (descriptor < 0) return 0;
    event.fd = descriptor;
    event.events = POLLIN;
    event.revents = 0;
    ready = poll(&event, 1U, 0);
    if (ready != 1 || (event.revents & POLLIN) == 0) {
        (void)close(descriptor);
        return 0;
    }
    FD_ZERO(&read_set);
    FD_SET(descriptor, &read_set);
    ready = select(descriptor + 1, &read_set, 0, 0, &timeout);
    if (ready != 1 || !FD_ISSET(descriptor, &read_set) || close(descriptor) != 0) {
        (void)close(descriptor);
        return 0;
    }

    descriptor = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (descriptor < 0 || (fcntl(descriptor, F_GETFL) & O_NONBLOCK) == 0 ||
        (fcntl(descriptor, F_GETFD) & FD_CLOEXEC) == 0) {
        if (descriptor >= 0) (void)close(descriptor);
        return 0;
    }
    if (setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &socket_option,
                   sizeof(socket_option)) != 0 ||
        getsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &socket_option,
                   &socket_option_length) != 0 || socket_option != 1 ||
        socket_option_length != sizeof(socket_option) ||
        getsockname(descriptor, (struct sockaddr *)&socket_address,
                    &socket_address_length) != 0 ||
        socket_address_length != sizeof(socket_address) ||
        socket_address.sin_family != AF_INET ||
        socket_address.sin_port != 0U) {
        (void)close(descriptor);
        return 0;
    }
    errno = 0;
    if (recv(descriptor, &packet, sizeof(packet), 0) != -1 || errno != EAGAIN) {
        (void)close(descriptor);
        return 0;
    }
    errno = 0;
    if (recv(descriptor, &packet, sizeof(packet), MSG_DONTWAIT) != -1 ||
        errno != EAGAIN) {
        (void)close(descriptor);
        return 0;
    }
    message_byte = 0;
    message_vector.iov_base = &message_byte;
    message_vector.iov_len = sizeof(message_byte);
    memset(&message, 0, sizeof(message));
    message.msg_iov = &message_vector;
    message.msg_iovlen = 1U;
    errno = 0;
    if (recvmsg(descriptor, &message, 0) != -1 || errno != EAGAIN ||
        close(descriptor) != 0) {
        (void)close(descriptor);
        return 0;
    }

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_CANONNAME | AI_NUMERICHOST | AI_NUMERICSERV;
    if (getaddrinfo("127.0.0.1", "80", &hints, &result) != 0 ||
        result == 0 || result->ai_family != AF_INET ||
        result->ai_socktype != SOCK_STREAM || result->ai_addrlen != sizeof(*address) ||
        result->ai_canonname == 0 || strcmp(result->ai_canonname, "127.0.0.1") != 0) {
        freeaddrinfo(result);
        return 0;
    }
    address = (struct sockaddr_in *)result->ai_addr;
    if (ntohs(address->sin_port) != 80U ||
        getnameinfo(result->ai_addr, result->ai_addrlen, host, sizeof(host),
                    service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) != 0 ||
        strcmp(host, "127.0.0.1") != 0 || strcmp(service, "80") != 0) {
        freeaddrinfo(result);
        return 0;
    }
    freeaddrinfo(result);

    legacy_host = gethostbyname("localhost");
    if (legacy_host == 0 || legacy_host->h_addrtype != AF_INET ||
        legacy_host->h_length != (int)sizeof(struct in_addr) ||
        legacy_host->h_addr == 0 ||
        ntohl(((const struct in_addr *)legacy_host->h_addr)->s_addr) !=
            0x7F000001U) return 0;
    if (gethostbyname_r("localhost", &legacy_host_storage,
                        legacy_host_buffer, sizeof(legacy_host_buffer),
                        &legacy_host_result, &legacy_host_error) != 0 ||
        legacy_host_result != &legacy_host_storage || legacy_host_error != 0 ||
        legacy_host_storage.h_addr == 0 ||
        ntohl(((const struct in_addr *)legacy_host_storage.h_addr)->s_addr) !=
            0x7F000001U) return 0;
    legacy_reverse = gethostbyaddr(legacy_host->h_addr,
                                   (socklen_t)legacy_host->h_length, AF_INET);
    if (legacy_reverse == 0 ||
        strcmp(legacy_reverse->h_name, "localhost") != 0) return 0;
    {
        char tiny_buffer[8];
        legacy_host_error = 0;
        if (gethostbyname_r("localhost", &legacy_host_storage,
                            tiny_buffer, sizeof(tiny_buffer),
                            &legacy_host_result, &legacy_host_error) != ERANGE ||
            legacy_host_result != 0 || legacy_host_error != ERANGE) return 0;
    }
    service_entry = getservbyname("http", "tcp");
    protocol_entry = getprotobyname("tcp");
    if (service_entry == 0 || service_entry->s_port != htons(80U) ||
        protocol_entry == 0 || protocol_entry->p_proto != IPPROTO_TCP) return 0;
    return 1;
}

static int test_command_line_and_glob(void) {
    char *arguments[] = {(char *)"test", (char *)"--output",
                         (char *)"value", (char *)"--verbose", 0};
    const struct option options[] = {
        {"output", required_argument, 0, 'o'},
        {"verbose", no_argument, 0, 'v'},
        {0, 0, 0, 0},
    };
    glob_t matches = {0};
    int long_index = -1;
    int option;

    optind = 0;
    option = getopt_long(4, arguments, "", options, &long_index);
    if (option != 'o' || long_index != 0 || optarg == 0 ||
        strcmp(optarg, "value") != 0 ||
        getopt_long(4, arguments, "", options, &long_index) != 'v' ||
        getopt_long(4, arguments, "", options, &long_index) != -1) {
        return libc_test_detail_fail("command-line-options");
    }
    if (glob("/etc/*.tst", 0, 0, &matches) != 0 || matches.gl_pathc != 1U ||
        matches.gl_pathv == 0 || strcmp(matches.gl_pathv[0], "/etc/vfsmap.tst") != 0) {
        globfree(&matches);
        return libc_test_detail_fail("command-line-glob");
    }
    globfree(&matches);
    if (glob("/etc/no-such-*.tst", GLOB_NOCHECK, 0, &matches) != 0 ||
        matches.gl_pathc != 1U || strcmp(matches.gl_pathv[0],
                                         "/etc/no-such-*.tst") != 0) {
        globfree(&matches);
        return libc_test_detail_fail("command-line-nocheck");
    }
    globfree(&matches);
    return 1;
}

static pthread_mutex_t g_thread_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_thread_condition = PTHREAD_COND_INITIALIZER;
static pthread_once_t g_thread_once = PTHREAD_ONCE_INIT;
static int g_thread_once_calls;
static int g_thread_value;
static pthread_key_t g_thread_key;
static int g_thread_destructor_calls;
static int g_thread_errno;
static double g_thread_fp_results[2];
static jmp_buf g_jump_environment;

static pthread_mutex_t g_key_reuse_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_key_reuse_condition = PTHREAD_COND_INITIALIZER;
static volatile int g_key_reuse_ready;
static volatile int g_key_reuse_release;
static volatile int g_key_reuse_failed;
static volatile int g_key_reuse_destructor_calls;

static void thread_key_destructor(void *value) {
    if (value != 0) ++g_thread_destructor_calls;
}

static void trigger_longjmp(void) {
    longjmp(g_jump_environment, 7);
}

static void initialize_thread_test(void) {
    ++g_thread_once_calls;
}

static void *thread_test_entry(void *argument) {
    if (pthread_self() == 0 || pthread_once(&g_thread_once,
                                            initialize_thread_test) != 0 ||
        pthread_setspecific(g_thread_key, argument) != 0 ||
        pthread_mutex_lock(&g_thread_mutex) != 0) {
        return (void *)0xBAD1;
    }
    g_thread_value = (int)(uintptr_t)argument;
    errno = EBUSY;
    g_thread_errno = errno;
    (void)pthread_cond_signal(&g_thread_condition);
    (void)pthread_mutex_unlock(&g_thread_mutex);
    return argument;
}

static void key_reuse_destructor(void *value) {
    if (value != 0) ++g_key_reuse_destructor_calls;
}

static void *key_reuse_entry(void *argument) {
    pthread_key_t key = (pthread_key_t)(uintptr_t)argument;
    if (pthread_setspecific(key, (void *)0xCAFEU) != 0) {
        g_key_reuse_failed = 1;
        /* Wake a parent that is waiting for the worker to publish state. */
        g_key_reuse_ready = 1;
        (void)pthread_cond_signal(&g_key_reuse_condition);
        return 0;
    }
    if (pthread_mutex_lock(&g_key_reuse_mutex) != 0) {
        g_key_reuse_failed = 1;
        g_key_reuse_ready = 1;
        (void)pthread_cond_signal(&g_key_reuse_condition);
        return 0;
    }
    g_key_reuse_ready = 1;
    (void)pthread_cond_signal(&g_key_reuse_condition);
    while (!g_key_reuse_release &&
           pthread_cond_wait(&g_key_reuse_condition,
                             &g_key_reuse_mutex) == 0) {
    }
    if (!g_key_reuse_release) g_key_reuse_failed = 1;
    (void)pthread_mutex_unlock(&g_key_reuse_mutex);
    if (pthread_getspecific(key) != 0) g_key_reuse_failed = 1;
    return 0;
}

static int test_key_reuse(void) {
    pthread_key_t old_key = 0;
    pthread_key_t new_key = 0;
    pthread_key_t guard_keys[PTHREAD_KEYS_MAX - 1U] = {0};
    pthread_t worker = 0;
    void *result = 0;
    int old_key_initialized = 0;
    int new_key_initialized = 0;
    int worker_started = 0;
    unsigned int guard_count = 0U;

    g_key_reuse_ready = 0;
    g_key_reuse_release = 0;
    g_key_reuse_failed = 0;
    g_key_reuse_destructor_calls = 0;
    if (pthread_key_create(&old_key, key_reuse_destructor) != 0) return 0;
    old_key_initialized = 1;
    while (guard_count < PTHREAD_KEYS_MAX - 1U &&
           pthread_key_create(&guard_keys[guard_count], 0) == 0) {
        ++guard_count;
    }
    if (guard_count != PTHREAD_KEYS_MAX - 1U) goto cleanup;
    if (pthread_create(&worker, 0, key_reuse_entry,
                       (void *)(uintptr_t)old_key) != 0) goto cleanup;
    worker_started = 1;
    if (pthread_mutex_lock(&g_key_reuse_mutex) != 0) goto cleanup;
    while (!g_key_reuse_ready &&
           pthread_cond_wait(&g_key_reuse_condition,
                             &g_key_reuse_mutex) == 0) {
    }
    (void)pthread_mutex_unlock(&g_key_reuse_mutex);
    if (!g_key_reuse_ready || pthread_key_delete(old_key) != 0) {
        goto cleanup;
    }
    old_key_initialized = 0;
    if (pthread_key_create(&new_key, key_reuse_destructor) != 0) {
        goto cleanup;
    }
    new_key_initialized = 1;
    if (new_key != old_key || pthread_getspecific(new_key) != 0) {
        goto cleanup;
    }
    if (pthread_mutex_lock(&g_key_reuse_mutex) != 0) goto cleanup;
    g_key_reuse_release = 1;
    (void)pthread_cond_signal(&g_key_reuse_condition);
    (void)pthread_mutex_unlock(&g_key_reuse_mutex);
    if (pthread_join(worker, &result) != 0) goto cleanup;
    worker_started = 0;
    if (g_key_reuse_failed || g_key_reuse_destructor_calls != 0) goto cleanup;
    (void)pthread_key_delete(new_key);
    new_key_initialized = 0;
    while (guard_count != 0U) {
        --guard_count;
        (void)pthread_key_delete(guard_keys[guard_count]);
    }
    return 1;

cleanup:
    if (worker_started) {
        (void)pthread_mutex_lock(&g_key_reuse_mutex);
        g_key_reuse_release = 1;
        (void)pthread_cond_signal(&g_key_reuse_condition);
        (void)pthread_mutex_unlock(&g_key_reuse_mutex);
        (void)pthread_join(worker, &result);
        worker_started = 0;
    }
    if (new_key_initialized) (void)pthread_key_delete(new_key);
    if (old_key_initialized) (void)pthread_key_delete(old_key);
    while (guard_count != 0U) {
        --guard_count;
        (void)pthread_key_delete(guard_keys[guard_count]);
    }
    return 0;
}

static void *thread_fp_entry(void *argument) {
    uintptr_t index = (uintptr_t)argument;
    double value = index == 0U ? 2.0 : -3.0;
    double step = index == 0U ? 0.25 : -0.5;
    for (uint32_t iteration = 0U; iteration < 10000000U; ++iteration) {
        value += step;
    }
    g_thread_fp_results[index] = value;
    return 0;
}

static int test_threads(void) {
    pthread_attr_t attribute;
    pthread_t thread;
    pthread_t fp_threads[2];
    void *result = 0;
    errno = EACCES;
    if (pthread_key_create(&g_thread_key, thread_key_destructor) != 0 ||
        pthread_setspecific(g_thread_key, (void *)0x1U) != 0 ||
        pthread_getspecific(g_thread_key) != (void *)0x1U ||
        pthread_attr_init(&attribute) != 0 ||
        pthread_attr_setstacksize(&attribute, 48U * 1024U) != 0 ||
        pthread_create(&thread, &attribute, thread_test_entry,
                       (void *)0x1234U) != 0) {
        (void)pthread_attr_destroy(&attribute);
        return 0;
    }
    (void)pthread_attr_destroy(&attribute);
    if (pthread_mutex_lock(&g_thread_mutex) != 0) return 0;
    while (g_thread_value == 0 &&
           pthread_cond_wait(&g_thread_condition, &g_thread_mutex) == 0) {
    }
    if (pthread_mutex_unlock(&g_thread_mutex) != 0 ||
        pthread_join(thread, &result) != 0 || result != (void *)0x1234U ||
        g_thread_once_calls != 1 || g_thread_value != 0x1234 ||
        g_thread_destructor_calls != 1 || g_thread_errno != EBUSY ||
        errno != EACCES || pthread_key_delete(g_thread_key) != 0) {
        (void)pthread_key_delete(g_thread_key);
        return 0;
    }
    if (!test_key_reuse()) return 0;
    if (pthread_create(&fp_threads[0], 0, thread_fp_entry, (void *)0U) != 0 ||
        pthread_create(&fp_threads[1], 0, thread_fp_entry, (void *)1U) != 0 ||
        pthread_join(fp_threads[0], 0) != 0 ||
        pthread_join(fp_threads[1], 0) != 0 ||
        g_thread_fp_results[0] != 2500002.0 ||
        g_thread_fp_results[1] != -5000003.0) {
        return 0;
    }
    return 1;
}

static int g_c11_once_calls;
static int g_c11_destructor_calls;

static void c11_once_function(void) {
    ++g_c11_once_calls;
}

static void c11_tss_destructor(void *value) {
    if (value != 0) ++g_c11_destructor_calls;
}

typedef struct c11_thread_state {
    mtx_t mutex;
    cnd_t condition;
    once_flag once;
    tss_t key;
    int value;
} c11_thread_state_t;

static int c11_thread_entry(void *raw_state) {
    c11_thread_state_t *state = (c11_thread_state_t *)raw_state;
    call_once(&state->once, c11_once_function);
    if (tss_set(state->key, state) != thrd_success ||
        mtx_lock(&state->mutex) != thrd_success) return 1;
    state->value = 37;
    (void)cnd_signal(&state->condition);
    (void)mtx_unlock(&state->mutex);
    return 37;
}

static int test_c11_recursive_mutex(void) {
    mtx_t mutex = {0};
    unsigned int lock_depth = 0U;
    int initialized = 0;
    int success = 0;

    if (mtx_init(&mutex, mtx_recursive) != thrd_success) goto cleanup;
    initialized = 1;
    if (mtx_lock(&mutex) != thrd_success) goto cleanup;
    ++lock_depth;
    if (mtx_lock(&mutex) != thrd_success) goto cleanup;
    ++lock_depth;
    if (mtx_trylock(&mutex) != thrd_success) goto cleanup;
    ++lock_depth;

    /* Each unlock releases one recursive level, including the trylock level. */
    if (mtx_unlock(&mutex) != thrd_success) goto cleanup;
    --lock_depth;
    if (mtx_unlock(&mutex) != thrd_success) goto cleanup;
    --lock_depth;
    if (mtx_unlock(&mutex) != thrd_success) goto cleanup;
    --lock_depth;
    success = 1;
cleanup:
    while (lock_depth != 0U) {
        if (mtx_unlock(&mutex) != thrd_success) break;
        --lock_depth;
    }
    if (initialized) mtx_destroy(&mutex);
    return success && lock_depth == 0U;
}

static void *errorcheck_unlock_entry(void *raw_mutex) {
    return (void *)(intptr_t)pthread_mutex_unlock(
        (pthread_mutex_t *)raw_mutex);
}

static int test_errorcheck_mutex(void) {
    pthread_mutexattr_t attribute;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_t worker = 0;
    struct timespec deadline;
    void *worker_result = 0;
    int attribute_initialized = 0;
    int mutex_initialized = 0;
    int mutex_type = 0;
    int success = 0;

    if (pthread_mutexattr_init(&attribute) != 0) return 0;
    attribute_initialized = 1;
    if (pthread_mutexattr_settype(&attribute, PTHREAD_MUTEX_ERRORCHECK) != 0 ||
        pthread_mutexattr_gettype(&attribute, &mutex_type) != 0 ||
        mutex_type != PTHREAD_MUTEX_ERRORCHECK ||
        pthread_mutex_init(&mutex, &attribute) != 0) goto cleanup;
    mutex_initialized = 1;
    (void)pthread_mutexattr_destroy(&attribute);
    attribute_initialized = 0;
    if (pthread_mutex_lock(&mutex) != 0 ||
        pthread_mutex_lock(&mutex) != EDEADLK ||
        pthread_mutex_trylock(&mutex) != EBUSY ||
        clock_gettime(CLOCK_REALTIME, &deadline) != 0 ||
        pthread_mutex_timedlock(&mutex, &deadline) != EDEADLK ||
        pthread_create(&worker, 0, errorcheck_unlock_entry, &mutex) != 0 ||
        pthread_join(worker, &worker_result) != 0 ||
        (int)(intptr_t)worker_result != EPERM ||
        pthread_mutex_unlock(&mutex) != 0 ||
        pthread_mutex_unlock(&mutex) != EPERM) {
        goto cleanup;
    }
    success = 1;
cleanup:
    if (worker != 0 && !success) (void)pthread_join(worker, 0);
    if (mutex_initialized) (void)pthread_mutex_destroy(&mutex);
    if (attribute_initialized) (void)pthread_mutexattr_destroy(&attribute);
    return success;
}

typedef struct recursive_condition_state {
    pthread_mutex_t *mutex;
    pthread_cond_t *condition;
} recursive_condition_state_t;

static void *recursive_condition_entry(void *raw_state) {
    recursive_condition_state_t *state =
        (recursive_condition_state_t *)raw_state;
    if (pthread_mutex_lock(state->mutex) != 0) return (void *)(intptr_t)1;
    (void)pthread_cond_signal(state->condition);
    (void)pthread_mutex_unlock(state->mutex);
    return 0;
}

static int test_recursive_condition_wait(void) {
    pthread_mutexattr_t attribute;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
    recursive_condition_state_t state = {&mutex, &condition};
    pthread_t worker = 0;
    void *worker_result = 0;
    unsigned int lock_depth = 0U;
    int attribute_initialized = 0;
    int mutex_initialized = 0;
    int condition_initialized = 0;
    int success = 0;

    if (pthread_mutexattr_init(&attribute) != 0) return 0;
    attribute_initialized = 1;
    if (pthread_mutexattr_settype(&attribute, PTHREAD_MUTEX_RECURSIVE) != 0 ||
        pthread_mutex_init(&mutex, &attribute) != 0) goto cleanup;
    mutex_initialized = 1;
    (void)pthread_mutexattr_destroy(&attribute);
    attribute_initialized = 0;
    if (pthread_cond_init(&condition, 0) != 0) goto cleanup;
    condition_initialized = 1;
    if (pthread_mutex_lock(&mutex) != 0) goto cleanup;
    ++lock_depth;
    if (pthread_mutex_lock(&mutex) != 0) goto cleanup;
    ++lock_depth;
    if (pthread_create(&worker, 0, recursive_condition_entry, &state) != 0 ||
        pthread_cond_wait(&condition, &mutex) != 0 ||
        pthread_mutex_unlock(&mutex) != 0) {
        goto cleanup;
    }
    --lock_depth;
    if (pthread_mutex_unlock(&mutex) != 0) goto cleanup;
    --lock_depth;
    if (lock_depth != 0U || pthread_join(worker, &worker_result) != 0 ||
        worker_result != 0) {
        goto cleanup;
    }
    worker = 0;
    success = 1;
cleanup:
    while (lock_depth != 0U) {
        if (pthread_mutex_unlock(&mutex) != 0) break;
        --lock_depth;
    }
    if (worker != 0) (void)pthread_join(worker, 0);
    if (condition_initialized) {
        (void)pthread_cond_destroy(&condition);
    }
    if (mutex_initialized) {
        (void)pthread_mutex_destroy(&mutex);
    }
    if (attribute_initialized) (void)pthread_mutexattr_destroy(&attribute);
    return success && lock_depth == 0U;
}

typedef struct c11_detached_state {
    mtx_t mutex;
    cnd_t condition;
    volatile int completed;
} c11_detached_state_t;

static int c11_detached_entry(void *raw_state) {
    c11_detached_state_t *state = (c11_detached_state_t *)raw_state;
    if (mtx_lock(&state->mutex) != thrd_success) return 1;
    state->completed = 1;
    (void)cnd_signal(&state->condition);
    (void)mtx_unlock(&state->mutex);
    return 0;
}

static int test_c11_detached_thread(void) {
    c11_detached_state_t state = {0};
    thrd_t thread = 0;
    int mutex_initialized = 0;
    int condition_initialized = 0;
    int mutex_locked = 0;
    int thread_active = 0;
    int detached = 0;
    int success = 0;

    if (mtx_init(&state.mutex, mtx_plain) != thrd_success) goto cleanup;
    mutex_initialized = 1;
    if (cnd_init(&state.condition) != thrd_success) goto cleanup;
    condition_initialized = 1;
    if (mtx_lock(&state.mutex) != thrd_success) goto cleanup;
    mutex_locked = 1;
    if (thrd_create(&thread, c11_detached_entry, &state) != thrd_success) {
        goto cleanup;
    }
    thread_active = 1;
    if (thrd_detach(thread) != thrd_success) goto cleanup;
    detached = 1;
    thread_active = 0;
    while (!state.completed &&
           cnd_wait(&state.condition, &state.mutex) == thrd_success) {
    }
    if (!state.completed || mtx_unlock(&state.mutex) != thrd_success) {
        goto cleanup;
    }
    mutex_locked = 0;
    success = 1;
cleanup:
    if (mutex_locked) {
        (void)mtx_unlock(&state.mutex);
        mutex_locked = 0;
    }
    if (thread_active) {
        /* A failed detach leaves ownership with this test, so join it. */
        (void)thrd_join(thread, 0);
        thread_active = 0;
    } else if (detached) {
        /* The worker has released the mutex before it can be detached. */
        while (!state.completed) thrd_yield();
    }
    if (condition_initialized) cnd_destroy(&state.condition);
    if (mutex_initialized) mtx_destroy(&state.mutex);
    return success;
}

static int test_c11_threads(void) {
    c11_thread_state_t state = {0};
    thrd_t thread = 0;
    int result = 0;
    int initialized_mutex = 0;
    int initialized_condition = 0;
    int initialized_key = 0;
    int mutex_locked = 0;
    int thread_active = 0;
    int success = 0;
    struct timespec zero = {0, 0};

    state.once = (once_flag)ONCE_FLAG_INIT;
    if (mtx_init(&state.mutex, mtx_plain) != thrd_success) goto cleanup;
    initialized_mutex = 1;
    if (cnd_init(&state.condition) != thrd_success) goto cleanup;
    initialized_condition = 1;
    if (tss_create(&state.key, c11_tss_destructor) != thrd_success) goto cleanup;
    initialized_key = 1;
    if (mtx_lock(&state.mutex) != thrd_success) goto cleanup;
    mutex_locked = 1;
    if (thrd_create(&thread, c11_thread_entry, &state) != thrd_success) goto cleanup;
    thread_active = 1;
    while (state.value == 0 &&
           cnd_wait(&state.condition, &state.mutex) == thrd_success) {
    }
    if (state.value != 37) goto cleanup;
    if (mtx_unlock(&state.mutex) != thrd_success) goto cleanup;
    mutex_locked = 0;
    if (thrd_join(thread, &result) != thrd_success) goto cleanup;
    thread_active = 0;
    thread = 0;
    if (result != 37 || g_c11_once_calls != 1 ||
         g_c11_destructor_calls != 1 || tss_get(state.key) != 0 ||
         !test_c11_recursive_mutex() || !test_errorcheck_mutex() ||
         !test_recursive_condition_wait() ||
        thrd_sleep(&zero, 0) != 0 || !test_c11_detached_thread()) {
        goto cleanup;
    }
    success = 1;
cleanup:
    if (mutex_locked) {
        (void)mtx_unlock(&state.mutex);
        mutex_locked = 0;
    }
    if (thread_active) {
        (void)thrd_join(thread, 0);
        thread_active = 0;
        thread = 0;
    }
    if (initialized_key) tss_delete(state.key);
    if (initialized_condition) cnd_destroy(&state.condition);
    if (initialized_mutex) mtx_destroy(&state.mutex);
    return success && initialized_key && initialized_condition &&
           initialized_mutex && g_c11_once_calls == 1 &&
           g_c11_destructor_calls == 1 && result == 37;
}

static int test_floating_point(void) {
    char *end = 0;
    char formatted[128];
    double value;
    float single;
    long double extended;

    errno = 0;
    value = strtod(" -12.5x", &end);
    if (value != -12.5 || end == 0 || strcmp(end, "x") != 0 || errno != 0) {
        return libc_test_detail_fail("floating-strtod");
    }
    single = strtof("0x1.8p+2", &end);
    if (single != 6.0F || end == 0 || *end != '\0') {
        return libc_test_detail_fail("floating-strtof");
    }
    extended = strtold("1.25e2", &end);
    if (extended != 125.0L || end == 0 || *end != '\0') {
        return libc_test_detail_fail("floating-strtold");
    }
    value = strtod("nan(payload)", &end);
    if (value == value || end == 0 || *end != '\0') {
        return libc_test_detail_fail("floating-parse-nan");
    }
    errno = 0;
    value = strtod("1e-5000", &end);
    if (value != 0.0 || errno != ERANGE || end == 0 || *end != '\0') {
        return libc_test_detail_fail("floating-underflow");
    }
    errno = 0;
    if (strtod("not-a-number", &end) != 0.0 || errno != 0 ||
        end == 0 || strcmp(end, "not-a-number") != 0) {
        return libc_test_detail_fail("floating-invalid");
    }
    if (snprintf(formatted, sizeof(formatted), "%.2f", 1.25) != 4 ||
        strcmp(formatted, "1.25") != 0 ||
        snprintf(formatted, sizeof(formatted), "%8.2f", -1.5) != 8 ||
        strcmp(formatted, "   -1.50") != 0 ||
        snprintf(formatted, sizeof(formatted), "%+08.2f", 1.5) != 8 ||
        strcmp(formatted, "+0001.50") != 0) {
        return libc_test_detail_fail("floating-format-fixed");
    }
    if (snprintf(formatted, sizeof(formatted), "%.2e", 1.25) != 8 ||
        strcmp(formatted, "1.25e+00") != 0 ||
        snprintf(formatted, sizeof(formatted), "%.4g", 12345.0) != 9 ||
        strcmp(formatted, "1.234e+04") != 0 ||
        snprintf(formatted, sizeof(formatted), "%.4g", 0.00123) != 7 ||
        strcmp(formatted, "0.00123") != 0 ||
        snprintf(formatted, sizeof(formatted), "%.4g", 1200.0) != 4 ||
        strcmp(formatted, "1200") != 0 ||
        snprintf(formatted, sizeof(formatted), "%.3g", 999.9) != 5 ||
        strcmp(formatted, "1e+03") != 0) {
        return libc_test_detail_fail("floating-format-general");
    }
    if (snprintf(formatted, sizeof(formatted), "%Lf", 1.25L) != 8 ||
        strcmp(formatted, "1.250000") != 0) {
        return libc_test_detail_fail("floating-format-long-double");
    }
    value = strtod("inf", &end);
    if (snprintf(formatted, sizeof(formatted), "%+f", value) != 4 ||
        strcmp(formatted, "+inf") != 0) {
        return libc_test_detail_fail("floating-format-inf");
    }
    value = strtod("nan", &end);
    if (snprintf(formatted, sizeof(formatted), "%F", value) != 3 ||
        strcmp(formatted, "NAN") != 0) {
        return libc_test_detail_fail("floating-format-nan");
    }
    int exponent = 0;
    int quotient = 0;
    double integer = 0.0;
    value = sqrt(2.0);
    if (!isfinite(value) || fabs(value - 1.4142135623730951) > 1e-12 ||
        fabs(sin(0.5) - 0.4794255386042030) > 1e-12 ||
        fabs(cos(0.5) - 0.8775825618903728) > 1e-12 ||
        fabs(exp(1.0) - 2.7182818284590452) > 1e-12 ||
        fabs(log(2.0) - 0.6931471805599453) > 1e-12 ||
        pow(2.0, 10.0) != 1024.0 || hypot(3.0, 4.0) != 5.0 ||
        cbrt(27.0) != 3.0 || floor(-1.25) != -2.0 ||
        ceil(-1.25) != -1.0 || fmod(5.5, 2.0) != 1.5 ||
        remainder(5.5, 2.0) != -0.5 || frexp(12.0, &exponent) != 0.75 ||
        exponent != 4 || ldexp(0.75, 4) != 12.0) {
        return libc_test_detail_fail("floating-basic-math");
    }
    if (modf(-3.25, &integer) != -0.25 || integer != -3.0 ||
        rint(2.5) != 2.0 || nearbyint(3.5) != 4.0 ||
        lrint(2.5) != 2L || lround(-2.5) != -3L ||
        llround(2.5) != 3LL || ilogb(8.0) != 3 || logb(0.75) != -1.0 ||
        scalbln(0.75, 4L) != 12.0) {
        return libc_test_detail_fail("floating-round-scale");
    }
    value = remquo(5.5, 2.0, &quotient);
    if (value != -0.5) {
        return libc_test_detail_fail("floating-remquo-remainder");
    }
    if (quotient != 3) {
        printf("LITEOS_LIBC_TEST_DETAIL_VALUE remquo-quotient=%d\n", quotient);
        return libc_test_detail_fail("floating-remquo-quotient");
    }
    if (
        !(nextafter(1.0, 2.0) > 1.0) ||
        !(nextafter(1.0, 0.0) < 1.0)) {
        return libc_test_detail_fail("floating-nextafter");
    }
    if (!(nexttoward(1.0, 1.0L + LDBL_EPSILON) > 1.0)) {
        return libc_test_detail_fail("floating-nexttoward");
    }
    if (nextafterl(0.0L, 1.0L) != LDBL_TRUE_MIN) {
        return libc_test_detail_fail("floating-nextafterl");
    }
    if (!isnan(nan("liteos"))) {
        return libc_test_detail_fail("floating-nan");
    }
    if (
        fabsl(asinhl(1.0L) - 0.88137358701954302523L) > 1e-15L ||
        fabsl(acoshl(2.0L) - 1.31695789692481670863L) > 1e-15L ||
        fabsl(atanhl(0.5L) - 0.54930614433405484570L) > 1e-15L) {
        return libc_test_detail_fail("floating-inverse-hyperbolic");
    }
    errno = 0;
    value = sqrt(-1.0);
    if (!isnan(value) || errno != EDOM) {
        return libc_test_detail_fail("floating-sqrt-domain");
    }
    errno = 0;
    value = atanh(2.0);
    if (!isnan(value) || errno != EDOM) {
        return libc_test_detail_fail("floating-atanh-domain");
    }
    return 1;
}

static int test_floating_environment(void) {
    fenv_t saved;
    fenv_t held;
    fexcept_t flags = 0;
    int success = 0;

    if (fegetenv(&saved) != 0) return libc_test_detail_fail("fenv-getenv");
    if (feclearexcept(FE_ALL_EXCEPT) != 0 ||
        fesetround(FE_DOWNWARD) != 0 || fegetround() != FE_DOWNWARD ||
        rint(1.25) != 1.0 ||
        fesetround(FE_UPWARD) != 0 || fegetround() != FE_UPWARD ||
        rint(1.25) != 2.0 ||
        fesetround(FE_TONEAREST) != 0 ||
        fesetround(0x100) == 0) {
        goto done;
    }
    if (feraiseexcept(FE_INVALID | FE_DIVBYZERO) != 0 ||
        (fetestexcept(FE_ALL_EXCEPT) & (FE_INVALID | FE_DIVBYZERO)) !=
            (FE_INVALID | FE_DIVBYZERO) ||
        fegetexceptflag(&flags, FE_INVALID) != 0 || flags != FE_INVALID ||
        feclearexcept(FE_INVALID) != 0 ||
        (fetestexcept(FE_ALL_EXCEPT) & FE_INVALID) != 0 ||
        fesetexceptflag(&flags, FE_INVALID) != 0 ||
        (fetestexcept(FE_ALL_EXCEPT) & FE_INVALID) == 0) {
        goto done;
    }
    if (feholdexcept(&held) != 0 || fetestexcept(FE_ALL_EXCEPT) != 0 ||
        feraiseexcept(FE_OVERFLOW) != 0 ||
        (fetestexcept(FE_ALL_EXCEPT) & FE_OVERFLOW) == 0 ||
        feupdateenv(&held) != 0 ||
        (fetestexcept(FE_ALL_EXCEPT) & (FE_INVALID | FE_OVERFLOW)) !=
            (FE_INVALID | FE_OVERFLOW)) {
        goto done;
    }
    success = 1;

done:
    if (fesetenv(&saved) != 0) success = 0;
    return success ? 1 : libc_test_detail_fail("floating-environment");
}

#define LITEOS_TEST_SSE __attribute__((target("sse")))

static int LITEOS_TEST_SSE test_complex_and_unicode(void) {
    const double pi = 3.141592653589793238462643383279502884;
    const char smile[] = "\xf0\x9f\x98\x80";
    float complex float_value = CMPLXF(1.0F, 2.0F);
    double complex double_value = CMPLX(3.0, 4.0);
    long double complex long_value = CMPLXL(3.0L, 4.0L);
    float complex float_product;
    double complex double_quotient;
    double complex square_root;
    double complex exponential;
    double complex logarithm;
    double complex power;
    double complex conjugate;
    float generic_float;
    float generic_complex_magnitude;
    long double generic_long;
    char16_t utf16[2] = {0};
    char32_t utf32 = 0;
    char encoded[8] = {0};
    mbstate_t state = {0};
    size_t result;

    if (crealf(float_value) != 1.0F || cimagf(float_value) != 2.0F ||
        creal(double_value) != 3.0 || cimag(double_value) != 4.0 ||
        creall(long_value) != 3.0L || cimagl(long_value) != 4.0L ||
        cabsf(float_value) < 2.23606F || cabsf(float_value) > 2.23607F ||
        cabs(double_value) != 5.0 || cabsl(long_value) != 5.0L) {
        return libc_test_detail_fail("complex-components");
    }

    float_product = float_value * CMPLXF(3.0F, -4.0F);
    double_quotient = CMPLX(5.0, 5.0) / CMPLX(1.0, -1.0);
    if (crealf(float_product) != 11.0F || cimagf(float_product) != 2.0F ||
        creal(double_quotient) != 0.0 || cimag(double_quotient) != 5.0) {
        return libc_test_detail_fail("complex-arithmetic");
    }

    square_root = csqrt(double_value);
    exponential = cexp(CMPLX(0.0, pi));
    logarithm = clog(double_value);
    power = cpow(CMPLX(2.0, 0.0), CMPLX(3.0, 0.0));
    conjugate = conj(double_value);
    if (creal(square_root) < 1.99999 || creal(square_root) > 2.00001 ||
        cimag(square_root) < 0.99999 || cimag(square_root) > 1.00001 ||
        creal(exponential) > -0.99999 || creal(exponential) < -1.00001 ||
        cimag(exponential) > 1e-12 || cimag(exponential) < -1e-12 ||
        creal(logarithm) < 1.60943 || creal(logarithm) > 1.60945 ||
        cimag(logarithm) < 0.92729 || cimag(logarithm) > 0.92730 ||
        fabs(creal(power) - 8.0) > 1e-12 || fabs(cimag(power)) > 1e-12 ||
        creal(conjugate) != 3.0 || cimag(conjugate) != -4.0) {
        return libc_test_detail_fail("complex-functions");
    }

    generic_float = fabs(-2.0F);
    generic_complex_magnitude = fabs(CMPLXF(3.0F, 4.0F));
    generic_long = sqrt(4.0L);
    if (generic_float != 2.0F || generic_complex_magnitude != 5.0F ||
        generic_long != 2.0L ||
        creal(sin(CMPLX(0.0, 0.0))) != 0.0 ||
        cimag(sin(CMPLX(0.0, 0.0))) != 0.0 ||
        carg(CMPLX(0.0, 1.0)) < 1.57079 ||
        carg(CMPLX(0.0, 1.0)) > 1.57080) {
        return libc_test_detail_fail("tgmath-selection");
    }

    result = mbrtoc16(&utf16[0], smile, sizeof(smile) - 1U, &state);
    if (result != sizeof(smile) - 1U || utf16[0] != (char16_t)0xd83dU ||
        mbsinit(&state) != 0) {
        return libc_test_detail_fail("uchar-high-surrogate");
    }
    result = mbrtoc16(&utf16[1], "", 1U, &state);
    if (result != (size_t)-3 || utf16[1] != (char16_t)0xde00U ||
        mbsinit(&state) != 1) {
        return libc_test_detail_fail("uchar-low-surrogate");
    }

    state = (mbstate_t){0};
    result = mbrtoc32(&utf32, smile, 1U, &state);
    if (result != (size_t)-2) {
        return libc_test_detail_fail("uchar-partial");
    }
    result = mbrtoc32(&utf32, smile + 1, sizeof(smile) - 2U, &state);
    if (result != sizeof(smile) - 2U || utf32 != (char32_t)0x1f600U) {
        return libc_test_detail_fail("uchar-complete");
    }
    state = (mbstate_t){0};
    result = c32rtomb(encoded, utf32, &state);
    if (result != sizeof(smile) - 1U ||
        memcmp(encoded, smile, sizeof(smile) - 1U) != 0) {
        return libc_test_detail_fail("uchar-c32rtomb");
    }
    state = (mbstate_t){0};
    result = c16rtomb(encoded, utf16[0], &state);
    if (result != 0U || c16rtomb(encoded, utf16[1], &state) !=
                             sizeof(smile) - 1U ||
        memcmp(encoded, smile, sizeof(smile) - 1U) != 0) {
        return libc_test_detail_fail("uchar-c16rtomb");
    }

    errno = 0;
    state = (mbstate_t){0};
    result = mbrtoc32(&utf32, "\xc0\x80", 2U, &state);
    if (result != (size_t)-1 || errno != EILSEQ || mbsinit(&state) != 1) {
        return libc_test_detail_fail("uchar-invalid-utf8");
    }
    errno = 0;
    result = c16rtomb(encoded, (char16_t)0xdc00U, &state);
    if (result != (size_t)-1 || errno != EILSEQ || mbsinit(&state) != 1) {
        return libc_test_detail_fail("uchar-invalid-surrogate");
    }
    return 1;
}

#undef LITEOS_TEST_SSE

static int test_process_lifecycle(void) {
    volatile int cow_marker = 1;
    int wait_status = 0;
    pid_t child = fork();
    pid_t result;

    if (child < 0) return 0;
    if (child == 0) _exit(cow_marker == 1 ? 7 : 8);
    cow_marker = 3;
    result = waitpid(child, &wait_status, WNOHANG);
    if (result < 0) return 0;
    if (result == 0) result = waitpid(child, &wait_status, 0);
    if (result != child || !WIFEXITED(wait_status) ||
        WEXITSTATUS(wait_status) != 7 || cow_marker != 3) return 0;
    errno = 0;
    return waitpid(child, 0, WNOHANG) == -1 && errno == ECHILD;
}

static int test_exec_descriptor_child(int argc, char **argv) {
    int inherited;
    int closed;
    if (argc != 4 || argv == 0) return 1;
    inherited = (int)strtol(argv[2], 0, 10);
    closed = (int)strtol(argv[3], 0, 10);
    if (getenv("LITEOS_EXEC_ENV") == 0 ||
        strcmp(getenv("LITEOS_EXEC_ENV"), "ok") != 0 ||
        fcntl(inherited, F_GETFD) < 0) return 2;
    errno = 0;
    if (fcntl(closed, F_GETFD) >= 0 || errno != EBADF) return 3;
    return 0;
}

static int test_cloexec_exec(void) {
    char inherited_text[16];
    char closed_text[16];
    char environment_text[] = "LITEOS_EXEC_ENV=ok";
    char *arguments[5];
    char *environment[] = {environment_text, 0};
    int inherited = open("/etc/vfsmap.tst", O_RDONLY);
    int closed = open("/etc/vfsmap.tst", O_RDONLY | O_CLOEXEC);
    int wait_status = 0;
    pid_t child;
    if (inherited < 0 || closed < 0) {
        if (inherited >= 0) (void)close(inherited);
        if (closed >= 0) (void)close(closed);
        return libc_test_detail_fail("cloexec-open");
    }
    (void)snprintf(inherited_text, sizeof(inherited_text), "%d", inherited);
    (void)snprintf(closed_text, sizeof(closed_text), "%d", closed);
    arguments[0] = "/sbin/libc-test";
    arguments[1] = "--fd-check";
    arguments[2] = inherited_text;
    arguments[3] = closed_text;
    arguments[4] = 0;
    child = fork();
    if (child == 0) {
        (void)execve(arguments[0], arguments, environment);
        _exit(127);
    }
    if (child < 0 || waitpid(child, &wait_status, 0) != child ||
        !WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
        if (child > 0) (void)waitpid(child, 0, 0);
        (void)close(inherited);
        (void)close(closed);
        return libc_test_detail_fail("cloexec-exec");
    }
    if (close(inherited) != 0 || close(closed) != 0) {
        return libc_test_detail_fail("cloexec-close");
    }
    return 1;
}

static int run_nasm_once(const char *source, const char *output) {
    char *arguments[] = {
        (char *)"nasm", (char *)"-f", (char *)"bin",
        (char *)source, (char *)"-o", (char *)output, 0
    };
    int wait_status = 0;
    pid_t child = fork();
    if (child < 0) return 0;
    if (child == 0) {
        (void)execve("/sbin/nasm", arguments, environ);
        _exit(127);
    }
    return waitpid(child, &wait_status, 0) == child && WIFEXITED(wait_status) &&
           WEXITSTATUS(wait_status) == 0;
}

static int remove_path_direct(const char *path) {
    os_file_path_op_t request = {0};
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.path = (uint64_t)(uintptr_t)path;
    return liteos_syscall6(OS_SYS_FILE_REMOVE, (uint64_t)(uintptr_t)&request,
                           0U, 0U, 0U, 0U, 0U) >= 0;
}

static int refresh_directory_direct(const char *path) {
    for (uint32_t index = 0U; index < 64U; ++index) {
        os_file_enumerate_t request = {0};
        int64_t status;
        request.hdr.size = sizeof(request);
        request.hdr.version = OS_SYSCALL_ABI_VERSION;
        request.path = (uint64_t)(uintptr_t)path;
        request.index = index;
        status = liteos_syscall6(OS_SYS_FILE_ENUMERATE,
                                 (uint64_t)(uintptr_t)&request,
                                 0U, 0U, 0U, 0U, 0U);
        if (status == -2) return 1;
        if (status < 0) return 0;
    }
    return 1;
}

static int refresh_fileman_direct(void) {
    static const char *const containers[] = {
        "/mnt", "/media", "/volumes", "/disks", "/drives",
    };

    for (uint32_t container = 0U;
         container < sizeof(containers) / sizeof(containers[0]); ++container) {
        struct stat status;
        if (stat(containers[container], &status) != 0 ||
            !S_ISDIR(status.st_mode)) continue;
        for (uint32_t index = 0U; index < 64U; ++index) {
            os_file_enumerate_t request = {0};
            int64_t result;
            request.hdr.size = sizeof(request);
            request.hdr.version = OS_SYSCALL_ABI_VERSION;
            request.path = (uint64_t)(uintptr_t)containers[container];
            request.index = index;
            result = liteos_syscall6(OS_SYS_FILE_ENUMERATE,
                                     (uint64_t)(uintptr_t)&request,
                                     0U, 0U, 0U, 0U, 0U);
            if (result == -2) break;
            if (result < 0) return 0;
        }
    }
    return refresh_directory_direct("/");
}

static int test_nasm_output_recreate(void) {
    static const char source_text[] = "bits 64\n" "times 8192 db 0x41\n";
    static const unsigned char expected[] = {0x41, 0x41, 0x41, 0x41};
    const char *source_path = "/notes.txt";
    const char *output_argument = "out.bin";
    const char *output_path = "/out.bin";
    const char *fileman_remove_path = "/OUT.BIN";
    unsigned char output[sizeof(expected)];
    struct stat status;
    FILE *stream;
    int held_descriptor = -1;
    size_t bytes;

    (void)unlink(output_path);
    (void)unlink(source_path);
    stream = fopen(source_path, "w");
    if (stream == 0) {
        (void)unlink(source_path);
        return libc_test_detail_fail("files-nasm-source");
    }
    if (fputs(source_text, stream) < 0) {
        (void)fclose(stream);
        (void)unlink(source_path);
        return libc_test_detail_fail("files-nasm-source");
    }
    if (fclose(stream) != 0) {
        (void)unlink(source_path);
        return libc_test_detail_fail("files-nasm-source");
    }
    if (!run_nasm_once(source_path, output_argument) ||
        stat(output_path, &status) != 0 || status.st_size != 8192) {
        (void)unlink(source_path);
        (void)unlink(output_path);
        return libc_test_detail_fail("files-nasm-first");
    }
    held_descriptor = open(output_path, O_RDONLY);
    if (held_descriptor < 0 || !refresh_fileman_direct()) {
        if (held_descriptor >= 0) (void)close(held_descriptor);
        (void)unlink(source_path);
        (void)unlink(output_path);
        return libc_test_detail_fail("files-nasm-refresh-before");
    }
    if (!remove_path_direct(fileman_remove_path)) {
        if (held_descriptor >= 0) (void)close(held_descriptor);
        (void)unlink(source_path);
        (void)unlink(output_path);
        return libc_test_detail_fail("files-nasm-remove");
    }
    if (stat(output_path, &status) == 0) {
        if (held_descriptor >= 0) (void)close(held_descriptor);
        (void)unlink(source_path);
        (void)unlink(output_path);
        return libc_test_detail_fail("files-nasm-stat-after-remove");
    }
    if (!refresh_fileman_direct()) {
        if (held_descriptor >= 0) (void)close(held_descriptor);
        (void)unlink(source_path);
        (void)unlink(output_path);
        return libc_test_detail_fail("files-nasm-refresh-after");
    }
    if (!run_nasm_once(source_path, output_argument)) {
        if (held_descriptor >= 0) (void)close(held_descriptor);
        (void)unlink(source_path);
        (void)unlink(output_path);
        return libc_test_detail_fail("files-nasm-second");
    }
    if (close(held_descriptor) != 0) {
        (void)unlink(source_path);
        (void)unlink(output_path);
        return libc_test_detail_fail("files-nasm-close-old");
    }
    stream = fopen(output_path, "rb");
    bytes = stream == 0 ? 0U : fread(output, 1U, sizeof(output), stream);
    if (stream != 0 && fclose(stream) != 0) bytes = 0U;
    (void)unlink(source_path);
    (void)unlink(output_path);
    if (bytes != sizeof(expected) || memcmp(output, expected, sizeof(expected)) != 0) {
        return libc_test_detail_fail("files-nasm-content");
    }
    return 1;
}

static int test_process_shared_sync(void);

static int test_extended_runtime(void) {
    char *arguments[] = {(char *)"test", (char *)"-a", (char *)"-bvalue", 0};
    char *line = 0;
    size_t line_capacity = 0U;
    char utf8[32];
    wchar_t wide[16];
    const char *multibyte = "LiteOS \xe4\xb8\xad";
    struct dirent **entries = 0;
    struct iovec vectors[2];
    char first[] = "hello";
    char second[] = " world";
    char combined[16] = {0};
    char partial[8] = {0};
    char template_name[] = "/tmp/libc-uio-XXXXXX";
    int descriptor;
    int count;
    int entry_count;
    int option;
    int jump_value;

    if (fnmatch("*.tst", "vfsmap.tst", 0) != 0 ||
        fnmatch("*.tst", "etc/vfsmap.tst", FNM_PATHNAME) != FNM_NOMATCH ||
        fnmatch("[a-z]iteos", "liteos", 0) != 0) {
        return libc_test_detail_fail("extended-fnmatch");
    }

    optind = 1;
    opterr = 0;
    option = getopt(3, arguments, "ab:");
    if (option != 'a' || getopt(3, arguments, "ab:") != 'b' ||
        optarg == 0 || strcmp(optarg, "value") != 0 ||
        getopt(3, arguments, "ab:") != -1) {
        return libc_test_detail_fail("extended-getopt");
    }

    if (mbstowcs(wide, multibyte, sizeof(wide) / sizeof(wide[0])) != 8U ||
        !iswalpha(wide[0]) || wcscmp(wide, L"LiteOS \x4e2d") != 0 ||
        wcstombs(utf8, wide, sizeof(utf8)) != strlen(multibyte) ||
        strcmp(utf8, multibyte) != 0 || wcswidth(wide, 8U) != 8) {
        return libc_test_detail_fail("extended-wide");
    }

    jump_value = setjmp(g_jump_environment);
    if (jump_value == 0) {
        trigger_longjmp();
        return libc_test_detail_fail("extended-longjmp-returned");
    }
    if (jump_value != 7) return libc_test_detail_fail("extended-longjmp-value");

    descriptor = mkstemp(template_name);
    if (descriptor < 0) return libc_test_detail_fail("extended-mkstemp");
    vectors[0].iov_base = first;
    vectors[0].iov_len = strlen(first);
    vectors[1].iov_base = second;
    vectors[1].iov_len = strlen(second);
    if (writev(descriptor, vectors, 2) != 11 ||
        lseek(descriptor, 0, SEEK_SET) != 0 ||
        readv(descriptor, &(struct iovec){combined, 11U}, 1) != 11 ||
        strcmp(combined, "hello world") != 0 ||
        preadv(descriptor, &(struct iovec){partial, 5U}, 1, 6) != 5 ||
        memcmp(partial, "world", 5U) != 0 ||
        pwritev(descriptor, &(struct iovec){first, 5U}, 1, 0) != 5 ||
        close(descriptor) != 0 || unlink(template_name) != 0) {
        (void)close(descriptor);
        (void)unlink(template_name);
        return libc_test_detail_fail("extended-vector-io");
    }

    entry_count = scandir("/etc", &entries, 0, alphasort);
    if (entry_count <= 0 || entries == 0) {
        return libc_test_detail_fail("extended-scandir");
    }
    count = 0;
    while (count < entry_count) {
        free(entries[count]);
        ++count;
    }
    free(entries);

    {
        FILE *stream = fopen("/etc/vfsmap.tst", "r");
        ssize_t length;
        bool closed = false;
        bool failed = stream == 0;
        if (!failed) {
            length = getline(&line, &line_capacity, stream);
            failed = length != 5 || strcmp(line, "world") != 0;
        }
        if (!failed && getline(&line, &line_capacity, stream) != -1) {
            failed = true;
        }
        if (!failed && fclose(stream) != 0) {
            failed = true;
            closed = true;
        } else if (!failed) {
            closed = true;
        }
        if (failed) {
            free(line);
            if (stream != 0 && !closed) (void)fclose(stream);
            return libc_test_detail_fail("extended-getline");
        }
    }
    free(line);

    {
        pthread_rwlock_t lock = PTHREAD_RWLOCK_INITIALIZER;
        pthread_spinlock_t spin = {0};
        pthread_barrier_t barrier;
        if (pthread_rwlock_rdlock(&lock) != 0 || pthread_rwlock_unlock(&lock) != 0 ||
            pthread_rwlock_wrlock(&lock) != 0 || pthread_rwlock_unlock(&lock) != 0 ||
            pthread_spin_init(&spin, PTHREAD_PROCESS_PRIVATE) != 0 ||
            pthread_spin_lock(&spin) != 0 || pthread_spin_unlock(&spin) != 0 ||
            pthread_spin_destroy(&spin) != 0 ||
            pthread_barrier_init(&barrier, 0, 1U) != 0 ||
            pthread_barrier_wait(&barrier) != PTHREAD_BARRIER_SERIAL_THREAD ||
            pthread_barrier_destroy(&barrier) != 0) {
            return libc_test_detail_fail("extended-sync");
        }
    }
    if (!test_process_shared_sync()) {
        return libc_test_detail_fail("extended-process-shared");
    }
    return 1;
}

static int test_pipes(void) {
    int pipefd[2] = {-1, -1};
    int duplicate = -1;
    struct pollfd event = {0};
    struct stat status = {0};
    char buffer[8] = {0};
    int child_status = 0;
    pid_t child;
    if (pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) != 0 ||
        (fcntl(pipefd[0], F_GETFL) & O_NONBLOCK) == 0 ||
        (fcntl(pipefd[1], F_GETFD) & FD_CLOEXEC) == 0 ||
        fstat(pipefd[0], &status) != 0 ||
        !S_ISFIFO(status.st_mode)) {
        if (pipefd[0] >= 0) (void)close(pipefd[0]);
        if (pipefd[1] >= 0) (void)close(pipefd[1]);
        return libc_test_detail_fail("pipes-create");
    }
    event.fd = pipefd[0];
    event.events = POLLIN;
    if (poll(&event, 1U, 0) != 0 ||
        write(pipefd[1], "abc", 3U) != 3 ||
        poll(&event, 1U, 0) != 1 || (event.revents & POLLIN) == 0 ||
        read(pipefd[0], buffer, sizeof(buffer)) != 3 ||
        memcmp(buffer, "abc", 3U) != 0 || read(pipefd[0], buffer, 1U) != -1 ||
        errno != EAGAIN || lseek(pipefd[0], 0, SEEK_CUR) != -1 ||
        errno != ESPIPE || close(pipefd[1]) != 0 ||
        poll(&event, 1U, 0) != 1 || (event.revents & POLLHUP) == 0 ||
        read(pipefd[0], buffer, 1U) != 0 || close(pipefd[0]) != 0) {
        if (pipefd[0] >= 0) (void)close(pipefd[0]);
        if (pipefd[1] >= 0) (void)close(pipefd[1]);
        return libc_test_detail_fail("pipes-io");
    }

    if (pipe(pipefd) != 0 || (duplicate = dup(pipefd[1])) < 0 ||
        close(pipefd[1]) != 0 || write(duplicate, "d", 1U) != 1 ||
        read(pipefd[0], buffer, 1U) != 1 || buffer[0] != 'd' ||
        close(duplicate) != 0 || read(pipefd[0], buffer, 1U) != 0 ||
        close(pipefd[0]) != 0) {
        if (duplicate >= 0) (void)close(duplicate);
        if (pipefd[0] >= 0) (void)close(pipefd[0]);
        if (pipefd[1] >= 0) (void)close(pipefd[1]);
        return libc_test_detail_fail("pipes-dup");
    }

    if (pipe(pipefd) != 0) return libc_test_detail_fail("pipes-fork-create");
    child = fork();
    if (child < 0) {
        (void)close(pipefd[0]);
        (void)close(pipefd[1]);
        return libc_test_detail_fail("pipes-fork");
    }
    if (child == 0) {
        if (close(pipefd[0]) != 0 || write(pipefd[1], "f", 1U) != 1 ||
            close(pipefd[1]) != 0) _exit(99);
        _exit(0);
    }
    if (close(pipefd[1]) != 0 || read(pipefd[0], buffer, 1U) != 1 ||
        buffer[0] != 'f' || close(pipefd[0]) != 0 ||
        waitpid(child, &child_status, 0) != child ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        (void)close(pipefd[0]);
        (void)close(pipefd[1]);
        (void)waitpid(child, 0, 0);
        return libc_test_detail_fail("pipes-fork-io");
    }
    return 1;
}

static int test_process_shared_sync(void) {
    struct shared_state {
        pthread_mutex_t mutex;
        volatile uint32_t marker;
    } *shared;
    pthread_mutexattr_t attribute;
    pid_t child;
    int status = 0;
    int result = 0;

    shared = (struct shared_state *)mmap(
        0, 4096U, PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) return 0;
    if (pthread_mutexattr_init(&attribute) != 0) {
        (void)munmap(shared, 4096U);
        return 0;
    }
    if (pthread_mutexattr_setpshared(&attribute, PTHREAD_PROCESS_SHARED) != 0 ||
        pthread_mutex_init(&shared->mutex, &attribute) != 0) {
        (void)pthread_mutexattr_destroy(&attribute);
        (void)munmap(shared, 4096U);
        return 0;
    }
    shared->marker = 0U;
    if (pthread_mutex_lock(&shared->mutex) != 0) result = 1;
    child = result == 0 ? fork() : (pid_t)-1;
    if (child == 0) {
        if (pthread_mutex_lock(&shared->mutex) != 0) _exit(1);
        shared->marker = 1U;
        (void)pthread_mutex_unlock(&shared->mutex);
        _exit(0);
    }
    if (child < 0 || pthread_mutex_unlock(&shared->mutex) != 0 ||
        waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0 || shared->marker != 1U) {
        if (child > 0) (void)waitpid(child, 0, 0);
        result = 1;
    }
    (void)pthread_mutex_destroy(&shared->mutex);
    (void)pthread_mutexattr_destroy(&attribute);
    (void)munmap(shared, 4096U);
    return result == 0;
}

static int test_files(void) {
    const char *path = "/etc/vfsmap.tst";
    char bytes[6] = {0};
    char resolved[PATH_MAX];
    char scanned[32];
    char formatted[96];
    char *allocated = 0;
    fpos_t position;
    struct stat status;
    struct stat at_status;
    FILE *stream;
    DIR *directory;
    DIR *fd_directory;
    struct dirent directory_entry;
    struct dirent *directory_result;
    struct dirent *entry;
    int number;
    unsigned int hexadecimal;
    int descriptor;
    int duplicate;
    int cloexec_duplicate;
    int directory_descriptor;
    int at_descriptor;
    int found_entry = 0;

    descriptor = open(path, O_RDONLY);
    if (descriptor < 0) return libc_test_detail_fail("files-open");
    if (fstat(descriptor, &status) != 0) {
        (void)close(descriptor);
        return libc_test_detail_fail("files-fstat");
    }
    if (!S_ISREG(status.st_mode)) {
        (void)close(descriptor);
        return libc_test_detail_fail("files-type");
    }
    if (status.st_size != 5) {
        (void)close(descriptor);
        return libc_test_detail_fail("files-size");
    }
    if (fpathconf(descriptor, _PC_NAME_MAX) != 63L) {
        (void)close(descriptor);
        return libc_test_detail_fail("files-pathconf");
    }
    if (read(descriptor, bytes, sizeof(bytes) - 1U) != 5) {
        (void)close(descriptor);
        return libc_test_detail_fail("files-read");
    }
    if (strcmp(bytes, "world") != 0) {
        (void)close(descriptor);
        return libc_test_detail_fail("files-content");
    }
    errno = 0;
    if (fcntl(descriptor, 0x7fffffff) != -1 || errno != EINVAL) {
        (void)close(descriptor);
        return libc_test_detail_fail("files-fcntl-invalid");
    }
    errno = 0;
    if (ioctl(descriptor, 0x4c00UL) != -1 || errno != ENOTTY) {
        (void)close(descriptor);
        return libc_test_detail_fail("files-ioctl-unsupported");
    }
    duplicate = dup(descriptor);
    if (duplicate < 0 || fcntl(duplicate, F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(duplicate, F_GETFD) != FD_CLOEXEC ||
        fcntl(descriptor, F_GETFD) != 0 || close(descriptor) != 0 ||
        lseek(duplicate, 0, SEEK_SET) != 0 || read(duplicate, bytes, 5U) != 5 ||
        close(duplicate) != 0) return libc_test_detail_fail("files-dup");

    descriptor = open(path, O_RDONLY);
    if (descriptor < 0) return libc_test_detail_fail("files-reopen");
    cloexec_duplicate = dup3(descriptor, 200, O_CLOEXEC);
    if (cloexec_duplicate != 200 ||
        fcntl(cloexec_duplicate, F_GETFD) != FD_CLOEXEC ||
        close(cloexec_duplicate) != 0 || close(descriptor) != 0) {
        if (cloexec_duplicate >= 0) (void)close(cloexec_duplicate);
        (void)close(descriptor);
        return libc_test_detail_fail("files-dup3");
    }

    descriptor = open(path, O_RDONLY | O_CLOEXEC);
    duplicate = descriptor < 0 ? -1 : dup2(descriptor, 201);
    if (duplicate != 201 || fcntl(duplicate, F_GETFD) != 0 ||
        close(duplicate) != 0 || close(descriptor) != 0) {
        if (duplicate >= 0) (void)close(duplicate);
        if (descriptor >= 0) (void)close(descriptor);
        return libc_test_detail_fail("files-dup2-cloexec");
    }

    directory_descriptor = open("/etc", O_RDONLY | O_DIRECTORY);
    if (directory_descriptor < 0 ||
        fstatat(directory_descriptor, "vfsmap.tst", &at_status, 0) != 0 ||
        !S_ISREG(at_status.st_mode) || at_status.st_size != 5 ||
        faccessat(directory_descriptor, "vfsmap.tst", R_OK, 0) != 0) {
        if (directory_descriptor >= 0) (void)close(directory_descriptor);
        return libc_test_detail_fail("files-at-stat");
    }
    at_descriptor = openat(directory_descriptor, "vfsmap.tst", O_RDONLY);
    if (at_descriptor < 0 || fstat(at_descriptor, &at_status) != 0 ||
        !S_ISREG(at_status.st_mode) || close(at_descriptor) != 0) {
        if (at_descriptor >= 0) (void)close(at_descriptor);
        (void)close(directory_descriptor);
        return libc_test_detail_fail("files-openat");
    }
    fd_directory = fdopendir(directory_descriptor);
    directory_result = 0;
    if (fd_directory == 0 ||
        readdir_r(fd_directory, &directory_entry, &directory_result) != 0) {
        if (fd_directory != 0) (void)closedir(fd_directory);
        else (void)close(directory_descriptor);
        return libc_test_detail_fail("files-fdopendir");
    }
    /* The first entry is implementation-defined; only verify fdopendir made
     * a usable directory stream. */
    if (directory_result == 0 || closedir(fd_directory) != 0) {
        return libc_test_detail_fail("files-readdir-r");
    }

    stream = fopen(path, "r");
    if (stream == 0 || fileno(stream) < 0 ||
        fread(bytes, 1U, 5U, stream) != 5U || bytes[0] != 'w' ||
        fseek(stream, 0L, SEEK_SET) != 0 || fgetc(stream) != 'w' ||
        ungetc('w', stream) != 'w' || fgetc(stream) != 'w' ||
        fgetpos(stream, &position) != 0 || fsetpos(stream, &position) != 0) {
        if (stream != 0) fclose(stream);
        return libc_test_detail_fail("files-stream-read");
    }
    rewind(stream);
    if (fgets(scanned, sizeof(scanned), stream) == 0 ||
        strcmp(scanned, "world") != 0 || fclose(stream) != 0) {
        return libc_test_detail_fail("files-stream-rewind");
    }

    descriptor = open(path, O_RDONLY);
    if (descriptor < 0) return libc_test_detail_fail("files-fdopen-open");
    stream = fdopen(descriptor, "re");
    if (stream == 0) {
        (void)close(descriptor);
        return libc_test_detail_fail("files-fdopen");
    }
    if ((fcntl(fileno(stream), F_GETFD) & FD_CLOEXEC) == 0 ||
        fseeko(stream, 5, SEEK_SET) != 0 || ftello(stream) != 5 ||
        fgetpos(stream, &position) != 0 || fsetpos(stream, &position) != 0) {
        (void)fclose(stream);
        return libc_test_detail_fail("files-fdopen-seek");
    }
    {
        off_t large_position = (off_t)INT32_MAX + 1;
        if (fseeko(stream, large_position, SEEK_SET) != 0 ||
            ftello(stream) != large_position ||
            fseeko(stream, 0, SEEK_SET) != 0) {
            (void)fclose(stream);
            return libc_test_detail_fail("files-large-seek");
        }
    }
    if (fclose(stream) != 0) return libc_test_detail_fail("files-fdopen-close");

    descriptor = open64(path, O_RDONLY);
    if (descriptor < 0 || lseek64(descriptor, 0, SEEK_END) != 5 ||
        lseek64(descriptor, 0, SEEK_SET) != 0 ||
        pread64(descriptor, bytes, 5U, 0) != 5 ||
        memcmp(bytes, "world", 5U) != 0 || close(descriptor) != 0) {
        if (descriptor >= 0) (void)close(descriptor);
        return libc_test_detail_fail("files-large-api");
    }

    descriptor = open(path, O_RDONLY);
    duplicate = descriptor < 0 ? -1 : fcntl(descriptor, F_DUPFD_CLOEXEC, 202);
    if (duplicate != 202 || fcntl(duplicate, F_GETFD) != FD_CLOEXEC ||
        fcntl(descriptor, F_GETFD) != 0 || close(duplicate) != 0 ||
        close(descriptor) != 0) {
        if (duplicate >= 0) (void)close(duplicate);
        if (descriptor >= 0) (void)close(descriptor);
        return libc_test_detail_fail("files-fcntl-dupfd");
    }

    {
        wchar_t wide_text[16];
        wchar_t wide_scan[16];
        FILE *wide_stream = fopen(path, "r");
        if (wide_stream == 0 || fwide(wide_stream, 1) <= 0 ||
            fgetws(wide_scan, sizeof(wide_scan) / sizeof(wide_scan[0]),
                   wide_stream) == 0 || wcscmp(wide_scan, L"world") != 0) {
            if (wide_stream != 0) (void)fclose(wide_stream);
            return libc_test_detail_fail("files-wide-read");
        }
        (void)fclose(wide_stream);
        wide_stream = tmpfile();
        if (wide_stream == 0 || fwide(wide_stream, 1) <= 0 ||
            fputws(L"hello", wide_stream) == (int)WEOF ||
            fseeko(wide_stream, 0, SEEK_SET) != 0 ||
            fgetws(wide_text, sizeof(wide_text) / sizeof(wide_text[0]),
                   wide_stream) == 0 || wcscmp(wide_text, L"hello") != 0 ||
            fclose(wide_stream) != 0) {
            if (wide_stream != 0) (void)fclose(wide_stream);
            return libc_test_detail_fail("files-wide-write");
        }
    }

    if (sscanf("42 0x2a", "%d %x", &number, &hexadecimal) != 2 ||
        number != 42 || hexadecimal != 42U ||
        snprintf(formatted, sizeof(formatted), "%s:%d", "ok", number) != 5 ||
        strcmp(formatted, "ok:42") != 0 ||
        asprintf(&allocated, "%s", "allocated") < 0 ||
        strcmp(allocated, "allocated") != 0) {
        free(allocated);
        return libc_test_detail_fail("files-format");
    }
    free(allocated);

    directory = opendir("/etc");
    if (directory == 0) return libc_test_detail_fail("files-opendir");
    while ((entry = readdir(directory)) != 0) {
        if (entry->d_name[0] != '\0') found_entry = 1;
    }
    if (closedir(directory) != 0 || !found_entry ||
        realpath(path, resolved) == 0 || strcmp(resolved, path) != 0) {
        return libc_test_detail_fail("files-directory-final");
    }
    return test_nasm_output_recreate();
}

int main(int argc, char **argv) {
    char source[32];
    char copy[32];
    char formatted[96];
    char *end = 0;
    char *heap;
    uint8_t *zero;
    struct timespec now = {0};
    long parsed;

    if (argc >= 2 && argv != 0 && strcmp(argv[1], "--fd-check") == 0) {
        return test_exec_descriptor_child(argc, argv);
    }

    if (argc < 1 || argv == 0 || !test_strings()) {
        return libc_test_fail("strings");
    }
    if (!test_standard_utilities()) return libc_test_fail("utilities");
    if (!test_entropy()) return libc_test_fail("entropy");
    if (!test_environment_and_time()) return libc_test_fail("environment");
    if (!test_files()) return libc_test_fail("files");
    if (!test_pipes()) return libc_test_fail("pipes");
    if (!test_virtual_memory()) return libc_test_fail("virtual-memory");
    if (!test_network_addresses()) return libc_test_fail("network");
    if (!test_poll_and_name_resolution()) {
        return libc_test_fail("poll-name-resolution");
    }
    if (!test_command_line_and_glob()) return libc_test_fail("command-line");
    if (!test_extended_runtime()) return libc_test_fail("extended");
    if (!test_threads()) return libc_test_fail("threads");
    if (!test_c11_threads()) return libc_test_fail("c11-threads");
    if (!test_floating_point()) return libc_test_fail("floating-point");
    if (!test_floating_environment()) return libc_test_fail("floating-environment");
    if (!test_complex_and_unicode()) return libc_test_fail("complex-unicode");
    if (!test_signals()) return libc_test_fail("signals");
    if (!test_process_lifecycle() || !test_cloexec_exec()) {
        return libc_test_fail("process");
    }
    if (!test_blend2d()) return libc_test_fail("blend2d");
    printf("LITEOS_BLEND2D_TEST_OK\n");
    if (!liteos_blend2d_api_test()) return libc_test_fail("blend2d-api");
    printf("LITEOS_BLEND2D_API_TEST_OK\n");
    if (!test_blend2d_font_zoom()) {
        return libc_test_fail("blend2d-font-zoom");
    }
    printf("LITEOS_BLEND2D_FONT_ZOOM_OK\n");

    strcpy(source, "liteos-c-runtime");
    memcpy(copy, source, strlen(source) + 1U);
    memmove(copy + 1U, copy, 6U);
    parsed = strtol("0x2a", &end, 0);
    heap = (char *)malloc(64U);
    zero = (uint8_t *)calloc(8U, 8U);
    if (parsed != 42L || end == 0 || *end != '\0' ||
        strncmp(copy, "lliteos", 7U) != 0 || heap == 0 || zero == 0 ||
        clock_gettime(CLOCK_REALTIME, &now) != 0 ||
        snprintf(formatted, sizeof(formatted), "argc=%d value=%lld hex=%zx", argc,
                 (long long)now.tv_sec, (size_t)0x2aU) < 0 ||
        strstr(formatted, "argc=") == 0 || strstr(formatted, "hex=2a") == 0) {
        free(heap);
        free(zero);
        return libc_test_fail("startup");
    }
    free(heap);
    free(zero);
    printf("LITEOS_LIBC_TEST_OK %s\n", formatted);
    return 0;
}
