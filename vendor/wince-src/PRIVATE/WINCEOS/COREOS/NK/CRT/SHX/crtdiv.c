/*
 * crtdiv.c - minimal SH-4 integer-divide runtime helpers.
 *
 * OURS. SH-4 has no integer divide instruction, so the MS SH compiler emits
 * calls to __divlu/__modlu/__modls/__divi64/__modi64. The compiler-helper ABI is
 * the standard SH convention (args r4,r5 -> result r0; cross-checked against the
 * SDK kernel's fulllibc __divlu @ 0x8C03F2BC). C function name `_xxx` decorates
 * to the `__xxx` symbol the call sites need; shift-subtract emits no recursive
 * divide call (verified). Cold paths (printf number formatting, page-out math),
 * so correctness > speed.
 */
typedef unsigned long      u32;
typedef long               s32;
typedef unsigned __int64   u64;
typedef __int64            s64;

static u32 udiv32(u32 n, u32 d, u32 *rem)
{
    u32 q = 0, r = 0;
    int i;
    for (i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) { r -= d; q |= (1ul << i); }
    }
    if (rem) *rem = r;
    return q;
}

static u64 udiv64(u64 n, u64 d, u64 *rem)
{
    u64 q = 0, r = 0;
    int i;
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= ((u64)1 << i); }
    }
    if (rem) *rem = r;
    return q;
}

/* unsigned 32-bit */
u32 _divlu(u32 n, u32 d) { return udiv32(n, d, 0); }
u32 _modlu(u32 n, u32 d) { u32 r; udiv32(n, d, &r); return r; }

/* signed 32-bit remainder (sign follows dividend) */
s32 _modls(s32 n, s32 d)
{
    u32 un = (n < 0) ? (u32)(-n) : (u32)n;
    u32 ud = (d < 0) ? (u32)(-d) : (u32)d;
    u32 r;
    udiv32(un, ud, &r);
    return (n < 0) ? -(s32)r : (s32)r;
}

/* signed 64-bit divide / remainder */
s64 _divi64(s64 n, s64 d)
{
    int neg = (n < 0) ^ (d < 0);
    u64 un = (n < 0) ? (u64)(-n) : (u64)n;
    u64 ud = (d < 0) ? (u64)(-d) : (u64)d;
    u64 q = udiv64(un, ud, 0);
    return neg ? -(s64)q : (s64)q;
}

s64 _modi64(s64 n, s64 d)
{
    u64 un = (n < 0) ? (u64)(-n) : (u64)n;
    u64 ud = (d < 0) ? (u64)(-d) : (u64)d;
    u64 r;
    udiv64(un, ud, &r);
    return (n < 0) ? -(s64)r : (s64)r;
}
