#pragma once

/* ELF libc shares the Linux x86-64 wchar ABI with Chromium/libc++.
 * PE/UEFI code keeps its separate 16-bit wide-character convention. */
#if !defined(__ASSEMBLER__) && __SIZEOF_WCHAR_T__ != 4
#error "System ELF user ABI requires 32-bit wchar_t"
#endif
