/* Deterministic first-runtime launcher for the Debian-built Chrome ELF. */

#include <libc/libc.h>
#include <fcntl.h>
#include <stddef.h>
#include <uapi/linux_abi.h>
#include <uapi/network.h>
#include <uapi/syscall.h>

#define CHROME_START_ARGUMENT_LIMIT 32U
#define CHROME_PROXY_ARGUMENT_LIMIT 256U

/* System networking can fail asynchronous speculative requests. Chromium's
 * prefetch response reader currently CHECK-aborts on that failure path, so
 * keep speculative prefetch/prerender disabled until its lifecycle is
 * supported and verified here. Omnibox search-engine prewarm also launches
 * speculative renderers while merely focusing the address field; avoid that
 * fork/VM-lock latency on the interactive path. The WebUI omnibox dropdown
 * independently creates WebContents (and spare renderers) on focus. Use the
 * existing native Views popup on System, retaining suggestions without a
 * renderer startup on each click. Normal navigation is unchanged. */
#define CHROME_DISABLED_FEATURES \
    "PrefetchUseContentRefactor,PrefetchPrerenderIntegration,Prewarm," \
    "WebUIOmniboxPopup,WebUIOmniboxFullPopup,WebUIOmniboxAimPopup"
#define CHROME_DISABLE_FEATURES_ARGUMENT \
    "--disable-features=" CHROME_DISABLED_FEATURES

/* QEMU's user-network host alias reaches the host's loopback proxy directly;
 * keep direct access as a fallback when the proxy is unavailable. */
#define CHROME_QEMU_DEFAULT_PROXY "http://10.0.2.2:7890,direct://"

static char *const g_default_arguments[] = {
    "/sbin/chrome",
    "--no-sandbox",
    /* This port runs in the QEMU test environment. Chromium's test mode
     * suppresses startup infobars without pretending the sandbox is enabled. */
    "--test-type",
    /* Route warnings/errors through the kernel's bounded Chrome stderr
     * capture so renderer/navigation failures are visible on the QEMU serial
     * log without enabling Chromium's high-volume verbose logging. */
    "--enable-logging=stderr",
    "--log-level=1",
    "--no-zygote",
    "--in-process-gpu",
    "--disable-dev-shm-usage",
    "--disable-background-networking",
    "--disable-component-update",
    "--disable-default-apps",
    "--disable-extensions",
    "--disable-sync",
    "--disable-breakpad",
    /* One-byte limits evicted every response while still paying SQLite
     * create/write/eviction costs. Keep useful, bounded session caches in
     * /tmp: the canonical 4 GiB guest can retain assets without an unbounded
     * default cache or speculative requests. This is not persistent storage
     * across a VM reboot. */
    "--disk-cache-size=67108864",
    "--media-cache-size=16777216",
    CHROME_DISABLE_FEATURES_ARGUMENT,
    "--no-first-run",
    "--no-default-browser-check",
    "--ozone-platform=nativeguest",
    "--profile-directory=Default",
    "--user-data-dir=/tmp/chrome-data",
    "https://www.google.com/",
};

static char *const g_software_arguments[] = {
    "--disable-gpu", "--disable-gpu-compositing", "--disable-gpu-program-cache",
};
static char *const g_vulkan_arguments[] = {
    "--enable-features=Vulkan", "--use-vulkan=native",
    "--disable-vulkan-surface", "--use-gl=disabled",
};

/* The stager publishes this only after the matching browser and resources.
 * Missing configuration preserves old software-only images; malformed/I/O
 * failures are explicit, not a silent claim of hardware acceleration. */
static int chrome_gpu_profile(void) {
    char buffer[17];
    size_t length = 0U;
    int descriptor = open("/etc/chrome-gpu-mode", O_RDONLY);
    if (descriptor < 0) return errno == ENOENT ? 0 : -1;
    while (length < sizeof(buffer) - 1U) {
        ssize_t count = read(descriptor, buffer + length, sizeof(buffer) - 1U - length);
        if (count < 0) {
            if (errno == EINTR) continue;
            (void)close(descriptor);
            return -1;
        }
        if (count == 0) break;
        length += (size_t)count;
    }
    (void)close(descriptor);
    if (length == 7U && memcmp(buffer, "vulkan\n", 7U) == 0) return 1;
    if (length == 9U && memcmp(buffer, "software\n", 9U) == 0) return 0;
    return -1;
}

static void chrome_start_debug_mark(const char *text, size_t length) {
    (void)__libc_linux_syscall6(OS_SYS_DEBUG_WRITE,
                                (uint64_t)(uintptr_t)text, length,
                                0U, 0U, 0U, 0U);
}

