"""Actual staging cleanup is exact, repeatable, and does not follow symlinks."""
import importlib.util
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('cleanup', ROOT/'tools/clean-runtime-aliases.py')
cleanup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cleanup)
with tempfile.TemporaryDirectory(prefix='runtime-cleanup-') as directory:
    base = Path(directory)
    esp = base/'esp'
    for relative in cleanup.RETIRED:
        path = esp/relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'old generated alias')
    current = esp/'lib/libsystemc.so.1'
    current.write_bytes(b'current runtime')
    outside = base/'outside'
    outside.write_bytes(b'protected')
    alias = esp/cleanup.RETIRED[0]
    alias.unlink()
    alias.symlink_to(outside)
    assert len(cleanup.clean(esp)) == len(cleanup.RETIRED)
    assert outside.read_bytes() == b'protected'
    assert current.read_bytes() == b'current runtime'
    assert cleanup.clean(esp) == []
    alias.write_bytes(b'retain on validation failure')
    invalid = esp/cleanup.RETIRED[-1]
    invalid.mkdir()
    try:
        cleanup.clean(esp)
        raise AssertionError('directory alias accepted')
    except ValueError:
        pass
    assert alias.is_file()
    invalid.rmdir()
    (esp/'lib').rename(esp/'real-lib')
    (esp/'lib').symlink_to(esp/'real-lib', target_is_directory=True)
    try:
        cleanup.clean(esp)
        raise AssertionError('symlinked staging parent accepted')
    except ValueError:
        pass
    assert (esp/'real-lib'/alias.name).is_file()
make = (ROOT/'makefile').read_text()
assert 'python3 tools/clean-runtime-aliases.py "$(BUILD)/esp"' in make
runner = (ROOT/'run-qemu.sh').read_text()
call = 'python3 "$script_dir/tools/clean-runtime-aliases.py" "$build_dir/esp"'
assert call in runner
assert runner.index(call) < runner.index('materialize_fat_image()')
print('Runtime alias cleanup: exact files, idempotence, current runtime retention, '
      'external symlink protection and validate-before-delete passed')
