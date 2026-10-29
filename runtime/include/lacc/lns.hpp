// LNS8 helpers for user space: quantisation, dequantisation and the reference GEMMs.
// The arithmetic itself lives in include/lacc_hw.h, shared with the device model, so the
// reference and the card are the same code path and results compare bit for bit.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

extern "C" {
#include "lacc_hw.h"
}

namespace lacc {

inline std::array<uint32_t, 8> default_lut() {
  std::array<uint32_t, 8> lut{};
  for (int i = 0; i < 8; ++i) lut[i] = static_cast<uint32_t>(std::lround(std::exp2(i / 8.0) * 65536.0));
  return lut;
}

// Nearest LNS8 code for x (in eighths of an octave), saturating at the format's range.
inline uint8_t to_lns8(float x) {
  if (x == 0.0f || !std::isfinite(x)) return 0;
  const double l = std::log2(std::fabs(static_cast<double>(x))) * LACC_LNS_FRAC + LACC_LNS_BIAS;
  // below half a step under the smallest code, the value rounds to zero
  if (l < 0.5) return 0;
  long e = std::lround(l);
  if (e < 1) e = 1;
  if (e > 127) e = 127;
  return static_cast<uint8_t>((x < 0 ? 0x80 : 0) | e);
}

inline float from_lns8(uint8_t v) {
  const int e = v & 0x7f;
  if (!e) return 0.0f;
  const double mag = std::exp2((e - LACC_LNS_BIAS) / static_cast<double>(LACC_LNS_FRAC));
  return static_cast<float>((v & 0x80) ? -mag : mag);
}

inline std::vector<uint8_t> quantize_lns8(std::span<const float> x, float scale) {
  std::vector<uint8_t> out(x.size());
  for (size_t i = 0; i < x.size(); ++i) out[i] = to_lns8(x[i] / scale);
  return out;
}

inline std::vector<int8_t> quantize_i8(std::span<const float> x, float scale) {
  std::vector<int8_t> out(x.size());
  for (size_t i = 0; i < x.size(); ++i) {
    long q = std::lround(x[i] / scale);
    out[i] = static_cast<int8_t>(q > 127 ? 127 : q < -127 ? -127 : q);
  }
  return out;
}

inline std::vector<int32_t> gemm_lns8_ref(std::span<const uint8_t> a, std::span<const uint8_t> b, uint32_t m,
                                          uint32_t n, uint32_t k,
                                          const std::array<uint32_t, 8>& lut = default_lut()) {
  std::vector<int32_t> c(static_cast<size_t>(m) * n);
  lacc_gemm_lns8(a.data(), b.data(), c.data(), m, n, k, lut.data());
  return c;
}

inline std::vector<int32_t> gemm_i8_ref(std::span<const int8_t> a, std::span<const int8_t> b, uint32_t m,
                                        uint32_t n, uint32_t k) {
  std::vector<int32_t> c(static_cast<size_t>(m) * n);
  lacc_gemm_i8(reinterpret_cast<const signed char*>(a.data()), reinterpret_cast<const signed char*>(b.data()),
               c.data(), m, n, k);
  return c;
}

}  // namespace lacc
