/*
 * crtshift.c - SH-4 64-bit shift runtime helpers.
 *
 * OURS. The compiler emits __lshi64 (64-bit left shift) and __rshui64 (64-bit
 * unsigned right shift) for `<<`/`>>` on 64-bit values (e.g. inside crtdiv.c's
 * udiv64). Implemented via 32-bit halves through a union so the bodies use NO
 * 64-bit shift operator (no recursion). SH-4 is little-endian: lo word first.
 * Standard SH helper ABI (value -> r4:r5, count -> r6; result r0:r1).
 */
typedef unsigned long    u32;
typedef unsigned __int64 u64;

typedef union { u64 q; struct { u32 lo, hi; } w; } U64;

u64 _lshi64(u64 v, int n)
{
    U64 in, out;
    in.q = v;
    n &= 63;
    if (n == 0)        out = in;
    else if (n < 32) { out.w.hi = (in.w.hi << n) | (in.w.lo >> (32 - n)); out.w.lo = in.w.lo << n; }
    else             { out.w.hi = in.w.lo << (n - 32); out.w.lo = 0; }
    return out.q;
}

u64 _rshui64(u64 v, int n)
{
    U64 in, out;
    in.q = v;
    n &= 63;
    if (n == 0)        out = in;
    else if (n < 32) { out.w.lo = (in.w.lo >> n) | (in.w.hi << (32 - n)); out.w.hi = in.w.hi >> n; }
    else             { out.w.lo = in.w.hi >> (n - 32); out.w.hi = 0; }
    return out.q;
}
