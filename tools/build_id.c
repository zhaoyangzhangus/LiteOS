/* 为发布产物计算稳定的 SHA-256 构建 ID。 */
#include <stdio.h>
#include <stdlib.h>

#include "sha256.h"

static int write_hex(FILE *output, const UINT8 digest[32]) {
    static const CHAR8 digits[] = "0123456789abcdef";
    for (UINTN index = 0; index < 32U; ++index) {
        if (fputc(digits[digest[index] >> 4], output) == EOF ||
            fputc(digits[digest[index] & 0x0FU], output) == EOF) return 0;
    }
    return fputc('\n', output) != EOF;
}

int main(int argc, char **argv) {
    FILE *input;
    FILE *output;
    long file_size;
    UINT8 *contents;
    UINT8 digest[32];

    if (argc != 3) {
        fprintf(stderr, "usage: build_id <input> <output>\n");
        return 2;
    }
    input = fopen(argv[1], "rb");
    if (input == NULL || fseek(input, 0, SEEK_END) != 0) {
        fprintf(stderr, "cannot open input: %s\n", argv[1]);
        if (input != NULL) fclose(input);
        return 1;
    }
    file_size = ftell(input);
    if (file_size < 0 || fseek(input, 0, SEEK_SET) != 0) {
        fclose(input);
        return 1;
    }
    contents = file_size == 0 ? NULL : (UINT8 *)malloc((size_t)file_size);
    if (file_size != 0 && contents == NULL) {
        fclose(input);
        return 1;
    }
    if (file_size != 0 && fread(contents, 1, (size_t)file_size, input) != (size_t)file_size) {
        free(contents);
        fclose(input);
        return 1;
    }
    fclose(input);
    sha256_compute(contents, (UINTN)file_size, digest);
    free(contents);

    output = fopen(argv[2], "wb");
    if (output == NULL || !write_hex(output, digest)) {
        fprintf(stderr, "cannot write output: %s\n", argv[2]);
        if (output != NULL) fclose(output);
        return 1;
    }
    fclose(output);
    return 0;
}
