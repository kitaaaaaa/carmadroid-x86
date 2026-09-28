// libm. Softfp ABI: doubles arrive in r0:r1 / r2:r3, floats in r0 / r1.
#include "hle_common.h"
#include <cmath>

static double D(Cpu& c, int pair) { RegArgs a(c); if (pair) a.u64v(); return a.f64(); }
static float F(Cpu& c, int i) { u32 v = c.r(i); float f; memcpy(&f, &v, 4); return f; }

#define M1(n) HLE(n) { c.retd(std::n(D(c, 0))); }
#define M2(n) HLE(n) { c.retd(std::n(D(c, 0), D(c, 1))); }
#define F1(n, host) HLE(n) { c.retf(std::host(F(c, 0))); }

M1(acos) M1(asin) M1(atan) M1(cos) M1(cosh) M1(exp) M1(floor) M1(ceil) M1(log) M1(log10)
M1(sin) M1(sinh) M1(sqrt) M1(tan) M1(tanh) M1(rint)
M2(atan2) M2(fmod) M2(pow)
F1(sqrtf, sqrt) F1(floorf, floor) F1(ceilf, ceil) F1(log10f, log10)
F1(sinf, sin) F1(cosf, cos) F1(tanf, tan) F1(atanf, atan) F1(acosf, acos) F1(asinf, asin) F1(expf, exp)
F1(logf, log) F1(fabsf, fabs)
HLE(atan2f) { c.retf(std::atan2(F(c, 0), F(c, 1))); }
HLE(powf) { c.retf(std::pow(F(c, 0), F(c, 1))); }
HLE(fmodf) { c.retf(std::fmod(F(c, 0), F(c, 1))); }
HLE(fabs) { c.retd(std::fabs(D(c, 0))); }
HLE(lrintf) { c.ret((u32)(s32)std::lrint(F(c, 0))); }
HLE(lrint) { c.ret((u32)(s32)std::lrint(D(c, 0))); }
HLE(ldexp) { c.retd(std::ldexp(D(c, 0), (int)c.r(2))); }
HLE(frexp) {
    int e;
    double m = std::frexp(D(c, 0), &e);
    mem::w32(c.r(2), (u32)e);
    c.retd(m);
}
HLE(modf) {
    double ip;
    double f = std::modf(D(c, 0), &ip);
    memcpy(mem::ptr(c.r(2)), &ip, 8);
    c.retd(f);
}
