/*
 * crtmem.c - minimal SH-4 C runtime: mem/str helpers the kernel calls.
 *
 * OURS. The DC SDK ships no static libc (only the coredll import lib); the real
 * kernel linked `fulllibc.lib` we don't have. These are the handful nkmain.lib
 * references (memcpy/memset/memcmp/strcmp/strlen + memmove). Plain C.
 *
 * #pragma function(...) forces real calls (no intrinsic substitution) so the
 * compiler can't rewrite a byte loop into a recursive memset/memcpy.
 */
#pragma function(memcpy, memset, memcmp, strcmp, strlen)

void *memcpy(void *dst, const void *src, unsigned int n)
{
    char *d = (char *)dst;
    const char *s = (const char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, unsigned int n)
{
    char *d = (char *)dst;
    const char *s = (const char *)src;
    if (d <= s) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

void *memset(void *dst, int c, unsigned int n)
{
    char *d = (char *)dst;
    while (n--) *d++ = (char)c;
    return dst;
}

int memcmp(const void *a, const void *b, unsigned int n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (n--) { if (*x != *y) return (int)*x - (int)*y; x++; y++; }
    return 0;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

unsigned int strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (unsigned int)(p - s);
}
