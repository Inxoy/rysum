// The kernels compiled for AVX2 + FMA (CMake gives this file /arch:AVX2 or -mavx2 -mfma, and no
// link-time optimisation). Only used when the CPU has them (Kernels.cpp).
#include "Kernels.h"
#define RYSUM_KERNEL_NS k_avx2
#define RYSUM_KERNEL_NAME "AVX2"
#include "KernelsImpl.inl"
