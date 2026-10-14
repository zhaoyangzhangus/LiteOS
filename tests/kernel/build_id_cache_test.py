"""Exercise the production build-ID rules without rebuilding guest images."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'makefile').read_text() + '\n' + (ROOT / 'tools/host-tests.mk').read_text()
rules = source.split('$(BUILD)/.host-toolchain:', 1)[1].split(
    '$(KERNEL_SYMBOLS):', 1)[0]
rules = '$(BUILD)/.host-toolchain:' + rules
macro = 'define HOST_TEST_LINK\n' + source.split('define HOST_TEST_LINK\n', 1)[1].split('endef', 1)[0] + 'endef\n'
with tempfile.TemporaryDirectory(prefix='liteos-build-id-cache-') as directory:
    base = Path(directory)
    for name in ['tools/build_id.c', 'boot/uefi/sha256.c',
                 'include/boot/sha256.h', 'include/uefi.h']:
        destination = base / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / name, destination)
    (base / 'input').write_bytes(b'abc')
    shutil.copyfile(ROOT / 'tools/host-tests.mk', base / 'tools/host-tests.mk')
    (base / 'makefile').write_text(
        'BUILD=build\nHOSTCC=gcc\nKERNEL_ELF=input\nKERNEL_BUILD_ID=build/id\n'
        '.PHONY: FORCE\nFORCE:\n'
        'build build/esp/EFI/OS:\n\tmkdir -p $@\n' + macro + rules +
        '\n-include build/build-id.exe.d\n')
    def run(*args):
        return subprocess.run(['make', 'build/id', *args], cwd=base,
                              capture_output=True, text=True, check=True).stdout
    def identity(path):
        stat = path.stat()
        return stat.st_ino, stat.st_mtime_ns, path.read_bytes()
    run()
    tool, build_id = base / 'build/build-id.exe', base / 'build/id'
    assert build_id.read_text().strip() == hashlib.sha256(b'abc').hexdigest()
    before_tool, before_id = identity(tool), identity(build_id)
    run()
    assert identity(tool) == before_tool and identity(build_id) == before_id
    (base / 'input').write_bytes(b'changed')
    run()
    assert identity(tool) == before_tool
    assert build_id.read_text().strip() == hashlib.sha256(b'changed').hexdigest()
    assert 'HOST_TEST_LINK build/build-id.exe' in run('HOSTCC=gcc -DHOST_VARIANT=1')
    (base / 'include/boot/sha256.h').write_text(
        (base / 'include/boot/sha256.h').read_text() + '\n/* header edit */\n')
    assert 'HOST_TEST_LINK build/build-id.exe' in run('HOSTCC=gcc -DHOST_VARIANT=1')
    before_tool, before_id = identity(tool), identity(build_id)
    run('HOSTCC=gcc -DHOST_VARIANT=1')
    assert identity(tool) == before_tool and identity(build_id) == before_id
print('Build-ID cache: no-op, input edit, host toolchain change and header invalidation passed')
