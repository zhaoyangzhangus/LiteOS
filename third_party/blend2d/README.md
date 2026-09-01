# Blend2D for LiteOS

This directory vendors Blend2D 0.21.2 from the upstream master commit
`6dbc2cefbc996379e07104e34519a440b49b15d7`. LiteOS builds the C API as a
freestanding static library with the x86-64 JIT and SSE2 baseline enabled.
TLS, futex, exceptions, RTTI, and the C++ standard library are disabled.

The small headers under `liteos/cxx` provide the C++ language-library pieces
used by Blend2D (`atomic`, type traits, limits, math, and utility) without
linking libstdc++. Headers under `liteos/include` give the sources C linkage
and the LiteOS ABI for allocation, time, files, and pthread declarations.

The public API remains the upstream C API and is available to ELF user
programs with `#include <blend2d/blend2d.h>`. Link those programs with
`/lib/libliteosgfx.so.1`; the build exports the complete compiled C API by
linking the Blend2D archive with `--whole-archive`. The LiteOS adapter
`user/runtime/libliteos_gfx.c` additionally provides a small caller-owned
XRGB8888 convenience ABI.

AsmJit is vendored under `3rdparty/asmjit` and is built into the same archive.
`BLEND2D_ENABLE_JIT=0` selects the reference-pipeline fallback for constrained
builds. LiteOS removes Blend2D's process auto-initializer because the dynamic
loader establishes TLS after DSO mapping; callers initialize and shut down the
runtime explicitly through `bl_runtime_init()` and `bl_runtime_shutdown()`.
The VFS preserves unlinked temporary files for live mmap objects, as required
by AsmJit's dual RW/RX mapping strategy. `liteos/compiler.c` supplies the
compiler helper missing from the freestanding x86_64 toolchain. See `LICENSE.md`
for the Zlib license and
https://github.com/blend2d/blend2d for the upstream project.
