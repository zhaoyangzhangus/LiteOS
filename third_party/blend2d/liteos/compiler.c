/* Compiler helper required by the freestanding x86_64 toolchain. */

__attribute__((visibility("hidden"))) int __popcountdi2(
    unsigned long long value) {
    value -= (value >> 1U) & 0x5555555555555555ULL;
    value = (value & 0x3333333333333333ULL) +
            ((value >> 2U) & 0x3333333333333333ULL);
    value = (value + (value >> 4U)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int)((value * 0x0101010101010101ULL) >> 56U);
}
