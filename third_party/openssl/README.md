# OpenSSL for LiteOS

This directory contains the OpenSSL 3.5.8 source needed by the LiteOS
freestanding user image. It is built as a static library with DSO loading,
threads, host file I/O, and assembler disabled. The default provider and TLS
1.2/1.3 client remain available to applications.

The source manifest comes from the upstream configuration
`no-shared no-dso no-threads no-async no-stdio no-tests no-apps no-asm
no-engine no-legacy no-comp no-dgram no-sock no-ui-console no-filenames`.

The generated headers are checked in so the normal LiteOS build does not need
Perl or the OpenSSL build host. `include/openssl/e_os2.h` and
`crypto/bn/bn_local.h` contain the small LiteOS platform hooks; transport,
entropy, and clock policy belong to the LiteOS adapter, not this vendor tree.

OpenSSL is distributed under the Apache License 2.0. See `LICENSE.txt`.
