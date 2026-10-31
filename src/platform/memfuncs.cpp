#include <psp2/kernel/clib.h>
#include <cstddef>

// The run-time helpers the compiler lowers memcpy, memmove and memset to. The toolchain's __aeabi_memmove copies one
// byte at a time and its __aeabi_memset and __aeabi_memclr are generic C loops; these forward to the system library,
// as memcpy, memmove and memset themselves do.
extern "C"
{

void __aeabi_memcpy(void* destination, const void* source, size_t count)
{
    sceClibMemcpy(destination, source, count);
}

void __aeabi_memcpy4(void* destination, const void* source, size_t count)
{
    sceClibMemcpy(destination, source, count);
}

void __aeabi_memcpy8(void* destination, const void* source, size_t count)
{
    sceClibMemcpy(destination, source, count);
}

void __aeabi_memmove(void* destination, const void* source, size_t count)
{
    sceClibMemmove(destination, source, count);
}

void __aeabi_memmove4(void* destination, const void* source, size_t count)
{
    sceClibMemmove(destination, source, count);
}

void __aeabi_memmove8(void* destination, const void* source, size_t count)
{
    sceClibMemmove(destination, source, count);
}

// The helpers take the count before the fill value.
void __aeabi_memset(void* destination, size_t count, int value)
{
    sceClibMemset(destination, value, count);
}

void __aeabi_memset4(void* destination, size_t count, int value)
{
    sceClibMemset(destination, value, count);
}

void __aeabi_memset8(void* destination, size_t count, int value)
{
    sceClibMemset(destination, value, count);
}

void __aeabi_memclr(void* destination, size_t count)
{
    sceClibMemset(destination, 0, count);
}

void __aeabi_memclr4(void* destination, size_t count)
{
    sceClibMemset(destination, 0, count);
}

void __aeabi_memclr8(void* destination, size_t count)
{
    sceClibMemset(destination, 0, count);
}

}
