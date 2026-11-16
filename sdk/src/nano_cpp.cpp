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

/* Static-object destructors: apps never run them (the game returns to the kernel), so
 * registration is a no-op. Needed since start.S runs .init_array constructors. */
extern "C" {
void *__dso_handle = 0;
int __aeabi_atexit(void *object, void (*destructor)(void *), void *dso) { (void)object; (void)destructor; (void)dso; return 0; }
int __cxa_atexit(void (*destructor)(void *), void *object, void *dso) { (void)object; (void)destructor; (void)dso; return 0; }
}
