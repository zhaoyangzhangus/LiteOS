#include <arch/x86_64/paging.h>
#include <kernel/display.h>
#include <kernel/pci.h>
#include <kernel/qemu_stdvga.h>

#define QEMU_STDVGA_VENDOR_ID 0x1234U
#define QEMU_STDVGA_DEVICE_ID 0x1111U

/*
 * Used only when the firmware GOP alias does not cover two complete frames.
 * Existing LiteOS MMIO windows are near the beginning of X86_64_MMIO_BASE;
 * keep this alias 4 GiB away from those fixed slots.
 */
#define QEMU_STDVGA_VRAM_VA \
    (X86_64_MMIO_BASE + 0x100000000ULL)

/* Legacy Bochs VBE index/data ports used by QEMU's primary PCI VGA device. */
#define QEMU_VBE_INDEX_PORT 0x01CEU
#define QEMU_VBE_DATA_PORT  0x01CFU

#define QEMU_VBE_INDEX_ID          0x00U
#define QEMU_VBE_INDEX_XRES        0x01U
#define QEMU_VBE_INDEX_YRES        0x02U
#define QEMU_VBE_INDEX_BPP         0x03U
#define QEMU_VBE_INDEX_ENABLE      0x04U
#define QEMU_VBE_INDEX_VIRT_WIDTH  0x06U
#define QEMU_VBE_INDEX_VIRT_HEIGHT 0x07U
#define QEMU_VBE_INDEX_X_OFFSET    0x08U
#define QEMU_VBE_INDEX_Y_OFFSET    0x09U

#define QEMU_VBE_ID0 0xB0C0U
#define QEMU_VBE_ID5 0xB0C5U
#define QEMU_VBE_ENABLED (1U << 0)

enum qemu_stdvga_error {
    QEMU_STDVGA_ERROR_NONE = 0U,
    QEMU_STDVGA_ERROR_NO_PCI_HOST = 1U,
    QEMU_STDVGA_ERROR_NO_DEVICE = 2U,
    QEMU_STDVGA_ERROR_DISPLAY = 3U,
    QEMU_STDVGA_ERROR_PIXEL_FORMAT = 4U,
    QEMU_STDVGA_ERROR_BAR0 = 5U,
    QEMU_STDVGA_ERROR_FRAMEBUFFER_BASE = 6U,
    QEMU_STDVGA_ERROR_VBE_ID = 7U,
    QEMU_STDVGA_ERROR_VBE_DISABLED = 8U,
    QEMU_STDVGA_ERROR_MODE = 9U,
    QEMU_STDVGA_ERROR_BPP = 10U,
    QEMU_STDVGA_ERROR_STRIDE = 11U,
    QEMU_STDVGA_ERROR_VRAM_SMALL = 12U,
    QEMU_STDVGA_ERROR_OFFSET = 13U,
    QEMU_STDVGA_ERROR_MAP = 14U,
    QEMU_STDVGA_ERROR_FLIP_VERIFY = 15U,
};

typedef struct qemu_stdvga_state {
    const pci_device_t *pci;
    volatile uint32_t *vram;

    uint64_t frame_bytes;
    uint64_t required_bytes;

    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t front_page;

    bool attempted;
    bool present;
    bool available;

    uint32_t last_error;
} qemu_stdvga_state_t;

static qemu_stdvga_state_t g_qemu_stdvga;

static inline void qemu_outw(uint16_t port, uint16_t value) {
    __asm__ volatile (
        "outw %0, %1"
        :
        : "a"(value), "Nd"(port));
}

static inline uint16_t qemu_inw(uint16_t port) {
    uint16_t value;

    __asm__ volatile (
        "inw %1, %0"
        : "=a"(value)
        : "Nd"(port));

    return value;
}

static uint16_t qemu_vbe_read(uint16_t index) {
    qemu_outw(QEMU_VBE_INDEX_PORT, index);
    return qemu_inw(QEMU_VBE_DATA_PORT);
}

static void qemu_vbe_write(uint16_t index, uint16_t value) {
    qemu_outw(QEMU_VBE_INDEX_PORT, index);
    qemu_outw(QEMU_VBE_DATA_PORT, value);
}

static const pci_device_t *qemu_stdvga_find(void) {
    const pci_host_t *host = pci_current_host();

    if (host == 0) return 0;

    return pci_find_device(
        host,
        QEMU_STDVGA_VENDOR_ID,
        QEMU_STDVGA_DEVICE_ID);
}

