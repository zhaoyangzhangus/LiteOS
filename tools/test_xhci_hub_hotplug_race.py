#!/usr/bin/env python3

from pathlib import Path
import re
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"


def serial_text(path):
    try:
        return path.read_text(errors="replace")
    except FileNotFoundError:
        return ""


def wait_until(predicate, timeout, label):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            print("PASS:", label)
            return True
        time.sleep(0.05)
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


def choose_usb_mouse(hmp):
    text = hmp.cmd("info mice")
    candidates = []
    for line in text.splitlines():
        m = re.search(r"Mouse\s+#(\d+)", line)
        if m:
            candidates.append((int(m.group(1)), line))
    if not candidates:
        raise RuntimeError("no mouse reported")
    for idx, line in candidates:
        if "hid mouse" in line.lower() or ("usb" in line.lower() and "mouse" in line.lower()):
            hmp.cmd(f"mouse_set {idx}")
            return idx
    idx = candidates[-1][0]
    hmp.cmd(f"mouse_set {idx}")
    return idx


def send_mouse_events(hmp, count=8):
    direction = 1
    for i in range(count):
        dx = 3 * direction
        dy = 1 if (i & 1) else -1
        hmp.cmd(f"mouse_move {dx} {dy}", 1.0)
        direction = -direction
        time.sleep(0.006)


def run_case(name, settle_seconds):
    serial = BUILD / f"qemu-serial-{name}.log"
    monitor = BUILD / f"qemu-monitor-{name}.sock"
    out_path = BUILD / f"hub-race-{name}.stdout"
    err_path = BUILD / f"hub-race-{name}.stderr"

    # run-qemu.sh writes the canonical qemu-serial.log. We copy it after run.
    canonical_serial = BUILD / "qemu-serial.log"

    for p in (monitor, out_path, err_path, canonical_serial):
        try:
            p.unlink()
        except FileNotFoundError:
            pass

    out = out_path.open("wb")
    err = err_path.open("wb")
    runner = None
    hmp = None

    print()
    print("=" * 72)
    print(f"CASE {name}: settle={settle_seconds:.1f}s")
    print("=" * 72)

    try:
        runner = subprocess.Popen(
            [
                str(ROOT / "run-qemu.sh"),
                "--usb-nested",
                "--monitor-socket", str(monitor),
                "--seconds", "45",
                "--no-build",
            ],
            cwd=ROOT,
            stdout=out,
            stderr=err,
        )

        if not wait_until(lambda: monitor.exists(), 12.0, "HMP online"):
            return False

        end = time.monotonic() + 5.0
        while hmp is None:
            try:
                hmp = HMP(monitor)
            except OSError:
                if time.monotonic() >= end:
                    raise
                time.sleep(0.05)

        if not wait_until(
            lambda: "LITEOS_XHCI_HUB_RUNTIME_OK" in serial_text(canonical_serial),
            25.0,
            "Hub runtime ready",
        ):
            return False

        if not wait_until(
            lambda: "LITEOS_USB_DEVICE_COUNT=3" in serial_text(canonical_serial),
            20.0,
            "nested topology ready",
        ):
            return False

        choose_usb_mouse(hmp)
        send_mouse_events(hmp, 8)

        # Match the original test behavior: this marker may already exist, but
        # the eight HMP commands themselves have completed before we continue.
        wait_until(
            lambda: "LITEOS_USB_MOUSE_EVENT_OK" in serial_text(canonical_serial),
            5.0,
            "mouse path alive",
        )

        if settle_seconds:
            print(f"settling {settle_seconds:.1f}s before device_del ...")
            time.sleep(settle_seconds)

        before = serial_text(canonical_serial)
        base_transfer = before.count("LITEOS_DIAG_HUB_TRANSFER_EVENT")
        base_change = before.count("LITEOS_USB_RUNTIME_HUB_CHANGE")

        print()
        print("BEFORE DELETE:")
        hmp.cmd("info usb")

        hmp.cmd("device_del liteos-nested-mouse")

        time.sleep(0.2)
        print()
        print("0.2s AFTER DELETE:")
        hmp.cmd("info usb")

        removed = wait_until(
            lambda: serial_text(canonical_serial).count("LITEOS_USB_RUNTIME_HUB_CHANGE") > base_change,
            10.0,
            "runtime Hub-change after delete",
        )

        after = serial_text(canonical_serial)
        got_transfer = after.count("LITEOS_DIAG_HUB_TRANSFER_EVENT") > base_transfer

        print()
        print("NEW DIAGNOSTICS:")
        for line in after[len(before):].splitlines():
            if (
                "LITEOS_DIAG_" in line
                or "LITEOS_USB_RUNTIME_HUB_CHANGE" in line
                or "LITEOS_XHCI_FAIL" in line
            ):
                print(line)

        print()
        print("RESULT",
              name,
              "transfer=", "YES" if got_transfer else "NO",
              "hub_change=", "YES" if removed else "NO")

        try:
            serial.write_text(after, encoding="utf-8")
        except Exception:
            pass

        return removed

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
            monitor.unlink()
        except FileNotFoundError:
            pass


def main():
    BUILD.mkdir(parents=True, exist_ok=True)

    immediate = run_case("immediate", 0.0)
    settled = run_case("settled", 1.0)

    print()
    print("=" * 72)
    print("SUMMARY")
    print("=" * 72)
    print("immediate:", "PASS" if immediate else "FAIL")
    print("settled  :", "PASS" if settled else "FAIL")

    if not immediate and settled:
        print("CLASSIFICATION: startup/runtime Hub TD readiness race")
        return 10
    if not immediate and not settled:
        print("CLASSIFICATION: HID activity interferes with Hub IRQ/deferred path")
        return 11
    if immediate and settled:
        print("CLASSIFICATION: original failure is intermittent; repeat stress needed")
        return 0

    print("CLASSIFICATION: unusual timing inversion")
    return 12


if __name__ == "__main__":
    sys.exit(main())
