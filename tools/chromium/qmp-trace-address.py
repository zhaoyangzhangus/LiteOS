"""Trace one calibrated address click in the existing QEMU; always resume it."""
from pathlib import Path
import subprocess
import time

root = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
if not (root/'build/qemu-running-kernel.elf').is_file():
    raise RuntimeError('Missing symbols for the running guest kernel')
qmp = QMPClient()

def command(name, arguments=None):
    return qmp.command(name, arguments)

def move(x, y):
    while x or y:
        dx, dy = max(-100, min(100, x)), max(-100, min(100, y))
        command('input-send-event', {'events': [
            {'type': 'rel', 'data': {'axis': 'x', 'value': dx}},
            {'type': 'rel', 'data': {'axis': 'y', 'value': dy}}]})
        x, y = x - dx, y - dy
        time.sleep(.1)

def click():
    for down in (True, False):
        command('input-send-event', {'events': [
            {'type': 'btn', 'data': {'button': 'left', 'down': down}}]})
        time.sleep(.04)


if not command('query-status')['running']:
    raise RuntimeError('Guest was already stopped; do not alter its debugger state')
move(-2000, -1200)
move(920, 400)
click()
time.sleep(1)
move(-520, -328)
process = None
try:
    command('human-monitor-command', {'command-line':
        'gdbserver unix:/tmp/system-address-fork.sock,server=on,wait=off'})
    with (root / 'build/chrome-address-fork.log').open('w') as log:
        process = subprocess.Popen(['gdb', '-q', '-batch',
            'build/qemu-running-kernel.elf', '-x',
            'tools/chromium/trace-address-fork.gdb'], cwd=root,
            stdout=log, stderr=subprocess.STDOUT)
        time.sleep(1)
        for attempt in range(60):
            if command('query-status')['running']:
                break
            time.sleep(.1)
        else:
            raise RuntimeError('Debugger did not resume guest')
        click()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=3)
finally:
    if process and process.poll() is None:
        process.terminate()
        process.wait(timeout=3)
    command('human-monitor-command', {'command-line': 'gdbserver none'})
    command('cont')
    print(command('query-status'))
