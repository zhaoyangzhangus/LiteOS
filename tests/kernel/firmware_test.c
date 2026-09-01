#include <kernel/firmware.h>

#include <stdio.h>

int main(void) {
    if (!firmware_core_self_test()) {
        puts("firmware: fail");
        return 1;
    }
    puts("firmware: ok");
    return 0;
}
