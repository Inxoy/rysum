#include "Kernels.h"
#include <cstdlib>

#define RYSUM_KERNEL_NS k_base
#define RYSUM_KERNEL_NAME "SSE2"
#include "KernelsImpl.inl"

#if (defined(_M_X64) || defined(__x86_64__)) && defined(RYSUM_HAVE_AVX2_KERNELS)
 #define RYSUM_X86_DISPATCH 1
 #if defined(_MSC_VER)
  #include <intrin.h>
  #include <immintrin.h>
 #endif
namespace rysum { namespace k_avx2 { extern const kern::Table table; } }
#endif

namespace rysum
{
namespace kern
{
#if defined(RYSUM_X86_DISPATCH)
    static bool cpuHasAvx2Fma() noexcept
    {
       #if defined(_MSC_VER)
        int r[4] {};
        __cpuid (r, 0);
        if (r[0] < 7) return false;
        __cpuid (r, 1);
        const bool fma = (r[2] & (1 << 12)) != 0, osxsave = (r[2] & (1 << 27)) != 0, avx = (r[2] & (1 << 28)) != 0;
        if (! (fma && osxsave && avx)) return false;
        if ((_xgetbv (0) & 6) != 6) return false;          // the OS saves the AVX registers
        __cpuidex (r, 7, 0);
        return (r[1] & (1 << 5)) != 0;                       // AVX2
       #else
        __builtin_cpu_init();
        return __builtin_cpu_supports ("avx2") && __builtin_cpu_supports ("fma");
       #endif
    }
#endif

    static const Table& choose() noexcept
    {
       #if defined(RYSUM_X86_DISPATCH)
        if (std::getenv ("RYSUM_NO_AVX2") == nullptr && cpuHasAvx2Fma()) return k_avx2::table;
       #endif
        return k_base::table;
    }

    const Table& get() noexcept
    {
        static const Table& t = choose();
        return t;
    }
}
}
