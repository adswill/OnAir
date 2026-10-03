#pragma once
// Runtime-dispatched vector code for x86-64 (Linux): the function is compiled twice, for AVX2 and for the baseline (SSE2),
// and the dynamic loader picks the right one for the CPU. Other targets compile the plain function. ARM uses NEON intrinsics
// where they exist (guarded by __ARM_NEON) and the compiler's own vectoriser elsewhere.
#if defined(__GNUC__) && defined(__x86_64__) && defined(__linux__) && !defined(__clang__) && !defined(DECT2_NO_SIMD)
#define DECT2_MULTIVERSION __attribute__((target_clones("avx2", "default")))
#else
#define DECT2_MULTIVERSION
#endif
