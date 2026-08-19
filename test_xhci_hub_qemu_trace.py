#!/usr/bin/env python3

from pathlib import Path
import re
import socket
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
if (HERE / "run-qemu.sh").exists():
    ROOT = HERE
elif (HERE.parent / "run-qemu.sh").exists():
    ROOT = HERE.parent
else:
    raise SystemExit("cannot locate LiteOS run-qemu.sh")

BUILD = ROOT / "build"
SERIAL = BUILD / "qemu-serial.log"
MONITOR = BUILD / "qemu-monitor-xhci-trace.sock"
RUNNER_STDOUT = BUILD / "xhci-trace-runner.stdout"
RUNNER_STDERR = BUILD / "xhci-trace-runner.stderr"

TRACE_EVENTS = [
    "usb_port_detach",
    "usb_hub_detach",
    "usb_hub_status_report",
    "usb_xhci_ep_kick",
    "usb_xhci_xfer_start",
    "usb_xhci_xfer_nak",
    "usb_xhci_xfer_retry",
    "usb_xhci_xfer_success",
    "usb_xhci_queue_event",
    "usb_xhci_irq_msix",
]

TRACE_MARKERS = tuple(name + " " for name in TRACE_EVENTS)


def serial_text():
    try:
        return SERIAL.read_text(errors="replace")
    except FileNotFoundError:
        return ""


def wait_until(predicate, label, timeout):
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
        self._read_until_prompt(3.0)

    def _read_until_prompt(self, timeout):
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

    def command(self, command, timeout=3.0, quiet=False):
        self.sock.sendall((command + "\n").encode())
        response = self._read_until_prompt(timeout)
        if not quiet:
            print("HMP>", command)
            for line in response.replace("\r", "").splitlines():
                if line.strip() != "(qemu)":
                    print("   ", line)

        low = response.lower()
        if (
            "unknown command" in low
            or "command not found" in low
            or "error:" in low
        ):
            raise RuntimeError("HMP failed: " + command + "\n" + response)
        return response

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


def choose_usb_mouse(hmp):
    text = hmp.command("info mice")
    candidates = []
    for line in text.splitlines():
        m = re.search(r"Mouse\s+#(\d+)", line)
        if m:
            candidates.append((int(m.group(1)), line))

    if not candidates:
        raise RuntimeError("no QEMU mouse")

    for index, line in candidates:
        if "hid mouse" in line.lower() or (
            "usb" in line.lower() and "mouse" in line.lower()
        ):
            hmp.command(f"mouse_set {index}")
            return index

    index = candidates[-1][0]
    hmp.command(f"mouse_set {index}")
    return index


def send_mouse_events(hmp, count=8):
    direction = 1
    for i in range(count):
        dx = 3 * direction
        dy = 1 if (i & 1) else -1
        hmp.command(f"mouse_move {dx} {dy}", timeout=1.0, quiet=True)
        direction = -direction
        time.sleep(0.006)


def read_trace_lines():
    try:
        lines = RUNNER_STDERR.read_text(errors="replace").splitlines()
    except FileNotFoundError:
        return []

    result = []
    for line in lines:
        if any(name in line for name in TRACE_EVENTS):
            result.append(line)
    return result


def print_trace_since(start_count):
    lines = read_trace_lines()
    print()
    print("=== QEMU TRACE AFTER DEVICE_DEL ===")
    new = lines[start_count:]
    if not new:
        print("(no selected QEMU trace events)")
    else:
        for line in new[-300:]:
            print(line)


def main():
    BUILD.mkdir(parents=True, exist_ok=True)

    for path in (MONITOR, RUNNER_STDOUT, RUNNER_STDERR, SERIAL):
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
                "--usb-nested",
                "--monitor-socket", str(MONITOR),
                "--seconds", "55",
                "--no-build",
            ],
            cwd=ROOT,
            stdout=out,
            stderr=err,
        )

        if not wait_until(lambda: MONITOR.exists(), "HMP socket online", 12.0):
            return 2

        deadline = time.monotonic() + 5.0
        while hmp is None:
            try:
                hmp = HMP(MONITOR)
            except OSError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.05)

        if not wait_until(
            lambda: "LITEOS_XHCI_HUB_RUNTIME_OK" in serial_text(),
            "Hub runtime ready",
            25.0,
        ):
            return 3

        if not wait_until(
            lambda: "LITEOS_USB_DEVICE_COUNT=3" in serial_text(),
            "nested topology published",
            20.0,
        ):
            return 4

        print()
        print("=== ENABLE QEMU USB/xHCI TRACE ===")
        for event in TRACE_EVENTS:
            hmp.command(f"trace-event {event} on", quiet=True)
            print("TRACE ON:", event)

        choose_usb_mouse(hmp)
        send_mouse_events(hmp, 8)

        if not wait_until(
            lambda: "LITEOS_USB_MOUSE_EVENT_OK" in serial_text(),
            "initial nested mouse works",
            5.0,
        ):
            return 5

        # Let stderr writes become visible before taking the trace baseline.
        time.sleep(0.1)
        before_lines = read_trace_lines()
        baseline = len(before_lines)

        print()
        print("=== BEFORE DEVICE_DEL ===")
        hmp.command("info usb")

        print()
        print("=== DEVICE_DEL ===")
        hmp.command("device_del liteos-nested-mouse")

        time.sleep(0.25)

        print()
        print("=== 250ms AFTER DEVICE_DEL ===")
        hmp.command("info usb")

        changed = wait_until(
            lambda: "LITEOS_USB_RUNTIME_HUB_CHANGE" in serial_text(),
            "guest runtime Hub change",
            5.0,
        )

        # Give timed interrupt-IN retry a little more time even when guest did
        # not observe the change.
        time.sleep(1.0)
        print_trace_since(baseline)

        print()
        print("=== GUEST DIAG AFTER DELETE ===")
        s = serial_text()
        for line in s.splitlines():
            if (
                "LITEOS_DIAG_XHCI_" in line
                or "LITEOS_DIAG_HUB_" in line
                or "LITEOS_USB_RUNTIME_HUB_CHANGE" in line
            ):
                print(line)

        print()
        print("=== CLASSIFICATION HINT ===")
        new_trace = "\n".join(read_trace_lines()[baseline:])

        if "usb_hub_detach" not in new_trace:
            print("NO usb_hub_detach: QEMU device deletion did not reach Hub detach.")
        elif "usb_xhci_ep_kick" not in new_trace:
            print("Hub detach happened, but xHCI endpoint was not kicked.")
            print("Strong suspect: xhci_wakeup_endpoint Slot/address lookup.")
        elif "usb_xhci_xfer_retry" not in new_trace:
            print("xHCI endpoint kick happened, but no pending retry transfer existed.")
            print("Strong suspect: controller-side Hub interrupt-IN retry ownership.")
        elif "usb_hub_status_report" not in new_trace:
            print("Retry happened, but Hub did not return a change bitmap.")
            print("Strong suspect: wPortChange timing/ACK.")
        elif "usb_xhci_xfer_success" not in new_trace:
            print("Hub reported change, but xHCI transfer did not complete successfully.")
        elif "usb_xhci_queue_event" not in new_trace:
            print("Transfer completed but no xHCI Event TRB was queued.")
        elif "usb_xhci_irq_msix" not in new_trace:
            print("Event TRB was queued but QEMU did not issue MSI-X.")
        else:
            print("QEMU completed the entire path through MSI-X.")
            print("If guest still missed it, return to guest ISR/vector handling.")

        return 0 if changed else 1

    finally:
        if hmp is not None:
            try:
                hmp.command("quit", timeout=0.5, quiet=True)
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
