# NASM on LiteOS

The build uses the upstream NASM 3.02 source tree with this fixed feature
configuration.  The target is a freestanding x86-64 ELF executable linked
against the LiteOS dynamic loader and libc; NASM's file, memory-map, time,
stdio, and command-line paths use the existing LiteOS POSIX-shaped ABI.

The LiteOS image ships both the `nasm` assembler and its `ndisasm` companion;
documentation generators and installer packaging remain host-side tools.
