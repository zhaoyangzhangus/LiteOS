#ifndef LITEOS_RSA_H
#define LITEOS_RSA_H

#include "uefi.h"

#define LITEOS_RSA2048_BYTES 256U

/* 解析固定长度的大端十六进制字符串。 */
BOOLEAN rsa2048_parse_hex(const CHAR8 *text, UINTN length,
                          UINT8 output[LITEOS_RSA2048_BYTES]);

/* 使用 Loader 内置的信任公钥验证 SHA-256 的 RSA-2048 PKCS#1 v1.5 签名。 */
BOOLEAN rsa2048_sha256_verify(const UINT8 digest[32],
                              const UINT8 signature[LITEOS_RSA2048_BYTES]);

#endif
