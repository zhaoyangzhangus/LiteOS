"""Neutral native test markers and real verifier failure reporting stay aligned."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'user/runtime/libc_test.c').read_text()
assert 'LITEOS_' not in source
assert '"liteos' not in source and '"LiTeOs' not in source
assert 'OS_LIBC_TEST_OK' in source and 'OS_LIBC_TEST_FAIL' in source
assert 'OS_EXEC_ENV=ok' in source and 'getenv("OS_EXEC_ENV")' in source
assert '#define OS_TEST_SSE' in source
assert 'strcpy(source, "system-c-runtime")' in source
assert 'strncmp(copy, "ssystem", 7U)' in source
for file in ('tools/verify-qemu-stages.sh', 'tools/run-qemu-native-matrix.ps1'):
    text = (ROOT / file).read_text()
    assert 'OS_LIBC_TEST_OK' in text and 'LITEOS_LIBC_TEST_OK' not in text
with tempfile.TemporaryDirectory(prefix='native-diagnostics-') as directory:
    log = Path(directory) / 'serial.log'
    for marker in ('OS_LIBC_TEST_FAIL', 'OS_LIBC_TEST_DETAIL_FAIL'):
        log.write_text('OS_LIBC_TEST_OK\n' + marker + ' stage=fixture errno=5\n')
        result = subprocess.run(['bash', str(ROOT / 'tools/verify-qemu-stages.sh'), str(log)],
                                capture_output=True, text=True)
        assert result.returncode == 1
        assert 'native libc test failed' in result.stderr and marker in result.stderr
    log.write_text('OS_LIBC_TEST_FAILURE_INJECTION\n')
    result = subprocess.run(['bash', str(ROOT / 'tools/verify-qemu-stages.sh'), str(log)],
                            capture_output=True, text=True)
    assert result.returncode == 1 and 'native libc test failed' not in result.stderr
    assert 'missing markers' in result.stderr
    failures = {
        'OS_STAGE_FAIL': 'explicit stage failure',
        'OS_DIAG_RT_FAIL': 'dynamic loader failed',
        'OS_LIBC_TEST_FAIL': 'native libc test failed',
        'OS_LIBC_TEST_DETAIL_FAIL': 'native libc test failed',
    }
    for marker, reason in failures.items():
        for prefix in ('', ' ', '[13:42:01] '):
            for suffix in ('', ' phase=fixture status=-5', '\tstatus=-5'):
                log.write_text('OS_LIBC_TEST_OK\n' + prefix + marker + suffix + '\n')
                result = subprocess.run(['bash', str(ROOT / 'tools/verify-qemu-stages.sh'), str(log)],
                                        capture_output=True, text=True)
                assert result.returncode == 1 and reason in result.stderr, (marker, prefix, suffix)
        for near_name in (marker + 'URE_INJECTION', marker + '_DETAIL',
                          'NOT_' + marker, 'prefix' + marker):
            log.write_text(near_name + ' phase=fixture\n')
            result = subprocess.run(['bash', str(ROOT / 'tools/verify-qemu-stages.sh'), str(log)],
                                    capture_output=True, text=True)
            assert result.returncode == 1 and reason not in result.stderr, near_name
            assert 'missing markers' in result.stderr, near_name
print('Native diagnostics: neutral success/error/environment/SSE names, both verifier consumers and failure-before-success gate passed')
