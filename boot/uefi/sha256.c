#include <boot/sha256.h>

typedef struct {
    UINT32 state[8];
    UINT64 bit_count;
    UINT8 block[64];
    UINTN used;
} SHA256_CONTEXT;

static const UINT32 k[64] = {
    0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
    0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
    0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
    0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
    0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
    0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
    0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
    0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
};

static UINT32 rotr(UINT32 x, UINT32 n) { return (x >> n) | (x << (32U - n)); }
static UINT32 ch(UINT32 x, UINT32 y, UINT32 z) { return (x & y) ^ (~x & z); }
static UINT32 maj(UINT32 x, UINT32 y, UINT32 z) { return (x & y) ^ (x & z) ^ (y & z); }
static UINT32 bs0(UINT32 x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
static UINT32 bs1(UINT32 x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
static UINT32 ss0(UINT32 x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
static UINT32 ss1(UINT32 x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

static UINT32 be32(const UINT8 *p) {
    return ((UINT32)p[0] << 24) | ((UINT32)p[1] << 16) | ((UINT32)p[2] << 8) | p[3];
}

static VOID put_be32(UINT8 *p, UINT32 x) {
    p[0] = (UINT8)(x >> 24); p[1] = (UINT8)(x >> 16); p[2] = (UINT8)(x >> 8); p[3] = (UINT8)x;
}

static VOID sha256_block(SHA256_CONTEXT *ctx, const UINT8 *block) {
    UINT32 w[64];
    for (UINTN i = 0; i < 16; ++i) w[i] = be32(block + i * 4);
    for (UINTN i = 16; i < 64; ++i) w[i] = ss1(w[i - 2]) + w[i - 7] + ss0(w[i - 15]) + w[i - 16];
    UINT32 a=ctx->state[0], b=ctx->state[1], c=ctx->state[2], d=ctx->state[3];
    UINT32 e=ctx->state[4], f=ctx->state[5], g=ctx->state[6], h=ctx->state[7];
    for (UINTN i = 0; i < 64; ++i) {
        UINT32 t1 = h + bs1(e) + ch(e,f,g) + k[i] + w[i];
        UINT32 t2 = bs0(a) + maj(a,b,c);
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}

static VOID sha256_init(SHA256_CONTEXT *ctx) {
    ctx->state[0]=0x6a09e667U; ctx->state[1]=0xbb67ae85U; ctx->state[2]=0x3c6ef372U; ctx->state[3]=0xa54ff53aU;
    ctx->state[4]=0x510e527fU; ctx->state[5]=0x9b05688cU; ctx->state[6]=0x1f83d9abU; ctx->state[7]=0x5be0cd19U;
    ctx->bit_count=0; ctx->used=0;
}

static VOID sha256_update(SHA256_CONTEXT *ctx, const UINT8 *data, UINTN length) {
    ctx->bit_count += (UINT64)length * 8ULL;
    while (length != 0) {
        UINTN take = 64 - ctx->used;
        if (take > length) take = length;
        for (UINTN i = 0; i < take; ++i) ctx->block[ctx->used+i] = data[i];
        ctx->used += take; data += take; length -= take;
        if (ctx->used == 64) { sha256_block(ctx, ctx->block); ctx->used=0; }
    }
}

static VOID sha256_final(SHA256_CONTEXT *ctx, UINT8 digest[32]) {
    UINT64 bits = ctx->bit_count;
    ctx->block[ctx->used++] = 0x80;
    while (ctx->used != 56) {
        if (ctx->used == 64) { sha256_block(ctx, ctx->block); ctx->used=0; }
        ctx->block[ctx->used++] = 0;
    }
    for (UINTN i = 0; i < 8; ++i) ctx->block[56+i] = (UINT8)(bits >> (56 - i*8));
    sha256_block(ctx, ctx->block);
    for (UINTN i = 0; i < 8; ++i) put_be32(digest + i*4, ctx->state[i]);
}

VOID sha256_compute(const UINT8 *data, UINTN length, UINT8 digest[32]) {
    SHA256_CONTEXT ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, length);
    sha256_final(&ctx, digest);
}
