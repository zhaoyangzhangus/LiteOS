#include "rsa.h"

/*
 * RSA-2048 验证器只使用固定大小的字节数组，避免依赖 UEFI Shell、CRT
 * 或大整数库。运算速度不是 Loader 的关键路径；模乘采用二进制 double-and-add，
 * 每次都在加法后立即归约，验证一次签名的中间值始终小于模数。
 */

#define RSA_BYTES LITEOS_RSA2048_BYTES

/*
 * LiteOS 开发发行根公钥（RSA-2048，e = 65537）。
 * 生产发布时应替换为离线生成并审计过的发行公钥，同时保留旧公钥轮换策略。
 * 配置文件没有权限修改该数组，因此不能通过替换 loader.conf 绕过信任根。
 */
static const UINT8 g_liteos_update_modulus[RSA_BYTES] = {
    0xF1,0x7F,0xAC,0x54,0x98,0xA6,0x6F,0xDC,0x0B,0xCE,0x09,0xE4,0x4C,0xC2,0x62,0x9C,
    0xD0,0x25,0x90,0xB0,0x35,0xAA,0x8F,0x93,0x2B,0xD7,0xFC,0x91,0xE9,0x48,0xB1,0xF6,
    0x4D,0x40,0x4F,0xE0,0xC8,0x46,0x03,0x37,0x64,0xF7,0xD8,0x52,0x9A,0x08,0x23,0xCA,
    0x33,0xAC,0x6F,0x2C,0x84,0x10,0x01,0xDD,0x33,0xE9,0x80,0xB3,0x58,0xFE,0x74,0x23,
    0xBF,0xF1,0x2C,0xF9,0x6A,0xDB,0x52,0x33,0xF1,0xE9,0x6B,0x8F,0xFF,0x3E,0x63,0x64,
    0xC5,0x2B,0x28,0xDB,0x06,0x9B,0x57,0x8C,0x3A,0xDF,0x0B,0x2A,0x79,0xC8,0x76,0x31,
    0x88,0xEF,0x4F,0xAE,0xFE,0x85,0x43,0xAE,0x66,0xC5,0x83,0x0E,0x83,0xE6,0x8F,0xCB,
    0x3D,0xBD,0x18,0xF8,0x76,0x51,0xC1,0x82,0xEC,0xE7,0x7D,0xDE,0x8F,0x04,0xE2,0x8E,
    0x7E,0xE8,0x5D,0xF0,0xE1,0xB5,0x61,0x90,0x4C,0xCB,0xF9,0x76,0x74,0x5B,0x5F,0xB6,
    0xF2,0x6D,0xD1,0x4D,0x83,0x84,0x61,0x02,0xB7,0xAE,0x6A,0x00,0xFA,0x72,0x45,0x76,
    0xC3,0x9E,0x41,0x8F,0xB8,0xDF,0x1F,0x31,0x5E,0xC4,0x93,0xF0,0x86,0x4D,0x09,0x5F,
    0xD1,0x5E,0x63,0x11,0xF2,0xCE,0x31,0x23,0x78,0xE8,0xFE,0xA4,0x44,0xF4,0x79,0x95,
    0x07,0xF2,0xA3,0xAE,0xFA,0x21,0x26,0x82,0x99,0xB1,0x6E,0xC8,0xF1,0x2F,0x08,0x59,
    0xEB,0x38,0xB5,0x70,0x52,0xAA,0x77,0x4E,0xE1,0x55,0xEA,0xC5,0xB8,0xE3,0x6B,0xA0,
    0xCD,0x6B,0xDF,0xFE,0xA5,0xDA,0x39,0x2E,0x25,0x23,0x70,0x8C,0x56,0x51,0x4D,0xC0,
    0x6A,0x62,0x61,0x58,0x17,0x90,0xDD,0xAF,0xAE,0xDD,0x3D,0xAC,0xED,0x98,0x6F,0x6D
};

static UINT8 rsa_hex_digit(CHAR8 value, BOOLEAN *valid) {
    if (value >= '0' && value <= '9') return (UINT8)(value - '0');
    if (value >= 'a' && value <= 'f') return (UINT8)(value - 'a' + 10);
    if (value >= 'A' && value <= 'F') return (UINT8)(value - 'A' + 10);
    *valid = 0;
    return 0;
}

BOOLEAN rsa2048_parse_hex(const CHAR8 *text, UINTN length,
                          UINT8 output[RSA_BYTES]) {
    if (text == 0 || output == 0 || length != RSA_BYTES * 2U) return 0;
    for (UINTN index = 0; index < RSA_BYTES; ++index) {
        BOOLEAN valid = 1;
        UINT8 high = rsa_hex_digit(text[index * 2U], &valid);
        UINT8 low = rsa_hex_digit(text[index * 2U + 1U], &valid);
        if (!valid) return 0;
        output[index] = (UINT8)((high << 4) | low);
    }
    return 1;
}