static bool qemu_stdvga_map_bar0(const pci_bar_t *bar,
                                 uint64_t required_bytes) {
    uint64_t span;
    uint64_t mapped = 0U;

    if (bar == 0 ||
        (bar->flags & PCI_RESOURCE_MEMORY) == 0U ||
        bar->address == 0U ||
        bar->length == 0U ||
        required_bytes == 0U ||
        required_bytes > bar->length ||
        required_bytes > UINT64_MAX - (PAGE_SIZE - 1ULL)) {
        return false;
    }

    span =
        (required_bytes + PAGE_SIZE - 1ULL) &
        ~(uint64_t)(PAGE_SIZE - 1ULL);

    if (span > bar->length ||
        span > X86_64_MMIO_END - QEMU_STDVGA_VRAM_VA + 1ULL) {
        return false;
    }

    while (mapped < span) {
        kstatus_t status =
            x86_map_page(
                x86_current_root_table(),
                (vaddr_t)(QEMU_STDVGA_VRAM_VA + mapped),
                paddr_make(bar->address + mapped),
                X86_PAGE_WRITE | X86_PAGE_GLOBAL,
                X86_CACHE_WC);

        if (status != K_OK) {
            while (mapped != 0U) {
                mapped -= PAGE_SIZE;
                (void)x86_unmap_page(
                    x86_current_root_table(),
                    (vaddr_t)(QEMU_STDVGA_VRAM_VA + mapped),
                    0);
            }
            return false;
        }

        mapped += PAGE_SIZE;
    }

    g_qemu_stdvga.vram =
        (volatile uint32_t *)(uintptr_t)QEMU_STDVGA_VRAM_VA;

    return true;
}

bool qemu_stdvga_hardware_present(void) {
    if (g_qemu_stdvga.attempted) {
        return g_qemu_stdvga.present;
    }
    return qemu_stdvga_find() != 0;
}

bool qemu_stdvga_flip_init(uint64_t framebuffer_physical,
                           uint64_t framebuffer_virtual,
                           uint64_t framebuffer_size) {
    const pci_host_t *host;
    const pci_device_t *pci;
    const pci_bar_t *bar;

    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;

    uint16_t id;
    uint16_t xres;
    uint16_t yres;
    uint16_t bpp;
    uint16_t enable;
    uint16_t virt_width;
    uint16_t virt_height;
    uint16_t x_offset;
    uint16_t y_offset;

    uint64_t frame_pixels;
    uint64_t frame_bytes;
    uint64_t required_bytes;

    if (g_qemu_stdvga.attempted) {
        return g_qemu_stdvga.available;
    }

    g_qemu_stdvga.attempted = true;
    g_qemu_stdvga.last_error = QEMU_STDVGA_ERROR_NONE;

    host = pci_current_host();
    if (host == 0) {
        g_qemu_stdvga.last_error = QEMU_STDVGA_ERROR_NO_PCI_HOST;
        return false;
    }

    pci = qemu_stdvga_find();
    if (pci == 0) {
        g_qemu_stdvga.last_error = QEMU_STDVGA_ERROR_NO_DEVICE;
        return false;
    }

    g_qemu_stdvga.present = true;
    g_qemu_stdvga.pci = pci;

    if (!display_core_query(0U, &width, &height, &stride, &format) ||
        width == 0U || height == 0U || stride < width) {
        g_qemu_stdvga.last_error = QEMU_STDVGA_ERROR_DISPLAY;
        return false;
    }

    /*
     * QEMU/Bochs x8r8g8b8 is little-endian B,G,R,X in memory, matching
     * UEFI PixelBlueGreenRedReserved8BitPerColor (LiteOS format 1).
     */
    if (format != 1U) {
        g_qemu_stdvga.last_error = QEMU_STDVGA_ERROR_PIXEL_FORMAT;
        return false;
    }

    bar = &pci->bars[0];
    if ((bar->flags & PCI_RESOURCE_MEMORY) == 0U ||
        bar->address == 0U || bar->length == 0U) {
        g_qemu_stdvga.last_error = QEMU_STDVGA_ERROR_BAR0;
        return false;
    }

    /* Y_OFFSET addresses BAR0 VRAM. Keep the proof strict for P14. */
    if (framebuffer_physical != bar->address) {
        g_qemu_stdvga.last_error = QEMU_STDVGA_ERROR_FRAMEBUFFER_BASE;
        return false;
    }

    id = qemu_vbe_read(QEMU_VBE_INDEX_ID);
    if (id < QEMU_VBE_ID0 || id > QEMU_VBE_ID5) {
        g_qemu_stdvga.last_error = QEMU_STDVGA_ERROR_VBE_ID;
        return false;
    }

    enable = qemu_vbe_read(QEMU_VBE_INDEX_ENABLE);
    if ((enable & QEMU_VBE_ENABLED) == 0U) {
        g_qemu_stdvga.last_error = QEMU_STDVGA_ERROR_VBE_DISABLED;
        return false;
    }

    xres = qemu_vbe_read(QEMU_VBE_INDEX_XRES);
    yres = qemu_vbe_read(QEMU_VBE_INDEX_YRES);
    bpp = qemu_vbe_read(QEMU_VBE_INDEX_BPP);
    virt_width = qemu_vbe_read(QEMU_VBE_INDEX_VIRT_WIDTH);
    virt_height = qemu_vbe_read(QEMU_VBE_INDEX_VIRT_HEIGHT);
    x_offset = qemu_vbe_read(QEMU_VBE_INDEX_X_OFFSET);
    y_offset = qemu_vbe_read(QEMU_VBE_INDEX_Y_OFFSET);

    if (xres != width || yres != height) {
        g_qemu_stdvga.last_error = QEMU_STDVGA_ERROR_MODE;
        return false;
    }

    if (bpp != 32U) {
        g_qemu_stdvga.last_error = QEMU_STDVGA_ERROR_BPP;
        return false;
    }

    if (virt_width != stride) {
        g_qemu_stdvga.last_error = QEMU_STDVGA_ERROR_STRIDE;
        return false;
    }

    if (height > UINT16_MAX / 2U || virt_height < height * 2U) {
        g_qemu_stdvga.last_error = QEMU_STDVGA_ERROR_VRAM_SMALL;
        return false;
    }

    if (x_offset != 0U ||
        (y_offset != 0U && y_offset != height)) {
        g_qemu_stdvga.last_error = QEMU_STDVGA_ERROR_OFFSET;
        return false;
    }

    frame_pixels = (uint64_t)stride * height;
    if (frame_pixels > UINT64_MAX / sizeof(uint32_t)) {
        g_qemu_stdvga.last_error = QEMU_STDVGA_ERROR_VRAM_SMALL;
        return false;
    }

    frame_bytes = frame_pixels * sizeof(uint32_t);
    if (frame_bytes == 0U || frame_bytes > UINT64_MAX / 2U) {
        g_qemu_stdvga.last_error = QEMU_STDVGA_ERROR_VRAM_SMALL;
        return false;
    }

    required_bytes = frame_bytes * 2U;
    if (required_bytes > bar->length) {
        g_qemu_stdvga.last_error = QEMU_STDVGA_ERROR_VRAM_SMALL;
        return false;
    }

    /*
     * OVMF may expose the complete BAR as FrameBufferSize. Reuse its WC alias
     * when possible; otherwise install a dedicated two-frame WC mapping.
     */
    if (framebuffer_virtual != 0U && framebuffer_size >= required_bytes) {
        g_qemu_stdvga.vram =
            (volatile uint32_t *)(uintptr_t)framebuffer_virtual;
    } else if (!qemu_stdvga_map_bar0(bar, required_bytes)) {
        g_qemu_stdvga.last_error = QEMU_STDVGA_ERROR_MAP;
        return false;
    }

    g_qemu_stdvga.frame_bytes = frame_bytes;
    g_qemu_stdvga.required_bytes = required_bytes;
    g_qemu_stdvga.width = width;
    g_qemu_stdvga.height = height;
    g_qemu_stdvga.stride = stride;
    g_qemu_stdvga.front_page = y_offset == 0U ? 0U : 1U;
    g_qemu_stdvga.available = true;
    return true;
}

