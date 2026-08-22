#!/usr/bin/env python3
from __future__ import annotations

import argparse
import difflib
import hashlib
from pathlib import Path
import shutil
import sys

FILES = {
    Path("kernel/graphics/window_server.c"):
        "2a85fc241e991ab5afd6133bf13728d135ebed5b",
    Path("kernel/core/display.c"):
        "8b13d71bb3b4b013d5ad253ff8608270193a1ce0",
    Path("OS_Implementation_Specification_COMPLETE/include/kernel/display.h"):
        "54ebd9c9f44dcddb28687dfce8ef5a88f9c8cc1d",
}

BACKUP_SUFFIX = ".before-compositor-p6"


def git_blob_sha(data: bytes) -> str:
    h = hashlib.sha1()
    h.update(f"blob {len(data)}\0".encode("ascii"))
    h.update(data)
    return h.hexdigest()


def replace_once(text: str, old: str, new: str, name: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(
            f"{name}: expected exactly one match, found {count}"
        )
    return text.replace(old, new, 1)


def remove_between(text: str, start: str, end: str, replacement: str,
                   name: str) -> str:
    a = text.find(start)
    if a < 0:
        raise RuntimeError(f"{name}: start marker not found")
    b = text.find(end, a)
    if b < 0:
        raise RuntimeError(f"{name}: end marker not found")
    if text.find(start, a + 1) >= 0:
        raise RuntimeError(f"{name}: start marker is not unique")
    return text[:a] + replacement + text[b:]


DISPLAY_FAST_PATH = r'''
/*
 * GOP WB -> WC publication primitive shared by every kernel display producer.
 *
 * The compositor used to own a private copy of this code.  Keeping the actual
 * device-memory writer in display core has two advantages:
 *
 *   1. window_server and DISPLAY_COMMIT use exactly the same tuned path;
 *   2. a future native scanout/page-flip backend has one display-layer point
 *      to replace instead of architecture-specific stores embedded in the
 *      compositor.
 *
 * XRGB8888 source pixels are native when the GOP framebuffer format is 1.
 * Other GOP layouts preserve the existing conversion path.
 */
#ifndef DISPLAY_GOP_MOVDIR64B
#define DISPLAY_GOP_MOVDIR64B 1U
#endif

static inline void display_wc_store64(volatile uint64_t *destination,
                                      uint64_t value) {
    __asm__ volatile (
        "movnti %1, %0"
        : "=m"(*destination)
        : "r"(value));
}

static inline uint64_t display_wb_load64(const uint32_t *source) {
    uint64_t value;

    __asm__ (
        "movq %1, %0"
        : "=r"(value)
        : "m"(*(const uint64_t *)(const void *)source));

    return value;
}

static inline void display_wc_store64b_direct(
    volatile uint32_t *destination,
    const uint32_t *source) {

    __asm__ volatile (
        "movdir64b (%1), %0"
        :
        : "r"((uintptr_t)destination),
          "r"(source)
        : "memory");
}

static void display_copy_native_xrgb8888_span(
    volatile uint32_t *__restrict destination,
    const uint32_t *__restrict source,
    uint32_t pixels) {

    if (destination == 0 || source == 0 || pixels == 0U) {
        return;
    }

    /*
     * Reach qword alignment first.  Both buffers advance together.
     */
    if (((uintptr_t)destination & 7U) != 0U) {
        *destination++ = *source++;
        --pixels;
    }

    /*
     * MOVDIR64B requires a 64-byte-aligned destination.  The short prefix is
     * still non-temporal so the WC path does not mix cached stores into the
     * bulk transaction.
     */
    while (pixels >= 2U &&
           ((uintptr_t)destination & 63U) != 0U) {

        uint64_t value = display_wb_load64(source);

        display_wc_store64(
            (volatile uint64_t *)(void *)destination,
            value);

        destination += 2U;
        source += 2U;
        pixels -= 2U;
    }

    if (DISPLAY_GOP_MOVDIR64B != 0U &&
        x86_boot_cpu_features.movdir64b) {

        while (pixels >= 16U) {
            display_wc_store64b_direct(destination, source);

            destination += 16U;
            source += 16U;
            pixels -= 16U;
        }
    }

    /*
     * Generic 64-byte fallback: 16 XRGB pixels = eight MOVNTI qwords.
     */
    while (pixels >= 16U) {
        uint64_t v0 = display_wb_load64(source + 0U);
        uint64_t v1 = display_wb_load64(source + 2U);
        uint64_t v2 = display_wb_load64(source + 4U);
        uint64_t v3 = display_wb_load64(source + 6U);
        uint64_t v4 = display_wb_load64(source + 8U);
        uint64_t v5 = display_wb_load64(source + 10U);
        uint64_t v6 = display_wb_load64(source + 12U);
        uint64_t v7 = display_wb_load64(source + 14U);

        volatile uint64_t *out =
            (volatile uint64_t *)(void *)destination;

        display_wc_store64(out + 0U, v0);
        display_wc_store64(out + 1U, v1);
        display_wc_store64(out + 2U, v2);
        display_wc_store64(out + 3U, v3);
        display_wc_store64(out + 4U, v4);
        display_wc_store64(out + 5U, v5);
        display_wc_store64(out + 6U, v6);
        display_wc_store64(out + 7U, v7);

        destination += 16U;
        source += 16U;
        pixels -= 16U;
    }

    if (pixels >= 8U) {
        uint64_t v0 = display_wb_load64(source + 0U);
        uint64_t v1 = display_wb_load64(source + 2U);
        uint64_t v2 = display_wb_load64(source + 4U);
        uint64_t v3 = display_wb_load64(source + 6U);

        volatile uint64_t *out =
            (volatile uint64_t *)(void *)destination;

        display_wc_store64(out + 0U, v0);
        display_wc_store64(out + 1U, v1);
        display_wc_store64(out + 2U, v2);
        display_wc_store64(out + 3U, v3);

        destination += 8U;
        source += 8U;
        pixels -= 8U;
    }

    while (pixels >= 2U) {
        uint64_t value = display_wb_load64(source);

        display_wc_store64(
            (volatile uint64_t *)(void *)destination,
            value);

        destination += 2U;
        source += 2U;
        pixels -= 2U;
    }

    if (pixels != 0U) {
        *destination = *source;
    }
}

void display_core_publish_xrgb8888_span(
    volatile uint32_t *destination,
    const uint32_t *source,
    uint32_t pixels) {

    if (destination == 0 || source == 0 || pixels == 0U) {
        return;
    }

    /*
     * The framebuffer geometry/format is immutable after display_core_init()
     * in the current boot model.  No display lock is taken here: compositor
     * copy workers deliberately publish disjoint spans in parallel.
     */
    if (g_display.format == 1U) {
        display_copy_native_xrgb8888_span(
            destination,
            source,
            pixels);
        return;
    }

    for (uint32_t x = 0U; x < pixels; ++x) {
        destination[x] =
            display_convert_pixel(
                source[x],
                g_display.format,
                g_display.masks);
    }
}

'''


def transform_display(text: str) -> str:
    text = replace_once(
        text,
        '#include <kernel/display.h>\n',
        '#include <kernel/display.h>\n#include <arch/x86_64/cpu.h>\n',
        "display: x86 feature include",
    )

    marker = "static bool display_commit_shape_valid("
    if text.count(marker) != 1:
        raise RuntimeError(
            "display: display_commit_shape_valid marker is not unique"
        )
    text = text.replace(
        marker,
        DISPLAY_FAST_PATH + marker,
        1,
    )

    old = '''        for (uint32_t y = first_row; y < last_row; ++y) {
            const uint32_t *row = (const uint32_t *)(const void *)
                (source + (uint64_t)y * pending->stride);
            for (uint32_t x = 0U; x < pending->width; ++x) {
                destination[(uint64_t)y * g_display.pixels_per_scanline + x] =
                    display_convert_pixel(row[x], g_display.format, g_display.masks);
            }
        }
        copied = true;'''

    new = '''        for (uint32_t y = first_row; y < last_row; ++y) {
            const uint32_t *row =
                (const uint32_t *)(const void *)(
                    source + (uint64_t)y * pending->stride);

            volatile uint32_t *out =
                destination +
                (uint64_t)y *
                    g_display.pixels_per_scanline;

            display_core_publish_xrgb8888_span(
                out,
                row,
                pending->width);
        }

        /*
         * A deferred continuation is allowed to run on another CPU.  MOVNTI /
         * MOVDIR64B ordering is CPU-local, so finish this chunk's WC stores
         * before dropping the display lock and scheduling the next chunk.
         */
        __asm__ volatile ("sfence" : : : "memory");

        copied = true;'''

    return replace_once(
        text,
        old,
        new,
        "display: async commit fast publication",
    )


def transform_header(text: str) -> str:
    marker = '''bool display_core_query(uint32_t output, uint32_t *width, uint32_t *height,
                        uint32_t *stride, uint32_t *format);

'''
    addition = marker + '''/*
 * Kernel compositor fast path.
 *
 * Source is XRGB8888 in ordinary WB RAM. Destination points into the current
 * scanout mapping.  The function does not fence: callers batch spans and issue
 * SFENCE at the transaction/chunk boundary.
 */
void display_core_publish_xrgb8888_span(volatile uint32_t *destination,
                                        const uint32_t *source,
                                        uint32_t pixels);

'''
    return replace_once(
        text,
        marker,
        addition,
        "display header: publish span declaration",
    )


def transform_window(text: str) -> str:
    config = '''/*
 * MOVDIR64B is ideal for the aligned 64-byte body of WB -> device-memory
 * publication: one instruction transfers one complete cache line without
 * touching XMM/YMM state.  Runtime CPUID still gates every use.
 */
#ifndef WINDOW_COMPOSITOR_MOVDIR64B
#define WINDOW_COMPOSITOR_MOVDIR64B 1U
#endif

'''
    if text.count(config) != 1:
        raise RuntimeError(
            "window: P4 MOVDIR64B config block not found exactly once"
        )
    text = text.replace(config, "", 1)

    start = '''/*
 * Copy one XRGB8888 scanline from normal WB RAM into the GOP WC mapping.
'''
    end = "typedef struct compositor_copy_wait_context"

    replacement = '''/*
 * Device-memory publication belongs to display core.
 *
 * P5 still owns damage selection, row partitioning and transaction policy;
 * this leaf call only performs the architecture/output-specific WB -> scanout
 * span write.  That keeps the compositor independent from MOVDIR64B/MOVNTI
 * details and gives DISPLAY_COMMIT the same optimized GOP path.
 */
static inline void compositor_copy_wc_scanline(
    volatile uint32_t *__restrict destination,
    const uint32_t *__restrict source,
    uint32_t pixels) {

    display_core_publish_xrgb8888_span(
        destination,
        source,
        pixels);
}

'''
    return remove_between(
        text,
        start,
        end,
        replacement,
        "window: extract GOP publication primitive",
    )


TRANSFORMS = {
    Path("kernel/core/display.c"): transform_display,
    Path("OS_Implementation_Specification_COMPLETE/include/kernel/display.h"):
        transform_header,
    Path("kernel/graphics/window_server.c"): transform_window,
}


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "LiteOS compositor P6: consolidate GOP publication in display core"
        )
    )
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--force-context", action="store_true")
    parser.add_argument("--emit-patch", metavar="PATH")
    args = parser.parse_args()

    originals: dict[Path, str] = {}
    modified: dict[Path, str] = {}

    for path, expected_sha in FILES.items():
        if not path.exists():
            print(f"error: missing {path}; run from LiteOS repo root",
                  file=sys.stderr)
            return 2

        data = path.read_bytes()
        sha = git_blob_sha(data)

        if sha != expected_sha and not args.force_context:
            print(
                f"error: P6 base mismatch: {path}\n"
                f"  expected: {expected_sha}\n"
                f"  current : {sha}",
                file=sys.stderr,
            )
            return 3

        originals[path] = data.decode("utf-8")

    try:
        for path, source in originals.items():
            modified[path] = TRANSFORMS[path](source)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 4

    if args.check:
        print("OK: P6 base/context verified")
        for path, expected_sha in FILES.items():
            print(f"{expected_sha}  {path}")
        print("P6: GOP WB->WC writer moves to display core")
        print("P6: async DISPLAY_COMMIT reuses MOVDIR64B/MOVNTI fast path")
        print("P6: compositor P5 damage/parallel policy remains unchanged")
        return 0

    patches: list[str] = []
    for path in FILES:
        patches.append(
            "".join(
                difflib.unified_diff(
                    originals[path].splitlines(True),
                    modified[path].splitlines(True),
                    fromfile=f"a/{path.as_posix()}",
                    tofile=f"b/{path.as_posix()}",
                    n=5,
                )
            )
        )

    patch_text = "".join(patches)

    if args.emit_patch:
        out = Path(args.emit_patch)
        out.write_text(patch_text, encoding="utf-8")
        print(f"wrote: {out}")
        return 0

    for path in FILES:
        backup = Path(str(path) + BACKUP_SUFFIX)
        if not backup.exists():
            shutil.copy2(path, backup)
            print(f"backup: {backup}")

        path.write_text(modified[path], encoding="utf-8", newline="")
        print(f"patched: {path}")

    print("next: git diff --check")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
