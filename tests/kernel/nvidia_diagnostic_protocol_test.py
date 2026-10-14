"""Freeze NVIDIA private constants and fallback linkage during log renaming."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
driver = (ROOT / 'kernel/drivers/gpu/nvidia/rm/nv_rm_driver.c').read_text()
bridge = (ROOT / 'kernel/drivers/gpu/nvidia/rm/nv_rm_vendor_bridge.c').read_text()
phase6 = (ROOT / 'kernel/drivers/gpu/nvidia/rm/nv_rm_phase6.c').read_text()
paths = [ROOT / 'kernel/drivers/display/core.c', ROOT / 'kernel/graphics/compositor.c',
         ROOT / 'kernel/init/devices.c', ROOT / 'kernel/init/runtime.c']
paths += list((ROOT / 'kernel/drivers/gpu/nvidia').rglob('*.c'))
paths += list((ROOT / 'kernel/drivers/display/nvidia').rglob('*.c'))
paths += list((ROOT / 'tools/nvidia').glob('*.ps1'))
for path in paths:
    text = path.read_text()
    assert 'LITEOS_NVIDIA_' not in text, path
    assert 'LITEOS_NVKMS_' not in text, path
assert 'OS_NVIDIA_GSP_BOOT_FINISH status=ready' in bridge
assert 'OS_NVIDIA_PHASE4_HAL_READY BLACKWELL GB205' in driver
for path in ('phase5_gate.ps1', 'phase5_hardware_gate_loop.ps1'):
    text = (ROOT / 'tools/nvidia' / path).read_text()
    assert 'OS_NVIDIA_GSP_BOOT_FINISH status=ready' in text
    assert 'OS_NVIDIA_GSP_READY' in text
for marker in ('NATIVE_COMMIT_BEGIN', 'NATIVE_COMMIT_SUBMIT_FAIL',
               'NATIVE_COMMIT_QUEUED', 'NATIVE_FLIP_OK', 'NATIVE_FLIP_FAIL'):
    assert 'OS_NVKMS_' + marker in (ROOT / 'kernel/drivers/display/core.c').read_text()
constants = '\n'.join(line for text in (bridge, phase6) for line in text.splitlines()
                      if line.startswith('#define NV_RM_MSIX_ENTRY_LIMIT') or
                         line.startswith('#define NV_RM_PHASE6_'))
weak = driver[driver.index('#if defined(__MINGW32__)'):driver.index('enum nv_rm_failure_stage')]
code = constants + '\n' + weak + r'''
_Static_assert(NV_RM_MSIX_ENTRY_LIMIT == 8, "vendor MSI-X cap");
_Static_assert(NV_RM_PHASE6_SMOKE_SIZE == 4096, "smoke bytes");
_Static_assert(NV_RM_PHASE6_SMOKE_ALIGNMENT == 4096, "smoke alignment");
_Static_assert(NV_RM_PHASE6_LARGE_SIZE == 16ULL * 1024 * 1024, "large allocation");
_Static_assert(NV_RM_PHASE6_CYCLE_COUNT == 20, "lifecycle repetitions");
NV_RM_WEAK void fallback(void) {}
'''
with tempfile.TemporaryDirectory(prefix='nv-diagnostic-protocol-') as tmp:
    fixture = Path(tmp) / 'test.c'; fixture.write_text(code)
    for defines, expected in (([], 'W'), (['-D__MINGW32__'], 'T'), (['-D__MINGW64__'], 'T')):
        obj = Path(tmp) / 'test.o'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        *defines, '-c', str(fixture), '-o', str(obj)], check=True)
        symbols = subprocess.check_output(['nm', str(obj)], text=True)
        assert re.search(r'\b' + expected + r' fallback$', symbols, re.M), symbols
print('NVIDIA_DIAGNOSTIC_PROTOCOL_TEST_OK')
