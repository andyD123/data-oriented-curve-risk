#pragma once
// The vector primitive set the grouped scan uses: eight doubles per vec8, eleven functions.
// One of four backends is selected at compile time; the kernel does not change.
//
//   LADDER_LANES_STDX     std::experimental::simd fixed_size<8>      (default) any target with the header
//   LADDER_LANES_AVX512   one __m512d                                AVX-512 intrinsics
//   LADDER_LANES_AVX2     two __m256d                                AVX2 + FMA intrinsics, for targets without AVX-512
//   LADDER_LANES_PORTABLE plain double[8] with loops the compiler    no intrinsics, no simd header: relies on
//                         auto-vectorises                            auto-vectorisation (SSE2, NEON, anything)
//
//   LADDER_LANES_AUTO     picks AVX512 / AVX2 / PORTABLE from the target's __AVX512F__ / __AVX2__ macros
//
// Streaming stores exist only on the x86 intrinsics backends; the others fall back to a normal store, which costs the
// read-for-ownership on write-once output (measured: the streaming gain is lost, nothing else changes).
#include <cstddef>

#if defined(LADDER_LANES_AUTO)
  #if defined(__AVX512F__)
    #define LADDER_LANES_AVX512
  #elif defined(__AVX2__) && defined(__FMA__)
    #define LADDER_LANES_AVX2
  #else
    #define LADDER_LANES_PORTABLE
  #endif
#endif

#if defined(LADDER_LANES_AVX512)
  #include <immintrin.h>
  namespace ladder {
  struct vec8 { __m512d v; };
  inline vec8 vzero()                          { return { _mm512_setzero_pd() }; }
  inline vec8 vbroadcast(double s)             { return { _mm512_set1_pd(s) }; }
  inline vec8 vload(const double* p)           { return { _mm512_load_pd(p) }; }
  inline vec8 vadd(vec8 a, vec8 b)             { return { _mm512_add_pd(a.v, b.v) }; }
  inline vec8 vmul(vec8 a, vec8 b)             { return { _mm512_mul_pd(a.v, b.v) }; }
  inline vec8 vfma(vec8 a, vec8 b, vec8 c)     { return { _mm512_fmadd_pd(a.v, b.v, c.v) }; }
  inline vec8 vneg(vec8 a)                     { return { _mm512_sub_pd(_mm512_setzero_pd(), a.v) }; }
  inline void vstore(double* p, vec8 a)        { _mm512_store_pd(p, a.v); }
  inline void vstore_stream(double* p, vec8 a) { _mm512_stream_pd(p, a.v); }
  inline void vfence()                         { _mm_sfence(); }
  }

#elif defined(LADDER_LANES_AVX2)
  #include <immintrin.h>
  namespace ladder {
  struct vec8 { __m256d lo, hi; };
  inline vec8 vzero()                          { return { _mm256_setzero_pd(), _mm256_setzero_pd() }; }
  inline vec8 vbroadcast(double s)             { return { _mm256_set1_pd(s), _mm256_set1_pd(s) }; }
  inline vec8 vload(const double* p)           { return { _mm256_load_pd(p), _mm256_load_pd(p + 4) }; }
  inline vec8 vadd(vec8 a, vec8 b)             { return { _mm256_add_pd(a.lo, b.lo), _mm256_add_pd(a.hi, b.hi) }; }
  inline vec8 vmul(vec8 a, vec8 b)             { return { _mm256_mul_pd(a.lo, b.lo), _mm256_mul_pd(a.hi, b.hi) }; }
  inline vec8 vfma(vec8 a, vec8 b, vec8 c)     { return { _mm256_fmadd_pd(a.lo, b.lo, c.lo), _mm256_fmadd_pd(a.hi, b.hi, c.hi) }; }
  inline vec8 vneg(vec8 a)                     { __m256d z = _mm256_setzero_pd(); return { _mm256_sub_pd(z, a.lo), _mm256_sub_pd(z, a.hi) }; }
  inline void vstore(double* p, vec8 a)        { _mm256_store_pd(p, a.lo); _mm256_store_pd(p + 4, a.hi); }
  inline void vstore_stream(double* p, vec8 a) { _mm256_stream_pd(p, a.lo); _mm256_stream_pd(p + 4, a.hi); }
  inline void vfence()                         { _mm_sfence(); }
  }