static bool chrome_has_explicit_proxy(int argc, char **argv) {
    static const char *const switches[] = {
        "--proxy-server=", "--proxy-pac-url=", "--no-proxy-server",
        "--proxy-auto-detect",
    };
    for (int index = 1; index < argc; ++index) {
        if (argv == 0 || argv[index] == 0) continue;
        for (size_t option = 0U;
             option < sizeof(switches) / sizeof(switches[0]); ++option) {
            size_t length = strlen(switches[option]);
            if (strncmp(argv[index], switches[option], length) == 0 &&
                (switches[option][length - 1U] == '=' ||
                 argv[index][length] == '\0')) return true;
        }
    }
    return false;
}

static const char *chrome_default_proxy(void) {
    os_net_status_t status = {0};
    status.hdr.size = sizeof(status);
    status.hdr.version = OS_SYSCALL_ABI_VERSION;
    if (__libc_native_syscall6(OS_SYS_NET_GET_STATUS,
            (uint64_t)(uintptr_t)&status, 0U, 0U, 0U, 0U, 0U) < 0)
        return "";
    /* The host alias exists only on the canonical QEMU user network. */
    if ((status.flags & OS_NET_STATUS_HARDWARE_PRESENT) != 0U &&
        status.mac[0] == 0x52U && status.mac[1] == 0x54U && status.mac[2] == 0U &&
        (status.ipv4_address == 0U ||
         (status.ipv4_address & 0xFFFFFF00U) == 0x0A000200U))
        return CHROME_QEMU_DEFAULT_PROXY;
    return "";
}

int main(int argc, char **argv) {
    char *chrome_arguments[CHROME_START_ARGUMENT_LIMIT];
    char proxy_argument[CHROME_PROXY_ARGUMENT_LIMIT];
    static const char proxy_prefix[] = "--proxy-server=";
    const char *proxy_server = getenv("OS_CHROME_PROXY_SERVER");
    size_t default_count = sizeof(g_default_arguments) /
                           sizeof(g_default_arguments[0]);
    size_t count = 0U;

    if (argc < 0 || (argc > 0 && argv == 0)) return 2;
    int gpu_profile = chrome_gpu_profile();
    if (gpu_profile < 0) {
        chrome_start_debug_mark("OS_CHROME_START_GPU_PROFILE_INVALID\n",
                               sizeof("OS_CHROME_START_GPU_PROFILE_INVALID\n") - 1U);
        return 2;
    }
    char *const *gpu_arguments = gpu_profile ? g_vulkan_arguments : g_software_arguments;
    size_t gpu_count = gpu_profile ?
        sizeof(g_vulkan_arguments) / sizeof(g_vulkan_arguments[0]) :
        sizeof(g_software_arguments) / sizeof(g_software_arguments[0]);
    /* A present-but-empty setting explicitly selects direct access. */

    chrome_start_debug_mark("OS_CHROME_START_MAIN\n",
                           sizeof("OS_CHROME_START_MAIN\n") - 1U);

    if (default_count + gpu_count + 2U >= CHROME_START_ARGUMENT_LIMIT) return 2;
    for (size_t index = 0U; index + 1U < default_count; ++index) {
        chrome_arguments[count++] = g_default_arguments[index];
    }
    for (size_t index = 0U; index < gpu_count; ++index) {
        chrome_arguments[count++] = gpu_arguments[index];
    }
    if (proxy_server == 0) proxy_server = chrome_default_proxy();
    if (!chrome_has_explicit_proxy(argc, argv) && proxy_server[0] != '\0') {
        size_t proxy_length = strlen(proxy_server);
        if (proxy_length > sizeof(proxy_argument) - sizeof(proxy_prefix))
            return 2;
        memcpy(proxy_argument, proxy_prefix, sizeof(proxy_prefix) - 1U);
        memcpy(proxy_argument + sizeof(proxy_prefix) - 1U, proxy_server,
               proxy_length + 1U);
        chrome_arguments[count++] = proxy_argument;
    }
    chrome_arguments[count++] = g_default_arguments[default_count - 1U];
    for (int index = 1; index < argc; ++index) {
        if (count + 1U >= CHROME_START_ARGUMENT_LIMIT) return 2;
        if (argv == 0 || argv[index] == 0) return 2;
        chrome_arguments[count++] = argv[index];
    }
    chrome_arguments[count] = 0;

    if (gpu_profile && setenv("VK_DRIVER_FILES",
            "/usr/share/vulkan/icd.d/nvidia_icd.json", 1) != 0) {
        chrome_start_debug_mark("OS_CHROME_START_GPU_ENV_FAILED\n",
                               sizeof("OS_CHROME_START_GPU_ENV_FAILED\n") - 1U);
        return 2;
    }
    chrome_start_debug_mark("OS_CHROME_START_BEFORE_EXEC\n",
                           sizeof("OS_CHROME_START_BEFORE_EXEC\n") - 1U);
    (void)execv(chrome_arguments[0], chrome_arguments);
    chrome_start_debug_mark("OS_CHROME_START_EXEC_RETURN\n",
                           sizeof("OS_CHROME_START_EXEC_RETURN\n") - 1U);
    return 127;
}
