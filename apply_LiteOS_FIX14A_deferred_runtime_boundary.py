#!/usr/bin/env python3
from pathlib import Path

ROOT = Path.cwd()

def edit(path, transform):
    p = ROOT / path
    if not p.exists():
        raise SystemExit(f"missing: {path}")
    old = p.read_text(encoding="utf-8")
    new = transform(old)
    if new == old:
        raise SystemExit(f"no change made: {path}")
    p.write_text(new, encoding="utf-8")
    print("updated:", path)

def replace_once(s, old, new, label):
    n = s.count(old)
    if n != 1:
        raise SystemExit(f"{label}: expected 1 match, found {n}")
    return s.replace(old, new, 1)

def patch_kernel_entry(s):
    early = '''    /*
     * deferred_init() intentionally ran during early boot before the
     * canonical scheduler existed.  Now give the global deferred queue its
     * persistent Ring0 executor; IRQ producers no longer depend on kernel_main
     * reaching the idle HLT loop before bottom halves can run.
     */
    if (!deferred_start_worker()) {
        serial_write("LITEOS_DEFERRED_WORKER_FAIL\\r\\n");
        halt_forever();
    }
    serial_write("LITEOS_DEFERRED_WORKER_OK\\r\\n");
'''
    if early not in s:
        raise SystemExit("FIX14A: early deferred worker block not found")
    s = s.replace(early, "", 1)

    target = '''    if (!window_server_init()) {
        serial_write("LITEOS_WINDOW_SERVER_INIT_FAIL\\r\\n");
        halt_forever();
    }

    if (!user_init_start()) {
'''
    replacement = '''    if (!window_server_init()) {
        serial_write("LITEOS_WINDOW_SERVER_INIT_FAIL\\r\\n");
        halt_forever();
    }

    /*
     * Start the persistent bottom-half executor only after every boot
     * self-test has finished.  Runtime device IRQs may already have queued
     * deferred items; deferred_start_worker() immediately drains those before
     * Ring3 services begin.
     *
     * Starting this worker earlier makes live xHCI/input events race with
     * deterministic boot self-tests (notably Bluetooth/Input validation).
     */
    if (!deferred_start_worker()) {
        serial_write("LITEOS_DEFERRED_WORKER_FAIL\\r\\n");
        halt_forever();
    }
    serial_write("LITEOS_DEFERRED_WORKER_OK\\r\\n");

    if (!user_init_start()) {
'''
    s = replace_once(s, target, replacement, "FIX14A late worker insertion")

    idle = '''        __asm__ volatile ("sti; hlt" : : : "memory");
        (void)deferred_run(8U);
        window_server_pump_input();
'''
    idle_new = '''        __asm__ volatile ("sti; hlt" : : : "memory");
        /*
         * Runtime deferred work is owned exclusively by the persistent
         * scheduler worker.  The idle thread must not become a second
         * concurrent bottom-half consumer.
         */
        window_server_pump_input();
'''
    s = replace_once(s, idle, idle_new, "FIX14A idle consumer removal")
    return s

def patch_hotplug_test(s):
    anchor = '''        wait_until(
            lambda:
                latest_device_count() >= 3,
            "Hub -> Hub -> Mouse published",
            20.0
        )

        #
        # Generate a few initial events, but deliberately stay well
'''
    replacement = '''        wait_until(
            lambda:
                latest_device_count() >= 3,
            "Hub -> Hub -> Mouse published",
            20.0
        )

        #
        # xHCI can become MSI-X-ready before the kernel has finished its
        # deterministic boot self-tests.  Do not inject live mouse input until
        # the persistent deferred worker is deliberately started at the
        # runtime boundary.
        #
        wait_until(
            lambda:
                "LITEOS_DEFERRED_WORKER_OK"
                in serial_text(),
            "persistent deferred worker ready",
            20.0
        )

        #
        # Generate a few initial events, but deliberately stay well
'''
    return replace_once(s, anchor, replacement, "FIX14A test worker wait")

edit("kernel/kernel_entry.c", patch_kernel_entry)
edit("tools/test_xhci_hub_hotplug.py", patch_hotplug_test)

print()
print("FIX14A applied.")
print("Next:")
print("  git diff --check")
print("  make clean && make")
print("  python3 tools/test_xhci_hub_hotplug.py")
