#include <kernel/display.h>
#include <kernel/kmem.h>
#include <kernel/perf.h>
#include <kernel/telemetry.h>
#include <kernel/vm.h>
#include "internal.h"

/* CPU-side retained-surface composition leaves. */

/*
 * Fast WB -> WB pixel copy.
 *
 * Source is the shared window backing page through the physical direct map.
 * Destination is the normal-RAM composite framebuffer.
 *
 * REP MOVSQ uses only integer registers and is well suited to contiguous
 * scanline spans.  No kernel SIMD/FPU state is touched.
 */
void compositor_copy_wb_pixels(
    uint32_t *destination,
    const uint32_t *source,
    uint32_t pixels) {

    uint64_t qwords;

    if (destination == 0 ||
        source == 0 ||
        pixels == 0U) {
        return;
    }

    qwords = pixels >> 1U;

    if (qwords != 0U) {
        void *out =
            (void *)destination;

        const void *in =
            (const void *)source;

        uint64_t count = qwords;

        __asm__ volatile (
            "rep movsq"
            : "+D"(out),
              "+S"(in),
              "+c"(count)
            :
            : "memory");
    }

    if ((pixels & 1U) != 0U) {
        destination[pixels - 1U] =
            source[pixels - 1U];
    }
}

bool compositor_copy_self_test(void) {
    uint64_t pixel_count64 =
        (uint64_t)g_window_server.display_width *
        g_window_server.display_height;
    uint32_t pixel_count;
    size_t bytes;
    uint32_t *source;
    uint32_t *destination;
    bool success = false;

    if (pixel_count64 == 0U || pixel_count64 > UINT32_MAX ||
        pixel_count64 > (uint64_t)SIZE_MAX / sizeof(uint32_t)) {
        return false;
    }

    pixel_count = (uint32_t)pixel_count64;
    bytes = (size_t)pixel_count * sizeof(uint32_t);
    source = (uint32_t *)kzalloc(bytes, 0);
    destination = (uint32_t *)kzalloc(bytes, 0);
    if (source == 0 || destination == 0) {
        if (source != 0) kfree(source);
        if (destination != 0) kfree(destination);
        return false;
    }

    for (uint32_t index = 0U; index < pixel_count; ++index) {
        source[index] = 0x00100000U | (index & 0x0000FFFFU);
    }

    uint64_t benchmark_start = telemetry_timestamp();
    compositor_copy_wb_pixels(destination, source, pixel_count);
    kernel_perf_emit_scope("graphics.fullscreen_copy", benchmark_start);
    kernel_perf_emit_value(
        "graphics.bytes_copied_frame",
        (uint64_t)pixel_count * sizeof(uint32_t));

    success = destination[0] == source[0] &&
              destination[pixel_count / 2U] == source[pixel_count / 2U] &&
              destination[pixel_count - 1U] == source[pixel_count - 1U];
    kfree(destination);
    kfree(source);
    return success;
}


/*
 * Private retained-scene fill.  The global composite pointer remains volatile
 * for the GOP alias fallback; callers enter this helper only after proving the
 * target is ordinary cacheable WB RAM.
 */
void compositor_fill_wb_pixels(
    uint32_t *destination,
    uint32_t pixels,
    uint32_t color) {

    uint64_t pair;
    uint64_t qwords;
    void *out;

    if (destination == 0 || pixels == 0U) {
        return;
    }

    if ((((uintptr_t)destination) & 7U) != 0U) {
        *destination++ = color;
        --pixels;
    }

    pair = (uint64_t)color | ((uint64_t)color << 32U);
    qwords = pixels >> 1U;
    out = (void *)destination;

    if (qwords != 0U) {
        uint64_t count = qwords;
        __asm__ volatile (
            "rep stosq"
            : "+D"(out), "+c"(count)
            : "a"(pair)
            : "memory");
    }

    if ((pixels & 1U) != 0U) {
        *(uint32_t *)out = color;
    }
}


/*
 * Fast background fill for a clipped surface span.
 *
 * This is mainly used during live resize or if a shared backing page cannot
 * be resolved.  Pack two XRGB pixels into one 64-bit store.
 */
void compositor_fill_surface_pixels(
    volatile uint32_t *destination,
    uint32_t pixels,
    uint32_t color) {

    uint64_t pair;

    if (destination == 0 || pixels == 0U) {
        return;
    }

    /*
     * Align destination before using uint64_t stores.
     */
    if ((((uintptr_t)destination) & 7U) != 0U) {
        *destination++ = color;
        --pixels;
    }

    pair =
        (uint64_t)color |
        ((uint64_t)color << 32U);

    while (pixels >= 8U) {
        volatile uint64_t *out =
            (volatile uint64_t *)(void *)destination;

        out[0] = pair;
        out[1] = pair;
        out[2] = pair;
        out[3] = pair;

        destination += 8U;
        pixels -= 8U;
    }

    while (pixels >= 2U) {
        *(volatile uint64_t *)(void *)destination =
            pair;

        destination += 2U;
        pixels -= 2U;
    }

    if (pixels != 0U) {
        *destination = color;
    }
}


/*
 * Copy one horizontal window-surface span.
 *
 * The span is split only when it crosses a 4K VM_OBJECT_SHARED backing page.
 * Therefore page lookup happens roughly once per 1024 XRGB8888 pixels rather
 * than once per pixel.
 *
 * cached_page/cached_base persist across scanlines, so a page shared by the
 * tail of one row and the beginning of the next does not need to be resolved
 * twice.
 */

/*
 * Persistent direct-page table for a window surface.
 *
 * VM_OBJECT_SHARED pages do not move while their object remains alive.
 * The compositor snapshot holds a reference to the live window, therefore
 * positive direct mappings may safely be reused across compositor frames.
 *
 * Missing pages are NEVER cached.  If Ring3 faults a new page later, the next
 * compositor lookup will see an empty slot and resolve it normally.
 */
