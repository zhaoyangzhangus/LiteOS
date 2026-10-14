#!/usr/bin/env python3
"""Check production frame-path log guards in both release and trace builds."""
from chromium_source import chromium_source
import pathlib
import subprocess

root = chromium_source()
groups = {
    'cc/trees/layer_tree_host.cc': (
        'OS_CC_UPDATE_AFTER_HUD',
        'OS_CC_UPDATE_AFTER_PROPERTY_TREES',
        'OS_CC_UPDATE_AFTER_DRAW_PROPERTIES',
        'OS_CC_UPDATE_AFTER_FIND_LAYERS'),
    'ui/compositor/external_begin_frame_adapter.cc': (
        'OS_EXTERNAL_BEGIN_FRAME_CLIENT_CREATE',
        'OS_EXTERNAL_BEGIN_FRAME_ON_BEGIN',
        'OS_EXTERNAL_BEGIN_FRAME_ACK'),
}
blocks = []
for path, markers in groups.items():
    source = (root / path).read_text()
    for marker in markers:
        position = source.index('"' + marker)
        start = source.rfind('#if ', 0, position)
        end = source.index('#endif', position) + len('#endif')
        blocks.append(source[start:end])
assert len(blocks) == 7
for trace, platform, legacy in ((False, True, False), (True, True, False),
                                (False, True, True), (True, False, False)):
    output = subprocess.check_output(
        ['cc', '-E', '-P', '-x', 'c',
         *(['-D__NATIVE_OS__'] if platform else []),
         *(['-DOS_STARTUP_TRACE'] if trace else []),
         *(['-DLITEOS_STARTUP_TRACE'] if legacy else []), '-'],
        input='\n'.join(blocks), text=True)
    for markers in groups.values():
        for marker in markers:
            assert (marker in output) == (trace and platform)
print('frame logs: seven blocks, neutral startup flag, release/legacy/non-platform suppression and explicit trace retention passed')
