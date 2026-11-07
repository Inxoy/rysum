// The kernels themselves (included by Kernels.cpp and KernelsAVX2.cpp with a different RYSUM_KERNEL_NS).
// Plain loops the compiler vectorises - with SSE2 or with AVX2 / FMA depending on how the file is compiled.
// No other headers on purpose: nothing inline from elsewhere may end up compiled with AVX2.

#if defined(_MSC_VER)
 #define RYSUM_KR __restrict
#else
 #define RYSUM_KR __restrict__
#endif

namespace rysum
{
namespace RYSUM_KERNEL_NS
{
    static void cmac (const float* RYSUM_KR a, const float* RYSUM_KR b, float* RYSUM_KR acc, int n, float sign)
    {
        for (int k = 0; k < n; k += 16)
        {
            const float* RYSUM_KR ar = a + k; const float* RYSUM_KR ai = a + k + 8;
            const float* RYSUM_KR br = b + k; const float* RYSUM_KR bi = b + k + 8;
            float* RYSUM_KR cr = acc + k; float* RYSUM_KR ci = acc + k + 8;
            for (int j = 0; j < 8; ++j)
            {
                const float re = ar[j] * br[j] - ai[j] * bi[j];
                const float im = ar[j] * bi[j] + ai[j] * br[j];
                cr[j] += sign * re;
                ci[j] += sign * im;
            }
        }
    }

    static void mix4 (float* RYSUM_KR o0, float* RYSUM_KR o1, float* RYSUM_KR o2, float* RYSUM_KR o3,
                      const float* RYSUM_KR x, const float* RYSUM_KR r, const float* a, const float* d, int n)
    {
        const float a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3], d0 = d[0], d1 = d[1], d2 = d[2], d3 = d[3];
        for (int i = 0; i < n; ++i)
        {
            const float xi = x[i], ri = r[i];
            o0[i] += xi * a0 + ri * d0;
            o1[i] += xi * a1 + ri * d1;
            o2[i] += xi * a2 + ri * d2;
            o3[i] += xi * a3 + ri * d3;
        }
    }

    static void mix4c (float* RYSUM_KR o0, float* RYSUM_KR o1, float* RYSUM_KR o2, float* RYSUM_KR o3,
                       const float* RYSUM_KR x, const float* a, int n)
    {
        const float a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
        for (int i = 0; i < n; ++i)
        {
            const float xi = x[i];
            o0[i] += xi * a0;
            o1[i] += xi * a1;
            o2[i] += xi * a2;
            o3[i] += xi * a3;
        }
    }

    static void mix4x4 (float* RYSUM_KR o0, float* RYSUM_KR o1, float* RYSUM_KR o2, float* RYSUM_KR o3,
                        const float* RYSUM_KR y0, const float* RYSUM_KR y1, const float* RYSUM_KR y2, const float* RYSUM_KR y3,
                        const float* e, int n)
    {
        const float a0 = e[0],  a1 = e[1],  a2 = e[2],  a3 = e[3];
        const float b0 = e[4],  b1 = e[5],  b2 = e[6],  b3 = e[7];
        const float c0 = e[8],  c1 = e[9],  c2 = e[10], c3 = e[11];
        const float d0 = e[12], d1 = e[13], d2 = e[14], d3 = e[15];
        for (int i = 0; i < n; ++i)
        {
            const float p = y0[i], q = y1[i], r = y2[i], s = y3[i];
            o0[i] += p * a0 + q * b0 + r * c0 + s * d0;
            o1[i] += p * a1 + q * b1 + r * c1 + s * d1;
            o2[i] += p * a2 + q * b2 + r * c2 + s * d2;
            o3[i] += p * a3 + q * b3 + r * c3 + s * d3;
        }
    }

    extern const kern::Table table;
    const kern::Table table { cmac, mix4, mix4c, mix4x4, RYSUM_KERNEL_NAME };
}
}
#undef RYSUM_KR