struct compositor_surface_page_cache {
    shared_section_t *section;
    uint32_t page_count;
    uint32_t reserved;

    /*
     * Flexible array:
     *
     *     pages[page_index] -> kernel direct-map address
     *
     * NULL means "not resolved yet", not "page permanently absent".
     */
    uint8_t *pages[];
};


/*
 * Return/create the per-window direct-page table.
 *
 * Allocation failure is harmless: the compositor simply falls back to the
 * normal VM lookup path.
 */
compositor_surface_page_cache_t *
compositor_surface_cache_get(
    const compositor_window_view_t *view) {

    window_server_window_t *reference;
    compositor_surface_page_cache_t *cache;
    uint64_t page_count64;
    size_t bytes;

    if (view == 0 ||
        view->reference == 0 ||
        view->section == 0 ||
        view->buffer_size == 0U) {
        return 0;
    }

    reference = view->reference;

    /*
     * Existing window section means the direct-page table remains valid.
     * Avoid recomputing page_count on the normal path.
     */
    cache =
        (compositor_surface_page_cache_t *)
            reference->compositor_cache;

    if (cache != 0 &&
        cache->section == view->section) {
        return cache;
    }

    page_count64 =
        (view->buffer_size + PAGE_SIZE - 1U) >>
        PAGE_SHIFT;

    if (page_count64 == 0U ||
        page_count64 > UINT32_MAX) {
        return 0;
    }

    /*
     * The current implementation keeps a stable section for a window.
     * Still validate it here so a future section replacement cannot expose
     * stale direct pointers.
     */
    if (cache != 0 &&
        cache->section == view->section &&
        cache->page_count == (uint32_t)page_count64) {
        return cache;
    }

    if (cache != 0) {
        reference->compositor_cache = 0;
        kfree(cache);
        cache = 0;
    }

    /*
     * page_count is bounded to uint32_t and the kernel is 64-bit, so this
     * multiplication is safely representable as size_t.
     */
    bytes =
        sizeof(compositor_surface_page_cache_t) +
        (size_t)page_count64 * sizeof(uint8_t *);

    cache =
        (compositor_surface_page_cache_t *)
            kzalloc(bytes, 0);

    if (cache == 0) {
        return 0;
    }

    cache->section =
        view->section;

    cache->page_count =
        (uint32_t)page_count64;

    cache->reserved = 0U;

    reference->compositor_cache =
        cache;

    return cache;
}


/*
 * Cached wrapper around vm_object_shared_page_direct().
 *
 * Fast hit:
 *
 *     page_index -> pointer array -> direct address
 *
 * No VM lock, no backing-page list traversal.
 */

/*
 * Sequential surface rendering normally crosses shared pages in ascending
 * order.  Once the next page already has a positive direct-page cache entry,
 * pull its first cache line toward L1 before the current page is exhausted.
 *
 * This is only a performance hint. Missing pages are never resolved or
 * allocated here.
 */
static inline void compositor_prefetch_surface_page(
    const compositor_surface_page_cache_t *cache,
    uint64_t page_index) {

    uint8_t *next;

    if (cache == 0 ||
        page_index + 1U >= cache->page_count) {
        return;
    }

    next =
        cache->pages[page_index + 1U];

    if (next == 0) {
        return;
    }

    __asm__ volatile (
        "prefetcht0 (%0)"
        :
        : "r"(next));
}


/*
 * Resolve row-invariant surface state once per window/damage pass.
 * This matters most for narrow retained-drag strips: successive rows can touch
 * different 4K pages even though only one or a few pixels are copied.
 */


/*
 * Positive cache hit:
 *     cache->pages[page_index]
 * No per-page reference->compositor_cache reload or section comparison.
 */
uint8_t *compositor_surface_page_resolve(
    const compositor_surface_source_context_t *context,
    uint64_t page_index) {

    compositor_surface_page_cache_t *cache;
    uint8_t *base = 0;

    if (context == 0 || !context->readable || context->object == 0) {
        return 0;
    }

    cache = context->cache;

    if (cache != 0 && page_index < cache->page_count) {
        base = cache->pages[page_index];
        if (base != 0) {
            compositor_prefetch_surface_page(cache, page_index);
            return base;
        }
    }

    if (page_index > (UINT64_MAX >> PAGE_SHIFT)) return 0;

    if (vm_object_shared_page_direct(
            context->object,
            page_index << PAGE_SHIFT,
            true,
            &base) != K_OK ||
        base == 0) {
        return 0;
    }

    if (cache != 0 && page_index < cache->page_count) {
        cache->pages[page_index] = base;
        compositor_prefetch_surface_page(cache, page_index);
    }

    return base;
}


void compositor_surface_fill_destination(
    uint32_t *wb_destination,
    volatile uint32_t *device_destination,
    uint32_t pixels,
    uint32_t color) {

    if (wb_destination != 0) {
        compositor_fill_wb_pixels(wb_destination, pixels, color);
    } else {
        compositor_fill_surface_pixels(device_destination, pixels, color);
    }
}


void compositor_surface_copy_destination(
    uint32_t *wb_destination,
    volatile uint32_t *device_destination,
    const uint32_t *source,
    uint32_t pixels) {

    if (wb_destination != 0) {
        compositor_copy_wb_pixels(wb_destination, source, pixels);
    } else {
        display_core_publish_xrgb8888_span(
            device_destination, source, pixels);
    }
}


/*
 * source_row_offset is maintained incrementally by the caller.  The normal
 * hot path now does only page-index arithmetic + direct page-array lookup.
 */

