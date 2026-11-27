// Deterministic math for anything that feeds the lock-step simulation (online battles compare every machine's state bit for bit).
// The C library's sinf / cosf / atan2f / hypotf / powf differ between platforms (glibc, bionic, msvcrt / ucrt, musl: tanf, logf and atan2f already differ
// between the Linux and the zig Windows build, 28 Nov), so a value one of them computed and wrote into a fighter differed per machine. These use only
// + - * / and sqrt / floor on doubles (all exactly rounded IEEE operations; the runtime builds with -ffp-contract=off), so every platform gets the same
// bits. Accuracy is a float's (error well below 1 ulp of the float result), speed is not a concern (a few calls per frame).
// PSPRECOMP_DMATH_OLD=1 uses the C library again (test aid to see the old behaviour).
#pragma once
#include <cmath>
#include <cstdlib>

namespace dm {
inline bool old_math() { static const bool o = std::getenv("PSPRECOMP_DMATH_OLD") != nullptr; return o; }

// sin / cos of x (radians): reduce by multiples of pi/2 (three-part constant), then a Taylor series of the remainder (|r| <= pi/4).
inline void sincos_d(double x, double* s, double* c) {
    const double inv = 0.63661977236758134308, p1 = 1.57079632673412561417, p2 = 6.07710050630396597660e-11, p3 = 2.02226624879595063154e-21;
    const double kf = std::floor(x * inv + 0.5);
    const double r = ((x - kf * p1) - kf * p2) - kf * p3, r2 = r * r;
    double sn = r, cs = 1.0, ts = r, tc = 1.0;
    for (int i = 1; i <= 9; i++) {
        tc = -tc * r2 / ((2.0 * i - 1.0) * (2.0 * i));
        ts = -ts * r2 / ((2.0 * i) * (2.0 * i + 1.0));
        cs += tc; sn += ts;
    }
    long long q = static_cast<long long>(kf) & 3;
    switch (q) { case 0: *s = sn; *c = cs; break; case 1: *s = cs; *c = -sn; break; case 2: *s = -sn; *c = -cs; break; default: *s = -cs; *c = sn; break; }
}
inline float sinf_(float x) { if (old_math()) return std::sin(x); double s, c; sincos_d(static_cast<double>(x), &s, &c); return static_cast<float>(s); }
inline float cosf_(float x) { if (old_math()) return std::cos(x); double s, c; sincos_d(static_cast<double>(x), &s, &c); return static_cast<float>(c); }

// atan(x) of a double: halve the argument twice (atan x = 2 atan(x / (1 + sqrt(1 + x^2)))), then a Taylor series (|x| < 0.2).
inline double atan_d(double x) {
    const bool neg = x < 0.0; double a = neg ? -x : x;
    const bool inv = a > 1.0; if (inv) a = 1.0 / a;
    a = a / (1.0 + std::sqrt(1.0 + a * a)); a = a / (1.0 + std::sqrt(1.0 + a * a));
    const double a2 = a * a; double t = a, sum = a;
    for (int i = 1; i <= 12; i++) { t = -t * a2; sum += t / (2.0 * i + 1.0); }
    sum *= 4.0;
    if (inv) sum = 1.57079632679489661923 - sum;
    return neg ? -sum : sum;
}
inline float atan2f_(float y, float x) {
    if (old_math()) return std::atan2(y, x);
    const double pi = 3.14159265358979323846;
    if (x == 0.0f && y == 0.0f) return std::signbit(x) ? (std::signbit(y) ? -static_cast<float>(pi) : static_cast<float>(pi)) : (std::signbit(y) ? -0.0f : 0.0f);
    double r;
    if (x == 0.0f) r = y > 0.0f ? 0.5 * pi : -0.5 * pi;
    else { r = atan_d(static_cast<double>(y) / static_cast<double>(x)); if (x < 0.0f) r += (y >= 0.0f ? pi : -pi); }
    return static_cast<float>(r);
}
inline float hypotf_(float x, float y) { if (old_math()) return std::hypot(x, y); return static_cast<float>(std::sqrt(static_cast<double>(x) * x + static_cast<double>(y) * y)); }
}  // namespace dm
