#!/usr/bin/env python3

from pathlib import Path
import os
import re
import socket
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("BUILD", str(ROOT / "build")))
if not BUILD.is_absolute():
    BUILD = ROOT / BUILD

SERIAL = BUILD / "qemu-serial.log"
MONITOR = Path(
    os.environ.get(
        "QEMU_MONITOR_SOCKET",
        str(Path(tempfile.gettempdir()) / "liteos-xhci-hub-hotplug.sock"),
    )
)

RUNNER_STDOUT = BUILD / "b10b3-runner.stdout"
RUNNER_STDERR = BUILD / "b10b3-runner.stderr"
# QEMU's HMP mouse_move path coalesces updates faster than the guest's USB
# polling interval.  Keep the default deliberately above that coalescing
# window so the stress phase measures HID re-arming rather than host-input
# batching.  CI can still override it for a controller-specific probe.
MOUSE_EVENT_DELAY = float(os.environ.get("XHCI_TEST_MOUSE_DELAY", "0.05"))
MOUSE_READY_TIMEOUT = float(
    os.environ.get("XHCI_TEST_MOUSE_READY_TIMEOUT", "5.0")
)
HUB_SETTLE_SECONDS = float(
    os.environ.get("XHCI_TEST_HUB_SETTLE", "1.0")
)
BOOT_READY_TIMEOUT = float(
    os.environ.get("XHCI_TEST_BOOT_READY_TIMEOUT", "60.0")
)
QEMU_SECONDS = os.environ.get("XHCI_TEST_QEMU_SECONDS", "120")
POLL_INTERVAL = float(os.environ.get("XHCI_TEST_POLL_INTERVAL", "0.25"))


def serial_text():
    if not SERIAL.exists():
        return ""

    try:
        return SERIAL.read_text(errors="replace")
    except OSError:
        # QEMU writes this file directly in headless mode.  A Windows/WSL
        # mounted-file read can briefly return EAGAIN/ENODATA while the file
        # is being extended; the next polling iteration will retry.
        return ""


def fail(message):
    raise RuntimeError(message)


def wait_until(predicate, label, timeout=15.0):
    if POLL_INTERVAL <= 0.0:
        fail("XHCI_TEST_POLL_INTERVAL must be positive")
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        if predicate():
            print("PASS:", label)
            return

        time.sleep(POLL_INTERVAL)

    fail("timeout: " + label)


def hub_change_count():
    return serial_text().count(
        "LITEOS_USB_RUNTIME_HUB_CHANGE"
    )


def latest_device_count():
    matches = re.findall(
        r"LITEOS_USB_DEVICE_COUNT=(\d+)",
        serial_text()
    )

    if not matches:
        return 0

    return int(matches[-1])


class HMP:
    def __init__(self, path):
        self.sock = socket.socket(
            socket.AF_UNIX,
            socket.SOCK_STREAM
        )

        self.sock.settimeout(0.25)
        self.sock.connect(str(path))

        self._read_until_prompt(3.0)

    def _read_until_prompt(self, timeout):
        deadline = time.monotonic() + timeout
        data = bytearray()

        while time.monotonic() < deadline:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue

            if not chunk:
                break

            data.extend(chunk)

            if b"(qemu)" in data:
                break

        return data.decode(
            errors="replace"
        )

    def command(self, command, timeout=3.0):
        self.sock.sendall(
            (command + "\n").encode()
        )

        response = self._read_until_prompt(
            timeout
        )

        compact = response.replace(
            "\r",
            ""
        ).strip()

        if compact:
            print(
                "HMP>",
                command
            )

            for line in compact.splitlines():
                if line.strip() != "(qemu)":
                    print(
                        "   ",
                        line
                    )

        lowered = response.lower()

        bad = (
            "error:" in lowered or
            "unknown command" in lowered or
            "not found" in lowered or
            "duplicate id" in lowered
        )

        if bad:
            fail(
                "HMP command failed: " +
                command
            )

        return response

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


def choose_usb_mouse(hmp):
    text = hmp.command(
        "info mice"
    )

    candidates = []

    for line in text.splitlines():
        m = re.search(
            r"Mouse\s+#(\d+)",
            line
        )

        if m:
            candidates.append(
                (
                    int(m.group(1)),
                    line
                )
            )

    if not candidates:
        fail(
            "QEMU monitor reports no mouse"
        )

    #
    # Prefer the explicit QEMU USB mouse.
    #
    for index, line in candidates:
        lowered = line.lower()

        if (
            "usb" in lowered and
            "mouse" in lowered
        ):
            hmp.command(
                f"mouse_set {index}"
            )

            return index

    #
    # In case this QEMU version labels it differently,
    # the hot-plugged USB mouse is normally the newest one.
    #
    index = candidates[-1][0]

    hmp.command(
        f"mouse_set {index}"
    )

    return index


