#!/usr/bin/env python3
from __future__ import annotations

import argparse
import difflib
import hashlib
from pathlib import Path
import shutil
import sys

FILES = {
    Path("kernel/core/display.c"):
        "e0673de76be150792fb779dc06e21e866a130852",
    Path("OS_Implementation_Specification_COMPLETE/include/kernel/display.h"):
        "693ebfde10ed78741a1b33dd7b741b0b9a5f0e2d",
}

BACKUP_SUFFIX = ".before-compositor-p7"


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


HEADER_TYPES = r'''
/*
 * Native scanout/page-flip contract.
 *
 * display_commit_submit() remains the public submission API.  When no native
 * backend is registered, display core uses the GOP copy fallback.  Once a
 * native backend is registered, the same submission is handed to atomic_commit
 * after its acquire fence has completed.
 *
 * A K_OK return from atomic_commit transfers asynchronous ownership of the
 * request to the backend.  The backend MUST invoke complete exactly once.
 *
 * complete(K_OK) means the new surface has been latched for scanout and the
 * previous scanout surface is no longer being read by the display engine.
 * That point is normally flip-done/vblank.  Display core signals the user's
 * release fence only after this callback.
 *
 * The completion callback may run synchronously from atomic_commit or later
 * from a normal/deferred kernel context, but MUST NOT run directly in hard IRQ
 * context.  A GPU IRQ handler should ACK the interrupt and schedule its own
 * bottom half before invoking complete.
 */
typedef struct display_scanout_request {
    uint32_t output;
    gpu_allocation_t *buffer;
    uint64_t offset;
    uint64_t stride;
    uint32_t width;
    uint32_t height;
    uint32_t format;
} display_scanout_request_t;

typedef void (*display_scanout_complete_fn)(
    void *completion_context,
    kstatus_t status);

typedef struct display_scanout_backend {
    void *context;

    kstatus_t (*atomic_commit)(
        void *context,
        const display_scanout_request_t *request,
        display_scanout_complete_fn complete,
        void *completion_context);
} display_scanout_backend_t;

'''


DISPLAY_NATIVE_HELPERS = r'''
/*
 * Native atomic scanout ownership
 * -------------------------------
 *
 * pending->buffer owns one object reference and one pin from
 * display_commit_submit().
 *
 * GOP fallback:
 *     release them when the copy/fence completes.
 *
 * Native success:
 *     transfer that exact reference+pin into g_display.current_scanout.
 *     Only after flip-done/vblank releases the previous current_scanout.
 *
 * Native failure:
 *     keep the old current_scanout and release pending normally.
 *
 * This makes the release fence a real scanout-lifetime fence rather than a
 * "page-flip command queued" fence.
 */
static void display_release_scanout_buffer(gpu_allocation_t *buffer) {
    if (buffer == 0) return;

    atomic_fetch_sub_explicit(
        &buffer->pin_count,
        1U,
        memory_order_acq_rel);

    object_put(buffer);
}


static bool display_scanout_backend_snapshot(
    display_scanout_backend_t *backend) {

    bool registered;

    if (backend == 0) {
        return false;
    }

    display_lock();

    registered =
        g_display.scanout_backend_registered;

    if (registered) {
        *backend =
            g_display.scanout_backend;
    }

    display_unlock();
    return registered;
}


static void display_native_scanout_complete(
    void *completion_context,
    kstatus_t status) {

    display_pending_commit_t *pending =
        (display_pending_commit_t *)completion_context;

    gpu_allocation_t *old_scanout = 0;
    uint64_t gpu_va = 0U;

    if (pending == 0) {
        return;
    }

    if (pending->buffer != 0) {
        gpu_va =
            pending->buffer->gpu_va.value;
    }

    if (status == K_OK) {
        /*
         * K_OK means the new scanout is latched and the old one is no longer
         * fetched by the display engine.
         *
         * Move, do not duplicate, pending's reference+pin into current_scanout.
         */
        display_lock();

        old_scanout =
            g_display.current_scanout;

        g_display.current_scanout =
            pending->buffer;

        pending->buffer = 0;
        g_display.pending_commit = false;

        display_unlock();

        display_release_scanout_buffer(
            old_scanout);

        (void)gpu_fence_signal(
            pending->signal_fence,
            pending->signal_value);

        (void)telemetry_record_latency(
            TELEMETRY_CATEGORY_GPU_SUBMIT,
            gpu_va,
            pending->start_tsc);
    } else {
        /*
         * Failed atomic flip: keep the current scanout untouched.  The new
         * buffer is still owned by pending and is released below.
         */
        display_clear_pending();

        (void)gpu_fence_fail(
            pending->signal_fence,
            status);
    }

    display_release_pending(pending);
}


static void display_submit_native_scanout(
    display_pending_commit_t *pending,
    const display_scanout_backend_t *backend) {

    display_scanout_request_t request;
    kstatus_t status;

    if (pending == 0 ||
        backend == 0 ||
        backend->atomic_commit == 0) {

        if (pending != 0) {
            display_clear_pending();

            (void)gpu_fence_fail(
                pending->signal_fence,
                K_EIO);

            display_release_pending(pending);
        }

        return;
    }

    request.output = 0U;
    request.buffer = pending->buffer;
    request.offset = pending->offset;
    request.stride = pending->stride;
    request.width = pending->width;
    request.height = pending->height;
    request.format = 1U;

    /*
     * Never hold display_lock across a GPU driver call.  A synchronous
     * completion is legal and may already have freed pending before the call
     * returns, so the K_OK path must not touch pending again.
     */
    status =
        backend->atomic_commit(
            backend->context,
            &request,
            display_native_scanout_complete,
            pending);

    if (status == K_OK) {
        return;
    }

    /*
     * Non-K_OK means the backend did not take asynchronous ownership and must
     * not invoke the completion callback.
     */
    display_clear_pending();

    (void)gpu_fence_fail(
        pending->signal_fence,
        status);

    display_release_pending(pending);
}

'''


