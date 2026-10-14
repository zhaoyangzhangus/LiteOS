"""Use actual versioned ELF fixtures, including a too-new dependency."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('abi', ROOT/'tools/nvidia/vulkan_runtime_abi.py')
abi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(abi)
with tempfile.TemporaryDirectory(prefix='vulkan-abi-') as directory:
    base = Path(directory)
    runtime = base/'runtime'
    runtime.mkdir()
    (base/'provider.c').write_text('int fixture(void) { return 7; }\n')
    for version, location in (('2.36', runtime/'libc.so.6'), ('2.38', base/'new-provider.so')):
        script = base/(version+'.map')
        script.write_text('GLIBC_'+version+' { global: fixture; local: *; };\n')
        subprocess.run(['cc', '-shared', '-fPIC', '-nostdlib', str(base/'provider.c'),
                        '-Wl,--version-script='+str(script), '-o', str(location)], check=True)
    (base/'consumer.c').write_text('extern int fixture(void); int use(void) { return fixture(); }\n')
    for name, provider in (('old', runtime/'libc.so.6'), ('new', base/'new-provider.so')):
        consumer = base/(name+'.so')
        subprocess.run(['cc', '-shared', '-fPIC', '-nostdlib', str(base/'consumer.c'),
                        str(provider), '-o', str(consumer)], check=True)
    abi.check(runtime, [base/'old.so'])
    try:
        abi.check(runtime, [base/'new.so'])
        raise AssertionError('accepted a missing GLIBC symbol version')
    except ValueError as error:
        assert 'fixture@GLIBC_2.38' in str(error)
    try:
        abi.check(base/'missing', [base/'old.so'])
        raise AssertionError('accepted missing runtime')
    except ValueError:
        pass
print('Vulkan ABI: actual versioned ELF imports/exports, baseline acceptance, '
      'new-version rejection and missing runtime rejection passed')