def send_mouse_events(hmp, count):
    direction = 1

    if MOUSE_EVENT_DELAY < 0.0:
        fail("XHCI_TEST_MOUSE_DELAY must not be negative")
    if HUB_SETTLE_SECONDS < 0.0:
        fail("XHCI_TEST_HUB_SETTLE must not be negative")

    for i in range(count):
        dx = 3 * direction
        dy = 1 if (i & 1) else -1
        hmp.command(
            f"mouse_move {dx} {dy}",
            timeout=1.0
        )

        direction = -direction

        #
        # Avoid collapsing every host movement into one guest report.
        #
        time.sleep(MOUSE_EVENT_DELAY)

    # A button transition is a stronger end-to-end probe than movement alone:
    # it must produce a boot-mouse interrupt report even when a headless QEMU
    # frontend coalesces relative motion.
    hmp.command("mouse_button 1", timeout=1.0)
    time.sleep(MOUSE_EVENT_DELAY)
    hmp.command("mouse_button 0", timeout=1.0)


def print_serial_tail():
    s = serial_text()

    lines = [
        line
        for line in s.splitlines()
        if (
            "LITEOS_XHCI" in line or
            "LITEOS_USB" in line
        )
    ]

    print()
    print("=== SERIAL TAIL ===")

    for line in lines[-120:]:
        print(line)