DISPLAY_REGISTRATION = r'''
kstatus_t display_core_register_scanout_backend(
    uint32_t output,
    const display_scanout_backend_t *backend) {

    kstatus_t status = K_OK;

    if (output != 0U ||
        backend == 0 ||
        backend->atomic_commit == 0 ||
        !display_initialize()) {

        return K_EINVAL;
    }

    display_lock();

    if (g_display.scanout_backend_registered) {
        status = K_EEXIST;
    } else if (g_display.pending_commit) {
        /*
         * Do not switch a commit from GOP copy to native scanout after it has
         * already been accepted. Registration is normally a one-time GPU
         * bring-up operation.
         */
        status = K_EBUSY;
    } else {
        g_display.scanout_backend =
            *backend;

        g_display.scanout_backend_registered =
            true;
    }

    display_unlock();
    return status;
}


bool display_core_has_native_scanout(uint32_t output) {
    bool registered;

    if (output != 0U ||
        !display_initialize()) {

        return false;
    }

    display_lock();

    registered =
        g_display.scanout_backend_registered;

    display_unlock();
    return registered;
}


'''


def transform_header(text: str) -> str:
    include_marker = '#include "gpu.h"\n\n'
    text = replace_once(
        text,
        include_marker,
        include_marker + HEADER_TYPES,
        "display.h: native scanout types",
    )

    api_marker = '''bool display_core_query(uint32_t output, uint32_t *width, uint32_t *height,
                        uint32_t *stride, uint32_t *format);

'''
    api_add = api_marker + '''/*
 * Register the native atomic scanout implementation for an output.
 *
 * Registration is intentionally one-shot in P7.  The backend context and its
 * function remain valid for the lifetime of the display device.
 */
kstatus_t display_core_register_scanout_backend(
    uint32_t output,
    const display_scanout_backend_t *backend);

bool display_core_has_native_scanout(uint32_t output);

'''
    return replace_once(
        text,
        api_marker,
        api_add,
        "display.h: native scanout API",
    )


