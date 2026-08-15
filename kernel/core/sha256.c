#include <kernel/sha256.h>

typedef struct kernel_sha256_context {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t block[64];
    size_t used;
} kernel_sha256_context_t;

static const uint32_t g_sha256_round_constants[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

static uint32_t kernel_rotr(uint32_t value, uint32_t count) {
    return (value >> count) | (value << (32U - count));
}

static uint32_t kernel_ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

static uint32_t kernel_maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

static uint32_t kernel_big_sigma0(uint32_t x) {
    return kernel_rotr(x, 2U) ^ kernel_rotr(x, 13U) ^ kernel_rotr(x, 22U);
}

static uint32_t kernel_big_sigma1(uint32_t x) {
    return kernel_rotr(x, 6U) ^ kernel_rotr(x, 11U) ^ kernel_rotr(x, 25U);
}

static uint32_t kernel_small_sigma0(uint32_t x) {
    return kernel_rotr(x, 7U) ^ kernel_rotr(x, 18U) ^ (x >> 3U);
}

static uint32_t kernel_small_sigma1(uint32_t x) {
    return kernel_rotr(x, 17U) ^ kernel_rotr(x, 19U) ^ (x >> 10U);
}

static uint32_t kernel_read_be32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) | bytes[3];
}

static void kernel_write_be32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

static void kernel_sha256_block(kernel_sha256_context_t *context,
                                const uint8_t block[64]) {
    uint32_t schedule[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    for (uint32_t i = 0; i < 16U; ++i) schedule[i] = kernel_read_be32(block + i * 4U);
    for (uint32_t i = 16U; i < 64U; ++i) {
        schedule[i] = kernel_small_sigma1(schedule[i - 2U]) + schedule[i - 7U] +
                       kernel_small_sigma0(schedule[i - 15U]) + schedule[i - 16U];
    }
    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    f = context->state[5];
    g = context->state[6];
    h = context->state[7];
    for (uint32_t i = 0; i < 64U; ++i) {
        uint32_t first = h + kernel_big_sigma1(e) + kernel_ch(e, f, g) +
                         g_sha256_round_constants[i] + schedule[i];
        uint32_t second = kernel_big_sigma0(a) + kernel_maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

static void kernel_sha256_init(kernel_sha256_context_t *context) {
    context->state[0] = 0x6a09e667U;
    context->state[1] = 0xbb67ae85U;
    context->state[2] = 0x3c6ef372U;
    context->state[3] = 0xa54ff53aU;
    context->state[4] = 0x510e527fU;
    context->state[5] = 0x9b05688cU;
    context->state[6] = 0x1f83d9abU;
    context->state[7] = 0x5be0cd19U;
    context->bit_count = 0U;
    context->used = 0U;
}

static void kernel_sha256_update(kernel_sha256_context_t *context,
                                 const uint8_t *data, size_t length) {
    context->bit_count += (uint64_t)length * 8ULL;
    while (length != 0U) {
        size_t take = sizeof(context->block) - context->used;
        if (take > length) take = length;
        for (size_t i = 0; i < take; ++i) context->block[context->used + i] = data[i];
        context->used += take;
        data += take;
        length -= take;
        if (context->used == sizeof(context->block)) {
            kernel_sha256_block(context, context->block);
            context->used = 0U;
        }
    }
}

static void kernel_sha256_final(kernel_sha256_context_t *context,
                                uint8_t digest[KERNEL_SHA256_DIGEST_SIZE]) {
    uint64_t bits = context->bit_count;
    context->block[context->used++] = 0x80U;
    while (context->used != 56U) {
        if (context->used == sizeof(context->block)) {
            kernel_sha256_block(context, context->block);
            context->used = 0U;
        }
        context->block[context->used++] = 0U;
    }
    for (uint32_t i = 0; i < 8U; ++i) {
        context->block[56U + i] = (uint8_t)(bits >> (56U - i * 8U));
    }
    kernel_sha256_block(context, context->block);
    for (uint32_t i = 0; i < 8U; ++i) {
        kernel_write_be32(digest + i * 4U, context->state[i]);
    }
}

void kernel_sha256_compute(const void *data, size_t length,
                           uint8_t digest[KERNEL_SHA256_DIGEST_SIZE]) {
    kernel_sha256_context_t context;
    if (data == 0 || digest == 0) return;
    kernel_sha256_init(&context);
    kernel_sha256_update(&context, (const uint8_t *)data, length);
    kernel_sha256_final(&context, digest);
}

bool kernel_sha256_equal(const uint8_t left[KERNEL_SHA256_DIGEST_SIZE],
                         const uint8_t right[KERNEL_SHA256_DIGEST_SIZE]) {
    uint8_t difference = 0U;
    if (left == 0 || right == 0) return false;
    for (uint32_t i = 0; i < KERNEL_SHA256_DIGEST_SIZE; ++i) {
        difference |= (uint8_t)(left[i] ^ right[i]);
    }
    return difference == 0U;
}

bool kernel_sha256_self_test(void) {
    static const uint8_t expected[KERNEL_SHA256_DIGEST_SIZE] = {
        0xbaU, 0x78U, 0x16U, 0xbfU, 0x8fU, 0x01U, 0xcfU, 0xeaU,
        0x41U, 0x41U, 0x40U, 0xdeU, 0x5dU, 0xaeU, 0x22U, 0x23U,
        0xb0U, 0x03U, 0x61U, 0xa3U, 0x96U, 0x17U, 0x7aU, 0x9cU,
        0xb4U, 0x10U, 0xffU, 0x61U, 0xf2U, 0x00U, 0x15U, 0xadU,
    };
    uint8_t digest[KERNEL_SHA256_DIGEST_SIZE];
    static const char input[] = "abc";

    kernel_sha256_compute(input, sizeof(input) - 1U, digest);
    return kernel_sha256_equal(digest, expected);
}
