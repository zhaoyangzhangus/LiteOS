"""Keep experimental GPU phase object paths and ownership checks aligned."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT/'tools/nvidia'
for path in TOOLS.glob('*.ps1'):
    text = path.read_text()
    if path.name != 'phase6_gate.ps1':
        assert not re.search(r'liteos', text, re.I), path.name
    assert 'LITEOS_NVKMS_PORT' not in text, path.name

phase_sources = {phase: (TOOLS/f'{phase}_build.ps1').read_text()
                 for phase in ('phase8', 'phase9', 'phase10', 'phase11',
                               'phase12', 'phase-w3', 'cube_desktop')}
for phase, source in phase_sources.items():
    directory = 'cube-desktop' if phase == 'cube_desktop' else phase
    assert re.search(r"\$\w+SystemRoot = Join-Path \$\w+Root 'system'", source), phase
    assert f"'BUILD=build/nvidia/{directory}/system'" in source, phase

pairings = {'phase9': 'phase8', 'phase10': 'phase9', 'phase11': 'phase9',
            'phase12': 'phase9', 'phase-w3': 'phase9', 'cube_desktop': 'phase12'}
for consumer, producer in pairings.items():
    source = phase_sources[consumer]
    patterns = re.findall(r"\$row -match '([^']+)'", source)
    patterns = [p for p in patterns if f'build/nvidia/{producer}/system/kernel/' in p]
    assert patterns, consumer
    for pattern in patterns:
        for leaf in ('entry.o', 'linux-abi.o', 'graphics-compositor.o'):
            row = f'C:/workspace/build/nvidia/{producer}/system/kernel/{leaf}'
            match = re.search(pattern, row)
            assert match and match[1] == leaf, (consumer, row)
            assert not re.search(pattern, row.replace('/system/', '/liteos/'))
            assert not re.search(pattern, row + '.old')

nvkms = (TOOLS/'nvkms_build.ps1').read_text()
w3_gate = (TOOLS/'phase-w3_gate.ps1').read_text()
for variable, filename in (('systemGlueObject', 'nv_nvkms_os.o'),
                           ('systemBackendObject', 'nv_nvkms_backend.o')):
    assert f"${variable} = Join-Path $outputRoot 'system\\{filename}'" in nvkms
    assert f"'/system/{filename}'" in w3_gate
assert "$systemFlags.Add('-DOS_KERNEL_BUILD')" in nvkms
assert "'system-nvkms-os-glue-v1'" in nvkms

phase6 = (TOOLS/'phase6_gate.ps1').read_text()
definition = phase6[phase6.index('$forbiddenSourcePattern ='):
                    phase6.index('if ($sourceText -match $forbiddenSourcePattern)')]
pattern = ''.join(re.findall(r"'([^']+)'", definition))
for prefix in ('os', 'system', 'liteos', 'OS', 'SYSTEM'):
    for operation in ('vram_alloc_physical', 'gmmu_map', 'gsp_rpc_alloc',
                      'gb205_channel_program'):
        for whitespace in ('', ' ', '\n'):
            assert re.search(pattern, prefix + '_' + operation + whitespace + '(')
        assert not re.search(pattern, 'not_' + prefix + '_' + operation + '(')
        assert not re.search(pattern, prefix + '_' + operation + '_extra(')
assert not re.search(pattern, 'rm_api->MapToCpu(client, memory);')
print('NVIDIA stage namespace: seven build roots, six producer/consumer path pairs, '
      'NVKMS object/signature agreement, obsolete flag removal and multi-prefix ownership guard passed (Windows source checks only)')
