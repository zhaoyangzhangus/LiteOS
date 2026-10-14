"""Run the real launcher preflight, without GN, Ninja, or service writes."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'tools/build-chrome.sh').read_text()
validation = source.split("<<'PY'\n", 1)[1].split('\nPY', 1)[0]
with tempfile.TemporaryDirectory(prefix='chrome-sdk-guard-') as directory:
    base = Path(directory)
    args = base/'chromium/out/liteos-debian/args.gn'
    args.parent.mkdir(parents=True)
    sdk = base/'chromium/build/linux/debian_bullseye_amd64-sysroot'
    header = sdk/'usr/include/linux/limits.h'
    header.parent.mkdir(parents=True)
    header.write_text('/* SDK fixture */\n')
    selected = str(base/'project with spaces')
    good = (f'native_os_root = "{selected}"\n'
            f'native_os_build_dir = "{selected}/build"\n'
            'use_sysroot = true\n'
            'sysroot = "//build/linux/debian_bullseye_amd64-sysroot"\n'
            'use_qt5 = false\nuse_qt6 = false\nrtc_use_pipewire = false\n')

    def run(text):
        args.write_text(text)
        return subprocess.run([sys.executable, '-c', validation, str(args), selected],
                              text=True, capture_output=True, timeout=10)

    assert run(good).returncode == 0
    for option in ('use_qt5', 'use_qt6', 'rtc_use_pipewire'):
        for text in (good.replace(option+' = false', option+' = true'),
                     good.replace(option+' = false\n', '')):
            result = run(text)
            assert result.returncode != 0 and option in result.stderr
    assert run(good.replace('use_sysroot = true', 'use_sysroot = false')).returncode
    assert run(good.replace('debian_bullseye_amd64-sysroot', 'host-sdk')).returncode
    header.unlink()
    assert run(good).returncode
print('Actual builder preflight: fixed SDK accepted; implicit/enabled Qt/PipeWire, '
      'host headers and missing SDK rejected before any build')
