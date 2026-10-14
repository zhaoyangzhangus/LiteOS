"""Exercise the actual graphical launch gate without sending guest inputs."""
import ast
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
source = ast.parse((ROOT / 'tools/chromium/qmp-gshell-launch-test.py').read_text())
function = next(node for node in source.body
                if isinstance(node, ast.FunctionDef) and node.name == 'launch_and_verify')
code = compile(ast.Module(body=[function], type_ignores=[]), '<actual-launch-gate>', 'exec')


class LaunchProgressTests(unittest.TestCase):
    def run_gate(self, frames, background=False, number=1):
        calls = []
        frames = iter(frames)
        def lines():
            calls.append(('frame',))
            return [next(frames), b'prompt']
        namespace = {'lines': lines, 'enter': lambda text: calls.append(('enter', text))}
        exec(code, namespace)
        try:
            namespace['launch_and_verify'](number, b'success', background)
        finally:
            self.calls = calls

    def test_each_launch_requires_visible_probe_then_new_success(self):
        for background in (False, True):
            for number in range(1, 257):
                self.run_gate([b'success', f'probe {number}'.encode(), b'success'],
                              background, number)
                self.assertEqual(self.calls, [
                    ('frame',), ('enter', f'echo LAUNCH PROBE {number}'),
                    ('frame',), ('enter', 'nasm -v &' if background else 'nasm -v'),
                    ('frame',)])

    def test_previous_success_cannot_pass_a_frozen_frame(self):
        for background in (False, True):
            with self.assertRaisesRegex(AssertionError, 'probe did not change'):
                self.run_gate([b'success', b'success', b'success'], background)
            self.assertEqual(len(self.calls), 3)  # Do not launch after a stuck probe.

    def test_freeze_after_probe_and_failed_exit_are_rejected(self):
        for result in (b'probe', b'failed exit', b'wrong status', b''):
            with self.assertRaisesRegex(AssertionError, 'expected status missing'):
                self.run_gate([b'success', b'probe', result])

    def test_missing_initial_status_does_not_send_input(self):
        with self.assertRaisesRegex(AssertionError, 'initial status missing'):
            self.run_gate([b'wrong shell'])
        self.assertEqual(self.calls, [('frame',)])


if __name__ == '__main__':
    unittest.main()