def main():
    BUILD.mkdir(
        parents=True,
        exist_ok=True
    )

    for path in (
        SERIAL,
        MONITOR,
        RUNNER_STDOUT,
        RUNNER_STDERR,
    ):
        try:
            path.unlink()
        except FileNotFoundError:
            pass

    out = RUNNER_STDOUT.open("wb")
    err = RUNNER_STDERR.open("wb")

    runner = None
    hmp = None

    try:
        runner = subprocess.Popen(
            [
                str(ROOT / "run-qemu.sh"),
                "--headless",
                "--usb-nested",
                "--monitor-socket",
                str(MONITOR),
                "--seconds",
                QEMU_SECONDS,
                "--no-build",
            ],
            cwd=ROOT,
            stdout=out,
            stderr=err,
        )

        # ----------------------------------------------------
        # Monitor online
        # ----------------------------------------------------

        wait_until(
            lambda:
                MONITOR.exists(),
            "QEMU HMP socket online",
            12.0
        )

        # ----------------------------------------------------
        # Initial nested topology
        # ----------------------------------------------------

        wait_until(
            lambda:
                "LITEOS_XHCI_MSIX_OK"
                in serial_text(),
            "MSI-X runtime ready",
            BOOT_READY_TIMEOUT
        )

        wait_until(
            lambda:
                latest_device_count() >= 3,
            "Hub -> Hub -> Mouse published",
            BOOT_READY_TIMEOUT
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
            BOOT_READY_TIMEOUT
        )

        # The deferred worker is published before the graphics worker.  Wait
        # for the complete post-boot scheduler boundary before injecting HID
        # input, otherwise the first report can race window-worker startup.
        wait_until(
            lambda:
                "LITEOS_WINDOW_WORKER_OK"
                in serial_text(),
            "window worker ready",
            BOOT_READY_TIMEOUT
        )

        # The window worker starts before the post-display user-runtime
        # self-test.  Do not inject HID traffic while that test is still
        # exercising VM, scheduler, and process teardown; otherwise this
        # hotplug test can manufacture an unrelated USER_RUNTIME timeout.
        wait_until(
            lambda:
                "LITEOS_USER_RUNTIME_SUBTESTS_OK"
                in serial_text(),
            "boot runtime self-tests ready",
            BOOT_READY_TIMEOUT
        )

        if "LITEOS_STAGE_FAIL" in serial_text():
            fail(
                "boot runtime self-test failed before hotplug"
            )

        # Keep the monitor server online during boot, but do not attach a
        # client until the boot-critical runtime self-test has completed.  A
        # live HMP connection changes QEMU's main-loop load enough to perturb
        # that deliberately tight self-test window on TCG.
        deadline = time.monotonic() + 5.0
        while True:
            try:
                hmp = HMP(MONITOR)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.05)

        #
        # Generate a few initial events, but deliberately stay well
        # below 256 so the 256 milestone can prove post-replug HID.
        #
        choose_usb_mouse(hmp)

        send_mouse_events(
            hmp,
            8
        )

        wait_until(
            lambda:
                "LITEOS_USB_MOUSE_EVENT_OK"
                in serial_text(),
            "initial nested mouse works",
            MOUSE_READY_TIMEOUT
        )

        if "LITEOS_XHCI_HID_EVENTS_256" in serial_text():
            fail(
                "256 HID milestone occurred before hotplug test"
            )

        # Let QEMU's Hub interrupt endpoint return to its normal NAK/retry
        # state before changing a downstream port.  This avoids coupling the
        # topology test to the last mouse report's completion timing.
        time.sleep(HUB_SETTLE_SECONDS)

        baseline_changes = (
            hub_change_count()
        )

        print()
        print(
            "Initial Hub-change count:",
            baseline_changes
        )

        # ----------------------------------------------------
        # Disconnect mouse
        # ----------------------------------------------------

        print()
        print(
            "=== HOT UNPLUG nested mouse ==="
        )

        hmp.command(
            "device_del liteos-nested-mouse"
        )

        wait_until(
            lambda:
                hub_change_count() >
                baseline_changes,
            "guest removed child subtree",
            10.0
        )

        if "LITEOS_XHCI_FAIL=" in serial_text():
            fail(
                "xHCI failure during subtree removal"
            )

        removed_changes = (
            hub_change_count()
        )

        #
        # Allow QEMU's delete request to settle before reusing the ID.
        #
        time.sleep(0.5)

        # ----------------------------------------------------
        # Reconnect same physical Hub port
        # ----------------------------------------------------

        print()
        print(
            "=== HOT REPLUG nested mouse ==="
        )

        hmp.command(
            "device_add usb-mouse,"
            "id=liteos-nested-mouse,"
            "bus=liteos-xhci.0,"
            "port=1.1.1"
        )

        wait_until(
            lambda:
                hub_change_count() >
                removed_changes,
            "guest enumerated and published replacement child",
            12.0
        )

        # device_add is asynchronous in QEMU.  The Hub-change marker means
        # the Slot is published, while the host input device may still be
        # joining QEMU's active-mouse list.
        time.sleep(HUB_SETTLE_SECONDS)

        if "LITEOS_XHCI_FAIL=" in serial_text():
            fail(
                "xHCI failure during child re-enumeration"
            )

        # ----------------------------------------------------
        # Prove new Slot.context owns HID runtime
        # ----------------------------------------------------

        print()
        print(
            "=== POST-REPLUG HID STRESS ==="
        )

        choose_usb_mouse(hmp)

        #
        # Enough reports to cross the driver's 256 completion
        # milestone even if a few host motions are coalesced.
        #
        send_mouse_events(
            hmp,
            640
        )

        wait_until(
            lambda:
                "LITEOS_XHCI_HID_EVENTS_256"
                in serial_text(),
            "replugged mouse reached >=256 HID completions",
            12.0
        )

        if (
            "LITEOS_XHCI_FAIL="
            in serial_text() or
            "LITEOS_XHCI_MSIX_FAIL" in serial_text() or
            "LITEOS_STAGE_FAIL" in serial_text()
        ):
            fail(
                "runtime xHCI or boot-stage failure after replug"
            )

        final_changes = (
            hub_change_count()
        )

        if final_changes < baseline_changes + 2:
            fail(
                "expected separate disconnect and reconnect topology changes"
            )

        print()
        print(
            "========================================="
        )
        print(
            "V3.10.6 B10B-3 HUB HOTPLUG: PASS"
        )
        print(
            "========================================="
        )
        print()
        print(
            "Verified:"
        )
        print(
            "  Hub #2 port disconnect event"
        )
        print(
            "  child Slot subtree teardown"
        )
        print(
            "  DMA/resource release survived"
        )
        print(
            "  same physical port reconnect"
        )
        print(
            "  new Slot enumeration"
        )
        print(
            "  working context -> Slot.context publish"
        )
        print(
            "  replugged HID >=256 completions"
        )

        print_serial_tail()

        #
        # Clean QEMU exit.
        #
        hmp.command(
            "quit",
            timeout=2.0
        )

        runner.wait(
            timeout=5.0
        )

        return 0

    except Exception as exc:
        print()
        print(
            "========================================="
        )
        print(
            "V3.10.6 B10B-3 HUB HOTPLUG: FAIL"
        )
        print(
            "========================================="
        )
        print()
        print(
            str(exc)
        )

        print_serial_tail()

        if RUNNER_STDERR.exists():
            text = RUNNER_STDERR.read_text(
                errors="replace"
            ).strip()

            if text:
                print()
                print(
                    "=== QEMU STDERR ==="
                )
                print(text[-4000:])

        return 1

    finally:
        if hmp is not None:
            try:
                hmp.command(
                    "quit",
                    timeout=0.5
                )
            except Exception:
                pass

            hmp.close()

        if runner is not None:
            try:
                runner.wait(
                    timeout=2.0
                )
            except subprocess.TimeoutExpired:
                runner.terminate()

                try:
                    runner.wait(
                        timeout=2.0
                    )
                except subprocess.TimeoutExpired:
                    runner.kill()
                    runner.wait()

        out.close()
        err.close()

        try:
            MONITOR.unlink()
        except FileNotFoundError:
            pass


if __name__ == "__main__":
    sys.exit(main())
