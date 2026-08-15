#include <stdio.h>
#include "sha256.h"

int main(void) {
    static const CHAR8 input[] = "abc";
    static const UINT8 expected[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    UINT8 actual[32];
    UINT8 parsed[32];
    sha256_compute((const UINT8 *)input, 3, actual);
    if (!sha256_equal(actual, expected)) return 1;
    if (!sha256_parse_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", parsed)) return 2;
    if (!sha256_equal(parsed, expected)) return 3;
    puts("sha256: ok");
    return 0;
}
