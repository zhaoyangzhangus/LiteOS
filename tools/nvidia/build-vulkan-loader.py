#!/usr/bin/env python3
"""Build Khronos' loader against the guest glibc baseline, not host libc."""
import argparse
import importlib.util
import os
import shutil
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('vulkan_abi', ROOT/'tools/nvidia/vulkan_runtime_abi.py')
abi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(abi)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', default='build', type=Path)
    parser.add_argument('--chromium-src', default=os.environ.get('CHROMIUM_SRC'), type=Path)
    parser.add_argument('--jobs', default=2, type=int)
    args = parser.parse_args()
    if args.build.is_absolute() or '..' in args.build.parts or args.jobs < 1:
        parser.error('BUILD must stay inside the project; jobs must be positive')
    if args.chromium_src is None:
        parser.error('CHROMIUM_SRC or --chromium-src is required')
    source = args.chromium_src.resolve()
    headers = source/'third_party/vulkan-headers/src'
    loader = source/'third_party/vulkan-loader/src'
    for directory in (headers, loader):
        if not (directory/'CMakeLists.txt').is_file():
            parser.error('missing Khronos source: '+str(directory))
    build = ROOT/args.build
    out = build/'nvidia/vulkan'
    sdk = out/'sdk'
    dev = build/'nvidia/host-deps/glibc/libc6-dev-extract/usr'
    runtime = build/'nvidia/host-deps/glibc/libc6-extract/lib/x86_64-linux-gnu'
    libraries = [runtime/name for name in ('libc.so.6', 'libdl.so.2', 'libpthread.so.0')]
    for path in [dev/'include/stdlib.h', *libraries]:
        if not path.is_file():
            parser.error('missing guest baseline: '+str(path))
    def run(command):
        subprocess.run(command, cwd=ROOT, check=True)
    run(['cmake', '-S', str(headers), '-B', str(out/'headers'),
         '-DCMAKE_INSTALL_PREFIX='+str(sdk), '-DVULKAN_HEADERS_ENABLE_TESTS=OFF'])
    run(['cmake', '--install', str(out/'headers')])
    flags = f'-isystem {dev}/include -isystem {dev}/include/x86_64-linux-gnu'
    link = '-nodefaultlibs -Wl,--no-as-needed,-z,defs,-rpath-link,'+str(runtime)+' '+' '.join(map(str, libraries))
    run(['cmake', '-S', str(loader), '-B', str(out/'loader'), '-G', 'Unix Makefiles',
         '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_PREFIX_PATH='+str(sdk),
         '-DCMAKE_C_FLAGS='+flags, '-DCMAKE_SHARED_LINKER_FLAGS='+link,
         '-DBUILD_TESTS=OFF', '-DBUILD_WSI_XCB_SUPPORT=OFF', '-DBUILD_WSI_XLIB_SUPPORT=OFF',
         '-DBUILD_WSI_XLIB_XRANDR_SUPPORT=OFF', '-DBUILD_WSI_WAYLAND_SUPPORT=OFF',
         '-DBUILD_WSI_DIRECTFB_SUPPORT=OFF'])
    run(['cmake', '--build', str(out/'loader'), '--parallel', str(args.jobs)])
    candidates = list((out/'loader/loader').glob('libvulkan.so.*'))
    if not candidates:
        raise RuntimeError('Khronos loader build produced no shared library')
    for candidate in candidates:
        dynamic = subprocess.check_output(['readelf', '-d', str(candidate)], text=True,
                                          env=dict(os.environ, LC_ALL='C'))
        if '[libc.so.6]' not in dynamic or not abi.symbols(candidate, True):
            raise RuntimeError('loader must resolve its imports against the guest libc')
    abi.check(runtime, candidates)
    license_dir = out/'licenses/loader'
    license_dir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(loader/'LICENSE.txt', license_dir/'LICENSE.txt')
    shutil.copytree(loader/'LICENSES', license_dir/'LICENSES', dirs_exist_ok=True)
    print('VULKAN_LOADER_ABI_READY='+str(out/'loader/loader/libvulkan.so.1'), flush=True)


if __name__ == '__main__':
    main()
