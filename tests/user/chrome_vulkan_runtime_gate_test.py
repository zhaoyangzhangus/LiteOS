"""Exercise the actual pre-deployment Vulkan gate against the guest ELF tree."""
from contextlib import redirect_stdout
import io
from pathlib import Path
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'tools/stage-chrome.sh').read_text()
start = source.index('import importlib.util\n')
end = source.index('\nPY\n', start)
gate = compile(source[start:end], 'actual-chrome-vulkan-runtime-gate', 'exec')
actual = root/'build/esp/compat/linux/lib/x86_64-linux-gnu'
assert actual.is_dir(), 'build the canonical Vulkan runtime before this test'
previous_argv = sys.argv
try:
    with tempfile.TemporaryDirectory(prefix='chrome-vulkan-runtime-gate-') as directory:
        esp = Path(directory)
        runtime = esp/'compat/linux/lib/x86_64-linux-gnu'
        runtime.mkdir(parents=True)
        # Only fixture links are removed below; the canonical runtime is read-only.
        for path in actual.iterdir():
            if path.is_file():
                (runtime/path.name).symlink_to(path)
        icd = esp/'usr/share/vulkan/icd.d/nvidia_icd.json'
        icd.parent.mkdir(parents=True)
        correct = (root/'build/esp/usr/share/vulkan/icd.d/nvidia_icd.json').read_bytes()
        icd.write_bytes(correct)
        sys.argv = ['gate', str(root), str(esp)]
        def run():
            with redirect_stdout(io.StringIO()) as output:
                exec(gate, {})
            assert output.getvalue().startswith('CHROME_VULKAN_RUNTIME_PASS ')
        def rejected():
            try:
                run()
            except (SystemExit, RuntimeError, ValueError, OSError):
                return
            raise AssertionError('invalid driver tree reached deployment')
        run()
        icd.write_text('{"ICD":{"library_path":"/host/driver.so"}}')
        rejected()
        icd.write_text('{malformed')
        rejected()
        icd.unlink()
        rejected()
        icd.write_bytes(correct)
        for name in ('libvulkan.so.1', 'libGLX_nvidia.so.0',
                     'libnvidia-gpucomp.so.595.84', 'libc.so.6'):
            (runtime/name).unlink()
            rejected()
            (runtime/name).symlink_to(actual/name)
        run()
finally:
    sys.argv = previous_argv
print('Chrome Vulkan runtime gate: real ELF closure, wrong/malformed/missing ICD, '
      'missing loader/driver/dlopen module/transitive libc and restored tree passed')
