// LNS8 format and the reference datapath: the definitions the card model and the runtime share.
#include <gtest/gtest.h>

#include <cmath>
#include <random>

#include "lacc/lns.hpp"

using namespace lacc;

TEST(Lns8, EveryCodeRoundTrips) {
  for (int v = 0; v < 256; ++v) {
    const uint8_t code = static_cast<uint8_t>(v);
    if ((code & 0x7f) == 0) {
      EXPECT_EQ(from_lns8(code), 0.0f);
      continue;
    }
    EXPECT_EQ(to_lns8(from_lns8(code)), code) << "code " << v;
  }
}

TEST(Lns8, NearestCodeWithinHalfAStep) {
  std::mt19937 rng(1);
  std::uniform_real_distribution<double> u(-7.8, 7.8);
  const double half_step = std::exp2(1.0 / 16) - 1;  // a sixteenth of an octave
  for (int i = 0; i < 20000; ++i) {
    const float x = static_cast<float>(std::exp2(u(rng))) * (i % 2 ? -1.f : 1.f);
    const float q = from_lns8(to_lns8(x));
    EXPECT_LE(std::fabs(q / x - 1), half_step + 1e-6) << x;
    EXPECT_EQ(std::signbit(q), std::signbit(x));
  }
}

TEST(Lns8, SaturatesAndFlushes) {
  EXPECT_EQ(to_lns8(0.f), 0);
  EXPECT_EQ(to_lns8(NAN), 0);
  EXPECT_EQ(to_lns8(1e9f) & 0x7f, 127);
  EXPECT_EQ(to_lns8(-1e9f), 0x80 | 127);
  EXPECT_EQ(to_lns8(1e-9f), 0);
  EXPECT_EQ(to_lns8(1.0f), LACC_LNS_BIAS);
}

TEST(Lns8, DefaultLutMatchesFirmwareTable) {
  // fw/mkfw.py writes round(2^(i/8) * 65536); the card loads it from the image
  const uint32_t expect[8] = {65536, 71468, 77936, 84990, 92682, 101070, 110218, 120194};
  auto lut = default_lut();
  for (int i = 0; i < 8; ++i) EXPECT_EQ(lut[i], expect[i]) << i;
}

TEST(Lns8, ProductIsTheSumOfLogs) {
  auto lut = default_lut();
  for (int a = 1; a < 128; a += 3)
    for (int b = 1; b < 128; b += 5) {
      const double exact = double(from_lns8(uint8_t(a))) * from_lns8(uint8_t(b));
      const double got = lacc_lns_product_q16(uint8_t(a), uint8_t(b), lut.data()) / 65536.0;
      // the product of two codes is itself on the LNS grid: exact up to Q16 rounding
      EXPECT_NEAR(got, exact, std::max(exact * 2e-5, 1.0 / 65536)) << a << "," << b;
    }
}

TEST(Lns8, ProductSignAndZero) {
  auto lut = default_lut();
  const uint8_t one = LACC_LNS_BIAS, minus_one = 0x80 | LACC_LNS_BIAS;
  EXPECT_EQ(lacc_lns_product_q16(one, one, lut.data()), 65536);
  EXPECT_EQ(lacc_lns_product_q16(one, minus_one, lut.data()), -65536);
  EXPECT_EQ(lacc_lns_product_q16(minus_one, minus_one, lut.data()), 65536);
  EXPECT_EQ(lacc_lns_product_q16(0, minus_one, lut.data()), 0);
  EXPECT_EQ(lacc_lns_product_q16(0x80, one, lut.data()), 0);  // negative zero is zero
}

TEST(Lns8, FinishRoundsHalfAwayAndSaturates) {
  EXPECT_EQ(lacc_lns_finish_q8(128), 1);
  EXPECT_EQ(lacc_lns_finish_q8(127), 0);
  EXPECT_EQ(lacc_lns_finish_q8(-128), -1);
  EXPECT_EQ(lacc_lns_finish_q8(-127), 0);
  EXPECT_EQ(lacc_lns_finish_q8(int64_t(1) << 50), 0x7fffffff);
  EXPECT_EQ(lacc_lns_finish_q8(-(int64_t(1) << 50)), INT32_MIN);
}

TEST(Gemm, Lns8MatchesFloatOnTheSameInputs) {
  std::mt19937 rng(9);
  std::normal_distribution<float> nd(0.f, 1.f);
  const uint32_t m = 33, n = 17, k = 129;
  std::vector<float> af(m * k), bf(k * n);
  for (auto& x : af) x = nd(rng);
  for (auto& x : bf) x = nd(rng);
  auto a = quantize_lns8(af, 1.f), b = quantize_lns8(bf, 1.f);
  auto c = gemm_lns8_ref(a, b, m, n, k);
  for (uint32_t i = 0; i < m; ++i)
    for (uint32_t j = 0; j < n; ++j) {
      double fp = 0;
      for (uint32_t p = 0; p < k; ++p) fp += double(from_lns8(a[i * k + p])) * from_lns8(b[p * n + j]);
      EXPECT_NEAR(c[i * n + j] / 256.0, fp, 0.5 / 256 + std::fabs(fp) * 1e-4);
    }
}

TEST(Gemm, Int8MatchesNaive) {
  std::mt19937 rng(4);
  const uint32_t m = 5, n = 7, k = 300;
  std::vector<int8_t> a(m * k), b(k * n);
  for (auto& x : a) x = static_cast<int8_t>(int(rng() % 255) - 127);
  for (auto& x : b) x = static_cast<int8_t>(int(rng() % 255) - 127);
  auto c = gemm_i8_ref(a, b, m, n, k);
  for (uint32_t i = 0; i < m; ++i)
    for (uint32_t j = 0; j < n; ++j) {
      int32_t s = 0;
      for (uint32_t p = 0; p < k; ++p) s += a[i * k + p] * b[p * n + j];
      EXPECT_EQ(c[i * n + j], s);
    }
}

TEST(Quantize, Int8Symmetric) {
  std::vector<float> x = {0.f, 1.f, -1.f, 0.49f, 0.51f, 1000.f, -1000.f};
  auto q = quantize_i8(x, 1.f);
  EXPECT_EQ(std::vector<int8_t>(q.begin(), q.end()), (std::vector<int8_t>{0, 1, -1, 0, 1, 127, -127}));
}