#elif defined(LADDER_LANES_PORTABLE)
  namespace ladder {
  struct alignas(64) vec8 { double v[8]; };
  inline vec8 vzero()                          { vec8 r; for (int i = 0; i < 8; ++i) r.v[i] = 0.0; return r; }
  inline vec8 vbroadcast(double s)             { vec8 r; for (int i = 0; i < 8; ++i) r.v[i] = s; return r; }
  inline vec8 vload(const double* p)           { vec8 r; for (int i = 0; i < 8; ++i) r.v[i] = p[i]; return r; }
  inline vec8 vadd(vec8 a, vec8 b)             { vec8 r; for (int i = 0; i < 8; ++i) r.v[i] = a.v[i] + b.v[i]; return r; }
  inline vec8 vmul(vec8 a, vec8 b)             { vec8 r; for (int i = 0; i < 8; ++i) r.v[i] = a.v[i] * b.v[i]; return r; }
  inline vec8 vfma(vec8 a, vec8 b, vec8 c)     { vec8 r; for (int i = 0; i < 8; ++i) r.v[i] = a.v[i] * b.v[i] + c.v[i]; return r; }   // contracted under -ffp-contract=fast
  inline vec8 vneg(vec8 a)                     { vec8 r; for (int i = 0; i < 8; ++i) r.v[i] = -a.v[i]; return r; }
  inline void vstore(double* p, vec8 a)        { for (int i = 0; i < 8; ++i) p[i] = a.v[i]; }
  inline void vstore_stream(double* p, vec8 a) { vstore(p, a); }       // no streaming store without intrinsics
  inline void vfence()                         {}
  }

#else   // LADDER_LANES_STDX (default)
  #include <experimental/simd>
  #if defined(__AVX512F__) || defined(__AVX2__)
    #include <immintrin.h>
  #endif
  namespace ladder {
  namespace stdx = std::experimental;
  using simd8 = stdx::fixed_size_simd<double, 8>;
  struct vec8 { simd8 v; };
  inline vec8 vzero()                          { return { simd8(0.0) }; }
  inline vec8 vbroadcast(double s)             { return { simd8(s) }; }
  inline vec8 vload(const double* p)           { return { simd8(p, stdx::vector_aligned) }; }
  inline vec8 vadd(vec8 a, vec8 b)             { return { a.v + b.v }; }
  inline vec8 vmul(vec8 a, vec8 b)             { return { a.v * b.v }; }
  inline vec8 vfma(vec8 a, vec8 b, vec8 c)     { return { a.v * b.v + c.v }; }                 // contracted under -ffp-contract=fast
  inline vec8 vneg(vec8 a)                     { return { -a.v }; }
  inline void vstore(double* p, vec8 a)        { a.v.copy_to(p, stdx::vector_aligned); }
  inline void vstore_stream(double* p, vec8 a) {
  #if defined(__AVX512F__)
      alignas(64) double t[8]; a.v.copy_to(t, stdx::vector_aligned); _mm512_stream_pd(p, _mm512_load_pd(t));
  #elif defined(__AVX2__)
      alignas(64) double t[8]; a.v.copy_to(t, stdx::vector_aligned); _mm256_stream_pd(p, _mm256_load_pd(t)); _mm256_stream_pd(p + 4, _mm256_load_pd(t + 4));
  #else
      a.v.copy_to(p, stdx::vector_aligned);
  #endif
  }
  inline void vfence() {
  #if defined(__AVX512F__) || defined(__AVX2__)
      _mm_sfence();
  #endif
  }
  }
#endif
