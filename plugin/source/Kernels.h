#pragma once

// The innermost loops of the plugin (mixing a signal into many channels, the complex products of
// the convolutions), compiled twice: for every x86-64 CPU (SSE2) and for CPUs with AVX2 + FMA
// (Intel since 2013, AMD since 2015). Which one runs is decided once at start-up from the CPU (see
// Kernels.cpp); setting the environment variable RYSUM_NO_AVX2 forces the plain version. On other
// CPUs (ARM) there is only the plain one.
//
// (This header only declares a table of function pointers: nothing in it can be compiled with AVX2
// by accident.)

namespace rysum
{
namespace kern
{
struct Table
{
    /** acc += sign * a * b, spectra in RealFFT's layout (blocks of 8 real parts, then their 8
       imaginary parts); n = number of floats (multiple of 16). DC / Nyquist are left to the caller.
     */
    void (*cmac)(const float *a, const float *b, float *acc, int n, float sign);
    /** o_k[i] += x[i] * a[k] + r[i] * d[k], k = 0..3 */
    void (*mix4)(float *o0, float *o1, float *o2, float *o3, const float *x, const float *r,
                 const float *a, const float *d, int n);
    /** o_k[i] += x[i] * a[k], k = 0..3 */
    void (*mix4c)(float *o0, float *o1, float *o2, float *o3, const float *x, const float *a,
                  int n);
    /** o_k[i] += sum_j y_j[i] * e[4 j + k], j, k = 0..3 */
    void (*mix4x4)(float *o0, float *o1, float *o2, float *o3, const float *y0, const float *y1,
                   const float *y2, const float *y3, const float *e, int n);
    const char *name;
};

/** the table for this CPU (chosen on the first call) */
const Table &get() noexcept;
} // namespace kern
} // namespace rysum
