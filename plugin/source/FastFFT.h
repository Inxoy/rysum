#pragma once

#include "Kernels.h"
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <pffft.h>

namespace rysum
{
/** a float array aligned to 64 bytes (SIMD loads, no cache-line splits); zeroed on allocation */
class AlignedFloats
{
public:
    AlignedFloats() = default;
    explicit AlignedFloats(size_t n)
    {
        assign(n);
    }
    AlignedFloats(const AlignedFloats &) = delete;
    AlignedFloats &operator=(const AlignedFloats &) = delete;
    AlignedFloats(AlignedFloats &&o) noexcept : ptr(o.ptr), num(o.num)
    {
        o.ptr = nullptr;
        o.num = 0;
    }
    AlignedFloats &operator=(AlignedFloats &&o) noexcept
    {
        if (this != &o)
        {
            release();
            ptr = o.ptr;
            num = o.num;
            o.ptr = nullptr;
            o.num = 0;
        }
        return *this;
    }
    ~AlignedFloats()
    {
        release();
    }

    /** (re)allocates n floats, all zero. Not for the audio thread. */
    void assign(size_t n)
    {
        release();
        if (n == 0)
            return;
        ptr = static_cast<float *>(::operator new(n * sizeof(float), std::align_val_t(64)));
        num = n;
        clear();
    }
    void clear() noexcept
    {
        if (ptr != nullptr)
            std::memset(ptr, 0, num * sizeof(float));
    }
    float *data() noexcept
    {
        return ptr;
    }
    const float *data() const noexcept
    {
        return ptr;
    }
    size_t size() const noexcept
    {
        return num;
    }
    bool empty() const noexcept
    {
        return num == 0;
    }
    float &operator[](size_t i) noexcept
    {
        return ptr[i];
    }
    const float &operator[](size_t i) const noexcept
    {
        return ptr[i];
    }

private:
    void release() noexcept
    {
        if (ptr != nullptr)
            ::operator delete(ptr, std::align_val_t(64));
        ptr = nullptr;
        num = 0;
    }
    float *ptr = nullptr;
    size_t num = 0;
};

/** Real FFT of size N (PFFFT, SSE / NEON) for fast convolution.

    The spectrum is N floats in an order made for multiplying: blocks of 16 floats = 8 real parts,
   then their 8 imaginary parts (PFFFT's own order with its 4-wide groups paired up, so the products
   run 8 wide with AVX). It is only meant to be multiplied with other spectra of the same size
   (multiplyAdd) and transformed back. The transforms are not scaled: inverse (forward (x)) = N * x,
   so impulse-response spectra are made with forwardScaled (1 / N). N must be a power of two >= 256.

    All pointers must be aligned to 16 bytes (AlignedFloats is). Real-time safe after construction.
 */
class RealFFT
{
public:
    explicit RealFFT(int n)
        : N(n), setup(pffft_new_setup(n, PFFFT_REAL)), work((size_t)n), tmp((size_t)n),
          paired(pffft_simd_size() == 4), k(kern::get())
    {
    }
    ~RealFFT()
    {
        if (setup != nullptr)
            pffft_destroy_setup(setup);
    }
    RealFFT(const RealFFT &) = delete;
    RealFFT &operator=(const RealFFT &) = delete;

    int size() const noexcept
    {
        return N;
    }

    void forward(const float *in, float *spec) noexcept
    {
        pffft_transform(setup, in, spec, work.data(), PFFFT_FORWARD);
        if (paired)
            swapQuads(spec);
    }
    void inverse(const float *spec, float *out) noexcept
    {
        if (paired)
        {
            std::memcpy(tmp.data(), spec, (size_t)N * sizeof(float));
            swapQuads(tmp.data());
            pffft_transform(setup, tmp.data(), out, work.data(), PFFFT_BACKWARD);
        }
        else
            pffft_transform(setup, spec, out, work.data(), PFFFT_BACKWARD);
    }

    /** spectrum of an impulse-response segment, already scaled by 1 / N */
    void forwardScaled(const float *in, float *spec) noexcept
    {
        forward(in, spec);
        const float kk = 1.0f / (float)N;
        for (int i = 0; i < N; ++i)
            spec[i] *= kk;
    }

    /** acc += a * b (complex, per bin) */
    void multiplyAdd(const float *a, const float *b, float *acc) const noexcept
    {
        mac(a, b, acc, 1.0f);
    }
    /** acc -= a * b */
    void multiplySub(const float *a, const float *b, float *acc) const noexcept
    {
        mac(a, b, acc, -1.0f);
    }

private:
    void mac(const float *a, const float *b, float *acc, float sign) const noexcept
    {
        if (!paired)
        {
            pffft_zconvolve_accumulate(setup, a, b, acc, sign);
            return;
        }
        // the first "complex" value holds DC (real part) and Nyquist (imaginary part, here at index
        // 8): both real
        const float dc = acc[0] + sign * a[0] * b[0], ny = acc[8] + sign * a[8] * b[8];
        k.cmac(a, b, acc, N, sign);
        acc[0] = dc;
        acc[8] = ny;
    }

    /** PFFFT's order (4 real, 4 imaginary, 4 real, 4 imaginary) <-> 8 real, 8 imaginary (its own
     * inverse) */
    void swapQuads(float *s) const noexcept
    {
        for (int b = 0; b < N; b += 16)
            for (int j = 0; j < 4; ++j)
            {
                const float t = s[b + 4 + j];
                s[b + 4 + j] = s[b + 8 + j];
                s[b + 8 + j] = t;
            }
    }

    int N;
    PFFFT_Setup *setup;
    AlignedFloats work, tmp;
    bool paired;
    const kern::Table &k;
};
} // namespace rysum