bool qemu_stdvga_flip_available(void) {
    return g_qemu_stdvga.available;
}

static volatile uint32_t *qemu_stdvga_page(uint32_t page) {
    uint64_t offset_pixels;

    if (!g_qemu_stdvga.available ||
        g_qemu_stdvga.vram == 0 || page > 1U ||
        (g_qemu_stdvga.frame_bytes & (sizeof(uint32_t) - 1U)) != 0U) {
        return 0;
    }

    offset_pixels =
        ((uint64_t)page * g_qemu_stdvga.frame_bytes) /
        sizeof(uint32_t);

    return g_qemu_stdvga.vram + offset_pixels;
}

volatile uint32_t *qemu_stdvga_front_buffer(void) {
    return qemu_stdvga_page(g_qemu_stdvga.front_page);
}

volatile uint32_t *qemu_stdvga_back_buffer(void) {
    return qemu_stdvga_page(g_qemu_stdvga.front_page ^ 1U);
}

bool qemu_stdvga_flip(void) {
    uint32_t next_page;
    uint16_t next_y;
    uint16_t observed_y;

    if (!g_qemu_stdvga.available) return false;

    next_page = g_qemu_stdvga.front_page ^ 1U;
    next_y = next_page == 0U ? 0U : (uint16_t)g_qemu_stdvga.height;

    /* Hidden VRAM is WC: finish it before changing the scanout base. */
    __asm__ volatile ("sfence" : : : "memory");

    qemu_vbe_write(QEMU_VBE_INDEX_Y_OFFSET, next_y);
    observed_y = qemu_vbe_read(QEMU_VBE_INDEX_Y_OFFSET);

    if (observed_y != next_y) {
        g_qemu_stdvga.available = false;
        g_qemu_stdvga.last_error = QEMU_STDVGA_ERROR_FLIP_VERIFY;
        return false;
    }

    g_qemu_stdvga.front_page = next_page;
    return true;
}

uint32_t qemu_stdvga_last_error(void) {
    return g_qemu_stdvga.last_error;
}
