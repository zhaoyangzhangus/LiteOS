#pragma once

#include <elf.h>

/* The System ELF declarations live in elf.h; this header supplies the
 * glibc-compatible include path and ElfW() macro expected by POSIX code. */
#ifndef ElfW
#define ElfW(type) Elf64_##type
#endif
