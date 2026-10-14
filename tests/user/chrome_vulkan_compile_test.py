"""Compile Vulkan integration using the canonical Chromium target flags.

Force only generated feature macros for syntax checks; never regenerate GN,
replace build products or imply that the running browser enabled Vulkan.
"""
from pathlib import Path
import argparse
import re
import shlex
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source
root = chromium_source()
out = root/'out/liteos-debian'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--runtime', choices=('system', 'glibc'), default='system')
parser.add_argument('--gpu-init-only', action='store_true')
parser.add_argument('--vulkan-only-context', action='store_true')
options = parser.parse_args()
runtime = options.runtime


def compile_source(target, source):
    lines = subprocess.check_output(
        ['ninja', '-C', str(out), '-t', 'commands', '-s', target],
        text=True, timeout=15).splitlines()
    argv = shlex.split(lines[-1])
    if Path(argv[0]).name == 'sccache':
        argv = argv[1:]
    args = []
    index = 0
    while index < len(argv):
        argument = argv[index]
        if argument in ('-o', '-MF'):
            index += 2
            continue
        if (argument not in ('-c', '-MMD', '-MD',
                             '-D_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE')
                and not argument.startswith('--native-os-glibc-root=')):
            args.append(source if argument.endswith('.cc') else argument)
        index += 1
    args += ['-fsyntax-only', '-DGPU_VULKAN_BUILDFLAGS_H_',
             '-DUI_GL_BUILDFLAGS_H_', '-DSK_VULKAN',
             '-I../../third_party/vulkan-headers/src/include']
    if runtime == 'glibc':
        guest = Path(__file__).resolve().parents[2]/'build/nvidia/host-deps/glibc'
        args.append('--native-os-glibc-root='+str(guest))
    else:
        args.append('-D_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE')
    macros = re.findall(r'#define (BUILDFLAG_INTERNAL_\w+)\(\) \((\d+)\)',
                        (out/'gen/ui/gl/buildflags.h').read_text())
    assert any(name == 'BUILDFLAG_INTERNAL_ENABLE_VULKAN' for name, _ in macros)
    for name, value in macros:
        if name == 'BUILDFLAG_INTERNAL_ENABLE_VULKAN':
            value = '1'
        args.append('-D' + name + '()=(' + value + ')')
    subprocess.run(args, cwd=out, check=True, timeout=40)
    print('VULKAN_ENABLED_SYNTAX_PASS', runtime, source, flush=True)


native = 'ui/ozone/platform/nativeguest/'
viz = 'components/viz/service/display_embedder/'
factory = 'obj/ui/ozone/platform/nativeguest/nativeguest/nativeguest_surface_factory.o'
if options.vulkan_only_context:
    for target, source in (
        ('obj/gpu/ipc/service/service/gpu_channel_manager.o',
         '../../gpu/ipc/service/gpu_channel_manager.cc'),
        ('obj/gpu/command_buffer/service/gles2_sources/shared_context_state.o',
         '../../gpu/command_buffer/service/shared_context_state.cc'),
        ('obj/gpu/command_buffer/service/gles2_sources/feature_info.o',
         '../../gpu/command_buffer/service/feature_info.cc'),
        ('obj/gpu/command_buffer/service/gles2_sources/shared_image_factory.o',
         '../../gpu/command_buffer/service/shared_image/shared_image_factory.cc'),
        ('obj/gpu/command_buffer/service/gles2_sources/external_vk_image_backing_factory.o',
         '../../gpu/command_buffer/service/shared_image/external_vk_image_backing_factory.cc'),
        ('obj/gpu/command_buffer/service/gles2_sources/external_vk_image_backing.o',
         '../../gpu/command_buffer/service/shared_image/external_vk_image_backing.cc'),
    ):
        compile_source(target, source)
    raise SystemExit(0)
if options.gpu_init_only:
    compile_source('obj/gpu/ipc/service/service/gpu_init.o',
                   '../../gpu/ipc/service/gpu_init.cc')
    compile_source('obj/components/viz/service/service/gpu_service_impl.o',
                   '../../components/viz/service/gl/gpu_service_impl.cc')
    raise SystemExit(0)
for source in ('nativeguest_surface_factory.cc', 'nativeguest_vulkan_implementation.cc'):
    compile_source(factory, '../../' + native + source)
compile_source('obj/components/viz/service/service/skia_output_device_offscreen.o',
               '../../' + viz + 'skia_output_device_vulkan_canvas.cc')
compile_source('obj/components/viz/service/service/skia_output_surface_impl_on_gpu.o',
               '../../' + viz + 'skia_output_surface_impl_on_gpu.cc')
