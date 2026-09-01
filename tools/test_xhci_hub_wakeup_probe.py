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
        str(Path(tempfile.gettempdir()) / "liteos-xhci-hub-wakeup-probe.sock"),
    )
)
MOUSE_EVENT_DELAY = float(os.environ.get("XHCI_TEST_MOUSE_DELAY", "0.006"))
BOOT_READY_TIMEOUT = float(
    os.environ.get("XHCI_TEST_BOOT_READY_TIMEOUT", "60.0")
)
QEMU_SECONDS = os.environ.get("XHCI_TEST_QEMU_SECONDS", "90")
POLL_INTERVAL = float(os.environ.get("XHCI_TEST_POLL_INTERVAL", "0.25"))
OUT = BUILD / "xhci-hub-wakeup-probe.stdout"
ERR = BUILD / "xhci-hub-wakeup-probe.stderr"


def serial_text():
    try:
        return SERIAL.read_text(errors="replace")
    except OSError:
        return ""


def wait_for(predicate, timeout, label):
    if POLL_INTERVAL <= 0.0:
        raise RuntimeError("XHCI_TEST_POLL_INTERVAL must be positive")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            print("PASS:", label)
            return True
        time.sleep(POLL_INTERVAL)
    print("TIMEOUT:", label)
    return False


class HMP:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(0.25)
        self.sock.connect(str(path))
        self._read(3.0)

    def _read(self, timeout):
        end = time.monotonic() + timeout
        data = bytearray()
        while time.monotonic() < end:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                break
            data.extend(chunk)
            if b"(qemu)" in data:
                break
        return data.decode(errors="replace")

    def cmd(self, command, timeout=3.0):
        self.sock.sendall((command + "\n").encode())
        text = self._read(timeout).replace("\r", "")
        print()
        print("HMP>", command)
        for line in text.splitlines():
            if line.strip() != "(qemu)":
                print("   ", line)
        return text

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


def print_new_diag(baseline_len):
    text = serial_text()[baseline_len:]
    wanted = []
    for line in text.splitlines():
        if (
            "LITEOS_DIAG_" in line
            or "LITEOS_USB_RUNTIME_HUB_CHANGE" in line
            or "LITEOS_XHCI_FAIL" in line
        ):
            wanted.append(line)

    print()
    print("=== NEW GUEST DIAGNOSTICS ===")
    if not wanted:
        print("(none)")
    else:
        for line in wanted:
            print(line)


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    for path in (MONITOR, OUT, ERR):
        try:
            path.unlink()
        except FileNotFoundError:
            pass

    out = OUT.open("wb")
    err = ERR.open("wb")
    runner = None
    hmp = None

    try:
        runner = subprocess.Popen(
            [
                str(ROOT / "run-qemu.sh"),
                "--headless",
                "--usb-nested",
                "--monitor-socket", str(MONITOR),
                "--seconds", QEMU_SECONDS,
                "--no-build",
            ],
            cwd=ROOT,
            stdout=out,
            stderr=err,
        )

        if not wait_for(lambda: MONITOR.exists(), 12.0, "HMP socket online"):
            return 2

        if not wait_for(
            lambda: "LITEOS_XHCI_HUB_RUNTIME_OK" in serial_text(),
            BOOT_READY_TIMEOUT,
            "Hub runtime ready",
        ):
            return 3

        # Hub runtime readiness precedes the post-display user-runtime
        # self-test.  Wait for that independent boot gate before removing a
        # device, so a recovery probe cannot turn a slow self-test into a
        # misleading stage failure.
        if not wait_for(
            lambda: "LITEOS_USER_RUNTIME_SUBTESTS_OK" in serial_text(),
            BOOT_READY_TIMEOUT,
            "boot runtime self-tests ready",
        ):
            return 4
        if "LITEOS_STAGE_FAIL" in serial_text():
            return 5

        # Attach HMP only after the boot-critical user-runtime self-test.  A
        # connected monitor increases TCG main-loop load and can otherwise
        # perturb that self-test's scheduling window.
        end = time.monotonic() + 5.0
        while hmp is None:
            try:
                hmp = HMP(MONITOR)
            except OSError:
                if time.monotonic() >= end:
                    raise
                time.sleep(0.05)

        # Give the freshly armed interrupt endpoints enough time to reach
        # QEMU's normal NAK/retry state even for the slowest legal interval
        # used by this test topology.
        time.sleep(1.0)

        print()
        print("========================================")
        print("BEFORE DEVICE_DEL")
        print("========================================")
        hmp.cmd("info usb")

        baseline_text = serial_text()
        baseline_len = len(baseline_text)
        baseline_transfer = baseline_text.count("LITEOS_DIAG_HUB_TRANSFER_EVENT")
        baseline_change = baseline_text.count("LITEOS_USB_RUNTIME_HUB_CHANGE")

        hmp.cmd("device_del liteos-nested-mouse")

        for delay in (0.2, 1.0, 5.0):
            time.sleep(delay if delay == 0.2 else delay - (0.2 if delay == 1.0 else 1.0))
            print()
            print("========================================")
            print(f"{delay:.1f}s AFTER DEVICE_DEL")
            print("========================================")
            hmp.cmd("info usb")
            print_new_diag(baseline_len)

        print()
        print("Waiting up to 40s for a Hub Transfer Event ...")
        got_transfer = wait_for(
            lambda: serial_text().count("LITEOS_DIAG_HUB_TRANSFER_EVENT")
                    > baseline_transfer,
            40.0,
            "Hub Transfer Event after nested detach",
        )

        got_change = serial_text().count("LITEOS_USB_RUNTIME_HUB_CHANGE") > baseline_change
        stage_failed = "LITEOS_STAGE_FAIL" in serial_text()

        print_new_diag(baseline_len)

        print()
        print("=== RESULT ===")
        print("hub_transfer_event:", "YES" if got_transfer else "NO")
        print("runtime_hub_change:", "YES" if got_change else "NO")
        print("stage_failure:", "YES" if stage_failed else "NO")

        if ERR.exists():
            qemu_err = ERR.read_text(errors="replace").strip()
            if qemu_err:
                print()
                print("=== QEMU STDERR TAIL ===")
                print(qemu_err[-6000:])

        return 0 if got_transfer and not stage_failed else 1

    finally:
        if hmp is not None:
            try:
                hmp.cmd("quit", 0.5)
            except Exception:
                pass
            hmp.close()

        if runner is not None:
            try:
                runner.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                runner.terminate()
                try:
                    runner.wait(timeout=2.0)
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
