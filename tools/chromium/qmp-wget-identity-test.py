"""Verify deployed wget's neutral identity and downloaded bytes via loopback."""
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from qmp_client import QMPClient
from qemu_frame import read_qemu_frame

received = threading.Event()
identities = []
body = b'wget probe ok'

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        identities.append((self.path, self.headers.get('User-Agent')))
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        received.set()

    def log_message(self, *_):
        pass

server = HTTPServer(('127.0.0.1', 0), Handler)
worker = threading.Thread(target=lambda: server.serve_forever(poll_interval=.1), daemon=True)
worker.start()
try:
    with tempfile.TemporaryDirectory(prefix='wget-guest-qa-') as directory, QMPClient() as q:
        assert q.command('query-status')['running']

        def enter(text):
            for char in text:
                if char == ':':
                    q.key('shift', 'semicolon')
                elif char.isupper():
                    q.key('shift', char.lower())
                else:
                    q.key({' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus'}.get(char, char))
            q.key('ret')
            time.sleep(.4)

        path = '/tmp/wget-qa-' + str(time.monotonic_ns())
        enter('wget -O ' + path + ' http://10.0.2.2:' + str(server.server_port) + '/qa')
        assert received.wait(15), 'Guest wget did not reach the loopback probe'
        assert identities == [('/qa', 'wget/1.0')], identities
        time.sleep(.8)
        frame_path = Path(directory) / 'frame.ppm'

        def frame():
            q.command('screendump', {'filename': str(frame_path)})
            return read_qemu_frame(frame_path)

        enter('clear')
        enter('cat ' + path)
        actual = frame()
        q.command('screendump', {'filename': str(ROOT / 'build/wget-neutral-identity.png'), 'format': 'png'})
        enter('clear')
        enter('echo ' + body.decode())
        expected = frame()

        def row(pixels):
            return b''.join(pixels[(y * 1920 + 250) * 3:(y * 1920 + 900) * 3]
                            for y in range(195, 212))

        assert row(expected) != b'\xff' * (650 * 17 * 3), 'Reference output is blank'
        assert row(actual) == row(expected), 'Downloaded file bytes differ from the probe response'
        enter('rm ' + path)
        enter('clear')
        print('QEMU wget: loopback HTTP, deployed User-Agent wget/1.0 and exact downloaded output PASS')
finally:
    server.shutdown()
    server.server_close()
    worker.join(timeout=2)
