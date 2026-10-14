#!/usr/bin/env python3
"""Compile the production platform timeout selection without Chromium deps."""
from chromium_source import chromium_source
import pathlib
import re
import subprocess
import tempfile

source = (chromium_source() / 'content/child/child_thread_impl.cc').read_text()
selection = re.search(r'#if defined\(__NATIVE_OS__\)\n// Cold profile.*?#endif', source, re.S)
assert selection is not None
with tempfile.TemporaryDirectory(prefix='liteos-ipc-timeout-') as directory:
    for defines, expected in [([], 15), (['-D__NATIVE_OS__'], 60)]:
        result = subprocess.run(['c++', '-E', '-P', '-x', 'c++', *defines, '-'],
                                input=selection.group(), text=True,
                                capture_output=True, check=True).stdout
        assert re.search(rf'kConnectionTimeoutS = {expected};', result), result
assert 'GetSwitchValueASCII(\n          switches::kIPCConnectionTimeout)' in source
assert 'channel_connected_factory_.reset();' in source
print('IPC watchdog: LiteOS=60s, other platforms=15s; override and connection cancellation retained')