def transform_display(text: str) -> str:
    state_old = '''    uint32_t format;
    uint32_t masks[4];
    /* 同一输出只允许一个未完成的异步拷贝，避免提交突发耗尽 deferred 队列。 */
    bool initialized;
    bool pending_commit;
} display_state_t;'''

    state_new = '''    uint32_t format;
    uint32_t masks[4];

    /*
     * Optional native scanout backend.
     *
     * current_scanout owns exactly one object reference and one pin while the
     * display engine may still fetch from that allocation.
     */
    display_scanout_backend_t scanout_backend;
    gpu_allocation_t *current_scanout;
    bool scanout_backend_registered;

    /* 同一输出只允许一个未完成的提交，GOP copy 与 native flip 共用此门。 */
    bool initialized;
    bool pending_commit;
} display_state_t;'''

    text = replace_once(
        text,
        state_old,
        state_new,
        "display.c: display state",
    )

    shape_old = '''    if (buffer == 0 || buffer->object.type != KOBJECT_TYPE_GPU_ALLOCATION ||
        buffer->backing == 0 || width == 0U || height == 0U ||
        stride < (uint64_t)width * sizeof(uint32_t) || offset > buffer->size) {
        return false;
    }'''

    shape_new = '''    if (buffer == 0 || buffer->object.type != KOBJECT_TYPE_GPU_ALLOCATION ||
        width == 0U || height == 0U ||
        stride < (uint64_t)width * sizeof(uint32_t) || offset > buffer->size) {
        return false;
    }'''

    text = replace_once(
        text,
        shape_old,
        shape_new,
        "display.c: generic scanout shape validation",
    )

    complete_marker = 'static void display_complete_commit(void *argument) {'
    if text.count(complete_marker) != 1:
        raise RuntimeError(
            "display.c: display_complete_commit marker is not unique"
        )
    text = text.replace(
        complete_marker,
        DISPLAY_NATIVE_HELPERS + complete_marker,
        1,
    )

    wait_old = '''    if (wait_status != K_OK) {
        display_clear_pending();
        (void)gpu_fence_fail(pending->signal_fence, wait_status);
        display_release_pending(pending);
        return;
    }

    first_row = pending->next_row;'''

    wait_new = '''    if (wait_status != K_OK) {
        display_clear_pending();
        (void)gpu_fence_fail(pending->signal_fence, wait_status);
        display_release_pending(pending);
        return;
    }

    /*
     * Acquire-fence completion is the common dispatch point.  Registration is
     * one-shot and forbidden while a commit is pending, so a submission cannot
     * change backend underneath this decision.
     */
    {
        display_scanout_backend_t backend;

        if (display_scanout_backend_snapshot(&backend)) {
            display_submit_native_scanout(
                pending,
                &backend);
            return;
        }
    }

    first_row = pending->next_row;'''

    text = replace_once(
        text,
        wait_old,
        wait_new,
        "display.c: native/GOP dispatch",
    )

    init_marker = 'bool display_core_init(uint64_t framebuffer_virtual, uint64_t framebuffer_size,'
    if text.count(init_marker) != 1:
        raise RuntimeError(
            "display.c: display_core_init marker is not unique"
        )
    text = text.replace(
        init_marker,
        DISPLAY_REGISTRATION + init_marker,
        1,
    )

    comment_old = '''    /*
     * DISPLAY_COMMIT 是异步的；如果允许同一缓冲区无限叠加 pending copy，
     * 高频输入会把 deferred 队列填满，进而让显示和输入互相拖住。收到
     * K_EBUSY 的调用者稍后重试即可，最后一个提交仍由 fence 完成通知。
     */'''

    comment_new = '''    /*
     * DISPLAY_COMMIT 是异步的；GOP copy 和 native page-flip 共用一个
     * pending 槽。这样 fence / buffer ownership 始终只有一个明确的状态
     * 转移点，也避免高频提交把 deferred 队列或 GPU flip queue 填满。
     * 收到 K_EBUSY 的调用者稍后重试即可。
     */'''

    return replace_once(
        text,
        comment_old,
        comment_new,
        "display.c: pending commit comment",
    )


TRANSFORMS = {
    Path("kernel/core/display.c"): transform_display,
    Path("OS_Implementation_Specification_COMPLETE/include/kernel/display.h"):
        transform_header,
}


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "LiteOS display P7: native atomic scanout backend"
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
            print(
                f"error: missing {path}; run from LiteOS repo root",
                file=sys.stderr,
            )
            return 2

        data = path.read_bytes()
        sha = git_blob_sha(data)

        if sha != expected_sha and not args.force_context:
            print(
                f"error: P7 base mismatch: {path}\n"
                f"  expected: {expected_sha}\n"
                f"  current : {sha}\n"
                "P7 expects the P6-applied source currently on GitHub main.",
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
        print("OK: P7 exact base/context verified")
        for path, expected_sha in FILES.items():
            print(f"{expected_sha}  {path}")
        print("P7: DISPLAY_COMMIT -> native atomic flip or GOP fallback")
        print("P7: release fence means flip-done/vblank on native backend")
        print("P7: current scanout owns one object ref + one pin")
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
        out.write_text(
            patch_text,
            encoding="utf-8",
        )
        print(f"wrote: {out}")
        return 0

    for path in FILES:
        backup = Path(str(path) + BACKUP_SUFFIX)

        if not backup.exists():
            shutil.copy2(path, backup)
            print(f"backup: {backup}")

        path.write_text(
            modified[path],
            encoding="utf-8",
            newline="",
        )
        print(f"patched: {path}")

    print("P7 applied")
    print("run: git diff --check")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