static INT32 rsa_compare(const UINT8 left[RSA_BYTES],
                         const UINT8 right[RSA_BYTES]) {
    for (UINTN index = 0; index < RSA_BYTES; ++index) {
        if (left[index] < right[index]) return -1;
        if (left[index] > right[index]) return 1;
    }
    return 0;
}

static VOID rsa_copy(UINT8 destination[RSA_BYTES],
                     const UINT8 source[RSA_BYTES]) {
    for (UINTN index = 0; index < RSA_BYTES; ++index) destination[index] = source[index];
}

static VOID rsa_subtract(UINT8 value[RSA_BYTES],
                         const UINT8 modulus[RSA_BYTES]) {
    UINT16 borrow = 0;
    for (UINTN index = RSA_BYTES; index-- != 0;) {
        INT32 difference = (INT32)value[index] - (INT32)modulus[index] - (INT32)borrow;
        value[index] = (UINT8)difference;
        borrow = difference < 0 ? 1U : 0U;
    }
}

static VOID rsa_add_mod(UINT8 output[RSA_BYTES],
                        const UINT8 left[RSA_BYTES],
                        const UINT8 right[RSA_BYTES],
                        const UINT8 modulus[RSA_BYTES]) {
    UINT16 carry = 0;
    for (UINTN index = RSA_BYTES; index-- != 0;) {
        UINT16 sum = (UINT16)left[index] + (UINT16)right[index] + carry;
        output[index] = (UINT8)sum;
        carry = (UINT16)(sum >> 8);
    }
    if (carry != 0U || rsa_compare(output, modulus) >= 0) {
        /* carry 对应的 2^2048 在减法中自然被丢弃，得到同余结果。 */
        rsa_subtract(output, modulus);
    }
}

static VOID rsa_multiply_mod(UINT8 output[RSA_BYTES],
                             const UINT8 left[RSA_BYTES],
                             const UINT8 right[RSA_BYTES],
                             const UINT8 modulus[RSA_BYTES]) {
    UINT8 result[RSA_BYTES] = {0};
    UINT8 doubled[RSA_BYTES];
    rsa_copy(doubled, left);
    for (UINTN byte = RSA_BYTES; byte-- != 0;) {
        for (UINTN bit = 0; bit < 8U; ++bit) {
            if ((right[byte] & (UINT8)(1U << bit)) != 0U) {
                UINT8 sum[RSA_BYTES];
                rsa_add_mod(sum, result, doubled, modulus);
                rsa_copy(result, sum);
            }
            {
                UINT8 sum[RSA_BYTES];
                rsa_add_mod(sum, doubled, doubled, modulus);
                rsa_copy(doubled, sum);
            }
        }
    }
    rsa_copy(output, result);
}

static BOOLEAN rsa_modulus_valid(const UINT8 modulus[RSA_BYTES]) {
    UINT8 zero[RSA_BYTES] = {0};
    return rsa_compare(modulus, zero) > 0 && (modulus[RSA_BYTES - 1U] & 1U) != 0U;
}

BOOLEAN rsa2048_sha256_verify(const UINT8 digest[32],
                              const UINT8 signature[RSA_BYTES]) {
    UINT8 value[RSA_BYTES];
    UINT8 result[RSA_BYTES] = {0};
    UINT8 expected[RSA_BYTES] = {0};
    static const UINT8 digest_info_prefix[] = {
        0x30,0x31,0x30,0x0D,0x06,0x09,0x60,0x86,0x48,0x01,
        0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20
    };

    if (digest == 0 || signature == 0 || !rsa_modulus_valid(g_liteos_update_modulus) ||
        rsa_compare(signature, g_liteos_update_modulus) >= 0) return 0;
    rsa_copy(value, signature);
    rsa_copy(result, value);
    /* e = 65537 = 2^16 + 1。 */
    for (UINTN power = 0; power < 16U; ++power) {
        UINT8 square[RSA_BYTES];
        rsa_multiply_mod(square, result, result, g_liteos_update_modulus);
        rsa_copy(result, square);
    }
    {
        UINT8 product[RSA_BYTES];
        rsa_multiply_mod(product, result, value, g_liteos_update_modulus);
        rsa_copy(result, product);
    }

    expected[0] = 0x00U;
    expected[1] = 0x01U;
    for (UINTN index = 2U; index < 204U; ++index) expected[index] = 0xFFU;
    expected[204U] = 0x00U;
    for (UINTN index = 0; index < sizeof(digest_info_prefix); ++index) {
        expected[205U + index] = digest_info_prefix[index];
    }
    for (UINTN index = 0; index < 32U; ++index) {
        expected[205U + sizeof(digest_info_prefix) + index] = digest[index];
    }
    for (UINTN index = 0; index < RSA_BYTES; ++index) {
        if (result[index] != expected[index]) return 0;
    }
    return 1;
}
