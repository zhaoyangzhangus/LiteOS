#!/usr/bin/env python3
from __future__ import annotations

import argparse
import difflib
import hashlib
from pathlib import Path
import shutil
import sys

TARGET = Path("kernel/graphics/window_server.c")
BASE_SHA = "722f147920039fc4722e9053ad3a4ee9f7af6a9e"
BACKUP = Path("kernel/graphics/window_server.c.before-compositor-p5")


def git_blob_sha(data: bytes) -> str:
    h = hashlib.sha1()
    h.update(f"blob {len(data)}\0".encode("ascii"))
    h.update(data)
    return h.hexdigest()


def replace_once(text: str, old: str, new: str, name: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(
            f"{name}: expected exactly one match, found {count}. "
            "The local source differs from the P5 base."
        )
    return text.replace(old, new, 1)


def transform(text: str) -> str:
    # P5.1: Large ordinary repaint may use the already-existing persistent
    # publication workers. Keep drag single-writer by default.
    text = replace_once(
        text,
        '''#ifndef WINDOW_COMPOSITOR_MOVDIR64B
#define WINDOW_COMPOSITOR_MOVDIR64B 1U
#endif

/*
 * Correctness baseline for GOP:''',
        '''#ifndef WINDOW_COMPOSITOR_MOVDIR64B
#define WINDOW_COMPOSITOR_MOVDIR64B 1U
#endif

/*
 * Ordinary client/full-screen repaints have no move-transaction atomicity
 * requirement.  For one sufficiently large rectangle, let the compositor and
 * its two persistent helpers publish disjoint contiguous row ranges.
 *
 * 1M pixels = 4 MiB at XRGB8888.  Below this, worker wake/completion overhead
 * is usually more expensive than the extra single-CPU publication time.
 */
#ifndef WINDOW_COMPOSITOR_PARALLEL_ORDINARY
#define WINDOW_COMPOSITOR_PARALLEL_ORDINARY 1U
#endif

#ifndef WINDOW_COMPOSITOR_PARALLEL_ORDINARY_PIXELS
#define WINDOW_COMPOSITOR_PARALLEL_ORDINARY_PIXELS (1024U * 1024U)
#endif

/*
 * After participant zero finishes its own row slice, helpers are normally only
 * a few microseconds behind.  Briefly spin before entering the scheduler so a
 * nearly-complete copy does not pay a full context switch.
 */
#ifndef WINDOW_COMPOSITOR_COPY_COMPLETION_SPINS
#define WINDOW_COMPOSITOR_COPY_COMPLETION_SPINS 1024U
#endif

/*
 * Correctness baseline for GOP:''',
        "P5 adaptive ordinary publication config",
    )

    # P5.2: completion hybrid wait.
    text = replace_once(
        text,
        '''    uint32_t participants;
    uint64_t generation;
    uint64_t pixels;''',
        '''    uint32_t participants;
    uint32_t completion_spins = 0U;
    uint64_t generation;
    uint64_t pixels;''',
        "P5 completion spin counter",
    )

    text = replace_once(
        text,
        '''    /* The compositor owns participant zero and handles every Nth row while
     * the helpers handle the remaining rows. */
    compositor_copy_rows(&g_compositor_copy_job, 0U, participants);
    __asm__ volatile ("sfence" : : : "memory");

    while (atomic_load_explicit(&g_compositor_copy_completed,
                                memory_order_acquire) < worker_count) {
        /* Give the scheduler a chance to run local work while remote CPUs
         * drain the rows.  No framebuffer lock is held here. */
        schedule();
    }''',
        '''    /*
     * Participant zero owns the first contiguous row slice; helper CPUs own
     * the following disjoint slices.
     */
    compositor_copy_rows(&g_compositor_copy_job, 0U, participants);
    __asm__ volatile ("sfence" : : : "memory");

    while (atomic_load_explicit(&g_compositor_copy_completed,
                                memory_order_acquire) < worker_count) {
        /*
         * In the normal case remote helpers started at the same generation and
         * are only slightly behind participant zero.  A short PAUSE phase is
         * cheaper than immediately descheduling the compositor.
         *
         * If a helper really is delayed, periodically yield so this never
         * becomes an unbounded busy wait.
         */
        if (completion_spins <
            WINDOW_COMPOSITOR_COPY_COMPLETION_SPINS) {
            ++completion_spins;
            __asm__ volatile ("pause");
            continue;
        }

        completion_spins = 0U;
        schedule();
    }''',
        "P5 hybrid helper completion wait",
    )

    # P5.3: ordinary single-large-rect parallel fast path before preempt disable.
    marker = '''    allow_yield = !commit_as_move_transaction &&
                  compositor_damage_pixels() >
                  WINDOW_COMPOSITOR_ATOMIC_PIXELS;

    /* Drag/resize frames are already collapsed to one complete rectangle.'''

    replacement = '''    allow_yield = !commit_as_move_transaction &&
                  compositor_damage_pixels() >
                  WINDOW_COMPOSITOR_ATOMIC_PIXELS;

    /*
     * Large ordinary single-rectangle publication.
     *
     * This is deliberately evaluated before sched_preempt_disable(): the
     * persistent helper path may sleep/yield while waiting for remote CPUs and
     * therefore requires preemption to remain enabled.
     *
     * Keep fragmented damage on the serial path. Waking helpers once per tiny
     * rectangle would destroy the benefit of the spatial damage system.
     */
    if (WINDOW_COMPOSITOR_PARALLEL_ORDINARY != 0U &&
        !commit_as_move_transaction &&
        g_compositor_snapshot.damage_count == 1U) {

        const window_damage_rect_t *ordinary =
            &g_compositor_snapshot.damage_rects[0];

        if (ordinary->left < ordinary->right &&
            ordinary->top < ordinary->bottom) {

            uint64_t ordinary_pixels =
                (uint64_t)(ordinary->right - ordinary->left) *
                (uint64_t)(ordinary->bottom - ordinary->top);

            if (ordinary_pixels >
                    WINDOW_COMPOSITOR_PARALLEL_ORDINARY_PIXELS &&
                compositor_copy_rect_parallel(ordinary)) {
                return;
            }
        }
    }

    /* Drag/resize frames are already collapsed to one complete rectangle.'''

    text = replace_once(
        text,
        marker,
        replacement,
        "P5 ordinary parallel commit fast path",
    )

    return text


def disable_ordinary(text: str) -> str:
    return replace_once(
        text,
        "#define WINDOW_COMPOSITOR_PARALLEL_ORDINARY 1U",
        "#define WINDOW_COMPOSITOR_PARALLEL_ORDINARY 0U",
        "P5 ordinary parallel A/B switch",
    )


def disable_spin(text: str) -> str:
    return replace_once(
        text,
        "#define WINDOW_COMPOSITOR_COPY_COMPLETION_SPINS 1024U",
        "#define WINDOW_COMPOSITOR_COPY_COMPLETION_SPINS 0U",
        "P5 completion spin A/B switch",
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="LiteOS compositor P5: adaptive parallel ordinary GOP publication"
    )
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--force-context", action="store_true")
    parser.add_argument("--disable-parallel-ordinary", action="store_true")
    parser.add_argument("--disable-spin-wait", action="store_true")
    parser.add_argument("--emit-patch", metavar="PATH")
    args = parser.parse_args()

    if not TARGET.exists():
        print(f"error: run from LiteOS repository root; missing {TARGET}",
              file=sys.stderr)
        return 2

    data = TARGET.read_bytes()
    sha = git_blob_sha(data)
    original = data.decode("utf-8")

    if sha != BASE_SHA and not args.force_context:
        print(
            "error: P5 base blob mismatch\n"
            f"  expected: {BASE_SHA}\n"
            f"  current : {sha}\n"
            "Use current main after P4, or --force-context only after reviewing "
            "the resulting diff.",
            file=sys.stderr,
        )
        return 3

    try:
        modified = transform(original)

        if args.disable_parallel_ordinary:
            modified = disable_ordinary(modified)

        if args.disable_spin_wait:
            modified = disable_spin(modified)

    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 4

    if args.check:
        print("OK: P5 base/context verified")
        print(f"blob: {sha}")
        print("ordinary parallel threshold: 1,048,576 pixels")
        print("completion spin budget: 1024 PAUSE iterations")
        if args.disable_parallel_ordinary:
            print("ordinary parallel publication: forced OFF")
        if args.disable_spin_wait:
            print("completion spin wait: forced OFF")
        return 0

    diff = "".join(
        difflib.unified_diff(
            original.splitlines(True),
            modified.splitlines(True),
            fromfile="a/kernel/graphics/window_server.c",
            tofile="b/kernel/graphics/window_server.c",
            n=5,
        )
    )

    if args.emit_patch:
        out = Path(args.emit_patch)
        out.write_text(diff, encoding="utf-8")
        print(f"wrote: {out}")
        return 0

    if not BACKUP.exists():
        shutil.copy2(TARGET, BACKUP)
        print(f"backup: {BACKUP}")

    TARGET.write_text(modified, encoding="utf-8", newline="")
    print(f"patched: {TARGET}")
    print("next: git diff --check")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
