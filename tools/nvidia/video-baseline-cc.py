#!/usr/bin/env python3
"""Build guest video tools against the same private glibc baseline as Vulkan."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
dev = root/'build/nvidia/host-deps/glibc/libc6-dev-extract/usr'
runtime = root/'build/nvidia/host-deps/glibc/libc6-extract/lib/x86_64-linux-gnu'
sdk = root/'build/nvidia/vulkan/sdk/include'
args = sys.argv[1:]
command = ['x86_64-linux-gnu-gcc', '-isystem', str(dev/'include'),
           '-isystem', str(dev/'include/x86_64-linux-gnu'), '-I', str(sdk)]
if any(x in args for x in ('-c', '-E', '-S', '--version', '-v')):
    command += args
else:
    lib = dev/'lib/x86_64-linux-gnu'
    shared = '-shared' in args
    command += ['-nostdlib', '-nodefaultlibs', '-nostartfiles']
    if not shared:
        command += ['-no-pie', '-Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2']
    command += [
                '-Wl,-rpath,/compat/linux/lib/x86_64-linux-gnu',
                '-Wl,--disable-new-dtags', '-Wl,-rpath-link,'+str(runtime),
                *([] if shared else [str(lib/'crt1.o')]), str(lib/'crti.o')]
    command += [x for x in args if x not in ('-lm','-ldl','-lpthread','-lc')]
    command += [str(runtime/'libm.so.6'), str(runtime/'libc.so.6'),
                str(lib/'libc_nonshared.a'), '-lgcc', '-lgcc_eh', str(lib/'crtn.o')]
sys.exit(subprocess.call(command))
