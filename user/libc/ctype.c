#include "liteos/libc.h"

static int ascii_byte(int value) {
    return value == EOF ? EOF : (unsigned char)value;
}

int isalpha(int value) {
    value = ascii_byte(value);
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

int isdigit(int value) {
    value = ascii_byte(value);
    return value >= '0' && value <= '9';
}

int isalnum(int value) {
    return isalpha(value) || isdigit(value);
}

int isblank(int value) {
    value = ascii_byte(value);
    return value == ' ' || value == '\t';
}

int iscntrl(int value) {
    value = ascii_byte(value);
    return (value >= 0 && value < 0x20) || value == 0x7f;
}

int islower(int value) {
    value = ascii_byte(value);
    return value >= 'a' && value <= 'z';
}

int isupper(int value) {
    value = ascii_byte(value);
    return value >= 'A' && value <= 'Z';
}

int isspace(int value) {
    value = ascii_byte(value);
    return value == ' ' || value == '\t' || value == '\n' ||
           value == '\r' || value == '\v' || value == '\f';
}

int isprint(int value) {
    value = ascii_byte(value);
    return value >= 0x20 && value <= 0x7e;
}

int isgraph(int value) {
    value = ascii_byte(value);
    return value >= 0x21 && value <= 0x7e;
}

int isascii(int value) {
    return value >= 0 && value <= 0x7f;
}

int ispunct(int value) {
    return isgraph(value) && !isalnum(value);
}

int isxdigit(int value) {
    value = ascii_byte(value);
    return isdigit(value) || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

int tolower(int value) {
    return isupper(value) ? value + ('a' - 'A') : value;
}

int toupper(int value) {
    return islower(value) ? value - ('a' - 'A') : value;
}
