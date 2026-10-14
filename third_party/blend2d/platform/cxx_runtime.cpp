/* Minimal global C++ allocation operators for the freestanding Blend2D ABI. */

#include <stddef.h>

extern "C" void *malloc(size_t size);
extern "C" void free(void *pointer);

void *operator new(size_t size) noexcept {
    return malloc(size);
}

void *operator new[](size_t size) noexcept {
    return malloc(size);
}

void operator delete(void *pointer) noexcept {
    free(pointer);
}

void operator delete[](void *pointer) noexcept {
    free(pointer);
}

void operator delete(void *pointer, size_t) noexcept {
    free(pointer);
}

void operator delete[](void *pointer, size_t) noexcept {
    free(pointer);
}
