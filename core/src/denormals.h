#pragma once

#include <cstdint>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <xmmintrin.h>
#define SORUS_FTZ_X86 1
#elif defined(_M_ARM64) || defined(__aarch64__)
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define SORUS_FTZ_ARM64_MSVC 1
#else
#define SORUS_FTZ_ARM64 1
#endif
#endif

namespace sorus {

// RAII: denormals flush to zero (and read as zero) while alive; the previous mode is restored on exit.
// x86/x64: MXCSR FTZ (bit 15) + DAZ (bit 6). arm64: FPCR FZ (bit 24). Other targets: no-op.
class ScopedFlushDenormals {
 public:
  ScopedFlushDenormals() {
#if defined(SORUS_FTZ_X86)
    saved_ = _mm_getcsr();
    _mm_setcsr(static_cast<unsigned>(saved_) | 0x8040u);
#elif defined(SORUS_FTZ_ARM64_MSVC)
    saved_ = static_cast<uint64_t>(_ReadStatusReg(ARM64_FPCR));
    _WriteStatusReg(ARM64_FPCR, static_cast<__int64>(saved_ | (uint64_t{1} << 24)));
#elif defined(SORUS_FTZ_ARM64)
    asm volatile("mrs %0, fpcr" : "=r"(saved_));
    const uint64_t v = saved_ | (uint64_t{1} << 24);
    asm volatile("msr fpcr, %0" : : "r"(v));
#endif
  }
  ~ScopedFlushDenormals() {
#if defined(SORUS_FTZ_X86)
    _mm_setcsr(static_cast<unsigned>(saved_));
#elif defined(SORUS_FTZ_ARM64_MSVC)
    _WriteStatusReg(ARM64_FPCR, static_cast<__int64>(saved_));
#elif defined(SORUS_FTZ_ARM64)
    asm volatile("msr fpcr, %0" : : "r"(saved_));
#endif
  }
  ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
  ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;

 private:
  [[maybe_unused]] uint64_t saved_ = 0;
};

}  // namespace sorus
