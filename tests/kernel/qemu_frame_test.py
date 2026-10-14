"""Actual shared frame reader: exact pixels, malformed frames and bounded IO."""
import ast
import importlib.util
import io
from pathlib import Path
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('qemu_frame', ROOT / 'tools/qemu_frame.py')
module = importlib.util.module_from_spec(spec)
with mock.patch.object(Path, 'open', side_effect=AssertionError('import must not read files')):
    spec.loader.exec_module(module)

HEADER = b'P6\n1920 1080\n255\n'
PIXELS = bytes(range(256)) * (module.RGB_BYTES // 256) + bytes(range(module.RGB_BYTES % 256))


class FrameTests(unittest.TestCase):
    def read(self, contents):
        stream = io.BytesIO(contents)
        with mock.patch.object(Path, 'open', return_value=stream):
            try:
                return module.read_qemu_frame('frame.ppm')
            finally:
                self.assertTrue(stream.closed)

    def test_exact_bytes_including_whitespace_and_hash_pixels(self):
        self.assertEqual(self.read(HEADER + PIXELS), PIXELS)
        self.assertEqual(self.read(b'P6\r\n1920\t1080\r\n255\r\n' + PIXELS), PIXELS)

    def test_bad_headers(self):
        for header in (b'', b'P3\n1920 1080\n255\n', b'P6\n1080 1920\n255\n',
                       b'P6\n1920 1080 1\n255\n', b'P6\n999999999 1080\n255\n',
                       b'P6\n1920 1080\n65535\n', b'P6\n1920 1080\n',
                       b'P6' + b' ' * 256 + b'\n1920 1080\n255\n'):
            with self.subTest(header=header[:40]), self.assertRaises(ValueError):
                self.read(header)

    def test_truncated_or_trailing_payload(self):
        for pixels in (b'', PIXELS[:-1], PIXELS + b'\n', PIXELS + b'P6\n'):
            with self.subTest(length=len(pixels)), self.assertRaises(ValueError):
                self.read(HEADER + pixels)

    def test_all_reads_are_bounded_and_file_is_closed(self):
        stream = mock.MagicMock()
        stream.__enter__.return_value = stream
        stream.readline.side_effect = [b'P6\n', b'1920 1080\n', b'255\n']
        stream.read.return_value = PIXELS
        with mock.patch.object(Path, 'open', return_value=stream):
            self.assertEqual(module.read_qemu_frame('frame.ppm'), PIXELS)
        self.assertEqual(stream.readline.call_args_list, [mock.call(256)] * 3)
        stream.read.assert_called_once_with(module.RGB_BYTES + 1)
        stream.__exit__.assert_called_once()

    def test_canonical_scripts_delegate_without_duplicated_parser(self):
        for relative in ('chromium/qmp-gshell-launch-test.py',
                         'chromium/qmp-gshell-viewport-test.py', 'chromium/qmp-taskmgr-test.py',
                         'nvidia/qmp-cube-drag-test.py', 'nvidia/qmp-window-resize-test.py'):
            with self.subTest(script=relative):
                tree = ast.parse((ROOT / 'tools' / relative).read_text())
                self.assertTrue(any(isinstance(node, ast.ImportFrom) and
                                    node.module == 'qemu_frame' for node in ast.walk(tree)))
                calls = [node for node in ast.walk(tree) if isinstance(node, ast.Call)]
                self.assertEqual(sum(isinstance(node.func, ast.Name) and
                                     node.func.id == 'read_qemu_frame' for node in calls), 1)
                self.assertFalse(any(isinstance(node.func, ast.Attribute) and
                                     node.func.attr == 'readline' for node in calls))


if __name__ == '__main__':
    unittest.main()
