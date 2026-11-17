// Vox humana - runtime CPU check for the AVX2 + FMA kernels (x86-64); elsewhere plain loops.
#pragma once

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#include <immintrin.h>
#define VOX_SSE 1
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define VOX_AVX2_FN
#else
#define VOX_AVX2_FN __attribute__((target("avx2,fma")))
#endif
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define VOX_NEON 1
#endif

namespace vox
{
#if VOX_SSE
/** AVX2 + FMA available (checked once) */
inline bool haveAvx2() noexcept
{
    static const bool yes = []
    {
#if defined(_MSC_VER) && !defined(__clang__)
        int r[4];
        __cpuid(r, 0);
        if (r[0] < 7)
            return false;
        __cpuid(r, 1);
        const bool fma = (r[2] & (1 << 12)) != 0, osx = (r[2] & (1 << 27)) != 0, avx = (r[2] & (1 << 28)) != 0;
        if (!(fma && osx && avx) || (_xgetbv(0) & 6) != 6)
            return false;
        __cpuidex(r, 7, 0);
        return (r[1] & (1 << 5)) != 0;
#else
        return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif
    }();
    return yes;
}

/** y[v][o] = b[o] + sum_i x[v][i] w[i][o] for nx rows; w is [in][out], out a multiple of 8 */
VOX_AVX2_FN inline void matmulAvx2(const float *x, int nx, int in, const float *w, const float *b, int out,
                                   float *y) noexcept
{
    for (int o0 = 0; o0 < out; o0 += 32)
    {
        const int nvec = (out - o0) >= 32 ? 4 : (out - o0) / 8;
        int v = 0;
        for (; v + 1 < nx; v += 2)
        {
            __m256 a[2][4];
            for (int k = 0; k < nvec; ++k)
                a[0][k] = a[1][k] = _mm256_loadu_ps(b + o0 + 8 * k);
            const float *x0 = x + (size_t)v * in, *x1 = x0 + in;
            for (int i = 0; i < in; ++i)
            {
                const float *wr = w + (size_t)i * out + o0;
                const __m256 s0 = _mm256_broadcast_ss(x0 + i), s1 = _mm256_broadcast_ss(x1 + i);
                for (int k = 0; k < nvec; ++k)
                {
                    const __m256 wk = _mm256_loadu_ps(wr + 8 * k);
                    a[0][k] = _mm256_fmadd_ps(s0, wk, a[0][k]);
                    a[1][k] = _mm256_fmadd_ps(s1, wk, a[1][k]);
                }
            }
            for (int k = 0; k < nvec; ++k)
            {
                _mm256_storeu_ps(y + (size_t)v * out + o0 + 8 * k, a[0][k]);
                _mm256_storeu_ps(y + (size_t)(v + 1) * out + o0 + 8 * k, a[1][k]);
            }
        }
        for (; v < nx; ++v)
        {
            __m256 a[4];
            for (int k = 0; k < nvec; ++k)
                a[k] = _mm256_loadu_ps(b + o0 + 8 * k);
            const float *x0 = x + (size_t)v * in;
            for (int i = 0; i < in; ++i)
            {
                const float *wr = w + (size_t)i * out + o0;
                const __m256 s0 = _mm256_broadcast_ss(x0 + i);
                for (int k = 0; k < nvec; ++k)
                    a[k] = _mm256_fmadd_ps(s0, _mm256_loadu_ps(wr + 8 * k), a[k]);
            }
            for (int k = 0; k < nvec; ++k)
                _mm256_storeu_ps(y + (size_t)v * out + o0 + 8 * k, a[k]);
        }
    }
    _mm256_zeroupper();
}
#endif
} // namespace vox
