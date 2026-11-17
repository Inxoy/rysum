// Vox humana - real FFT (PFFFT, ordered spectrum) and aligned buffers.
#pragma once

#include <cstddef>
#include <cstring>
#include <pffft.h>
#include <stdexcept>
#include <utility>

namespace vox
{
/** float array aligned for PFFFT, zeroed on allocation */
class Floats
{
public:
    Floats() = default;
    explicit Floats(size_t n) { assign(n); }
    Floats(const Floats &) = delete;
    Floats &operator=(const Floats &) = delete;
    Floats(Floats &&o) noexcept : p(std::exchange(o.p, nullptr)), n(std::exchange(o.n, 0)) {}
    Floats &operator=(Floats &&o) noexcept
    {
        if (this != &o)
        {
            release();
            p = std::exchange(o.p, nullptr);
            n = std::exchange(o.n, 0);
        }
        return *this;
    }
    ~Floats() { release(); }

    void assign(size_t count)
    {
        release();
        if (count == 0)
            return;
        p = static_cast<float *>(pffft_aligned_malloc(count * sizeof(float)));
        if (p == nullptr)
            throw std::bad_alloc();
        n = count;
        clear();
    }
    void clear() noexcept
    {
        if (p != nullptr)
            std::memset(p, 0, n * sizeof(float));
    }
    float *data() noexcept { return p; }
    const float *data() const noexcept { return p; }
    size_t size() const noexcept { return n; }
    float &operator[](size_t i) noexcept { return p[i]; }
    const float &operator[](size_t i) const noexcept { return p[i]; }

private:
    void release() noexcept
    {
        if (p != nullptr)
            pffft_aligned_free(p);
        p = nullptr;
        n = 0;
    }
    float *p = nullptr;
    size_t n = 0;
};

/** real FFT of size N, spectrum in the canonical order:
    s[0] = X(0), s[1] = X(N/2), then (Re, Im) of X(1) .. X(N/2 - 1).
    Unscaled: inverse(forward(x)) = N x. */
class FFT
{
public:
    FFT() = default;
    explicit FFT(int size) { init(size); }
    FFT(const FFT &) = delete;
    FFT &operator=(const FFT &) = delete;
    FFT(FFT &&o) noexcept : N(o.N), setup(std::exchange(o.setup, nullptr)), work(std::move(o.work)) {}
    ~FFT()
    {
        if (setup != nullptr)
            pffft_destroy_setup(setup);
    }

    void init(int size)
    {
        if (setup != nullptr)
            pffft_destroy_setup(setup);
        N = size;
        setup = pffft_new_setup(size, PFFFT_REAL);
        if (setup == nullptr)
            throw std::runtime_error("FFT size not supported");
        work.assign((size_t)size);
    }
    int size() const noexcept { return N; }
    void forward(const float *in, float *spec) noexcept
    {
        pffft_transform_ordered(setup, in, spec, work.data(), PFFFT_FORWARD);
    }
    void inverse(const float *spec, float *out) noexcept
    {
        pffft_transform_ordered(setup, spec, out, work.data(), PFFFT_BACKWARD);
    }

private:
    int N = 0;
    PFFFT_Setup *setup = nullptr;
    Floats work;
};
} // namespace vox
