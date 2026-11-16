#include <stddef.h>
#include <stdlib.h>

extern "C" void __cxa_pure_virtual(void) {
    for (;;) {}
}

void *operator new(size_t size) {
    return malloc(size ? size : 1);
}

void *operator new[](size_t size) {
    return malloc(size ? size : 1);
}

void operator delete(void *ptr) noexcept {
    free(ptr);
}

void operator delete[](void *ptr) noexcept {
    free(ptr);
}

void operator delete(void *ptr, size_t) noexcept {
    free(ptr);
}

void operator delete[](void *ptr, size_t) noexcept {
    free(ptr);
}
