// A small classifier, end to end: a synthetic dataset, an FP32 MLP trained on the host, and
// inference with the two hidden-layer matmuls in FP32, LNS8 or INT8. The quantised matmuls run
// through a GEMM callback, so the same code runs on the reference (host tests) or on the card
// (examples/mlp.cpp in the guest), and the results can be compared bit for bit.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <random>
#include <span>
#include <vector>

#include "lacc/lns.hpp"

namespace lacc::mlp {

struct Dataset {
  uint32_t n = 0, d = 0, classes = 0;
  std::vector<float> x;       // n x d
  std::vector<uint32_t> y;    // n
};

// Gaussian clusters on a random low-dimensional manifold, with heavy-tailed feature scales:
// close enough to real activations that per-tensor INT8 loses something on the outliers.
inline Dataset make_dataset(uint32_t n, uint32_t seed, uint32_t d = 32, uint32_t classes = 8) {
  std::mt19937 rng(1234);    // the class geometry is fixed; the seed only draws the samples
  std::normal_distribution<float> nd(0.f, 1.f);
  std::vector<float> centers(classes * d), feature_scale(d);
  for (auto& c : centers) c = nd(rng) * 0.55f;
  for (auto& s : feature_scale) s = std::exp(nd(rng) * 0.9f);
  Dataset ds{n, d, classes, std::vector<float>(size_t(n) * d), std::vector<uint32_t>(n)};
  std::mt19937 draw(seed);
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t c = draw() % classes;
    ds.y[i] = c;
    for (uint32_t j = 0; j < d; ++j) ds.x[size_t(i) * d + j] = (centers[c * d + j] + nd(draw)) * feature_scale[j];
  }
  return ds;
}

struct Model {
  uint32_t d = 0, h = 0, classes = 0;
  std::vector<float> w1, b1, w2, b2;   // w1: d x h, w2: h x classes (row-major, x @ w)
};

inline void matmul(std::span<const float> a, std::span<const float> b, std::span<float> c, uint32_t m, uint32_t n,
                   uint32_t k) {
  std::fill(c.begin(), c.end(), 0.f);
  for (uint32_t i = 0; i < m; ++i)
    for (uint32_t p = 0; p < k; ++p) {
      const float av = a[size_t(i) * k + p];
      for (uint32_t j = 0; j < n; ++j) c[size_t(i) * n + j] += av * b[size_t(p) * n + j];
    }
}

// Mini-batch SGD with momentum on softmax cross-entropy. Deterministic for a given seed.
inline Model train(const Dataset& ds, uint32_t hidden = 64, uint32_t epochs = 12, uint32_t seed = 5) {
  Model m{ds.d, hidden, ds.classes};
  std::mt19937 rng(seed);
  std::normal_distribution<float> nd(0.f, 1.f);
  // the model sees raw features; the first layer's init is scaled to each feature's range
  m.w1.resize(size_t(ds.d) * hidden);
  m.b1.assign(hidden, 0.f);
  m.w2.resize(size_t(hidden) * ds.classes);
  m.b2.assign(ds.classes, 0.f);
  std::vector<float> fscale(ds.d, 0.f);
  for (uint32_t i = 0; i < ds.n; ++i)
    for (uint32_t j = 0; j < ds.d; ++j) fscale[j] = std::max(fscale[j], std::fabs(ds.x[size_t(i) * ds.d + j]));
  for (uint32_t j = 0; j < ds.d; ++j)
    for (uint32_t k = 0; k < hidden; ++k) m.w1[size_t(j) * hidden + k] = nd(rng) * std::sqrt(2.f / ds.d) / fscale[j] * 3.f;
  for (auto& w : m.w2) w = nd(rng) * std::sqrt(2.f / hidden);

  const uint32_t bs = 32;
  const float lr = 0.05f, mom = 0.9f;
  std::vector<float> vw1(m.w1.size()), vb1(hidden), vw2(m.w2.size()), vb2(ds.classes);
  std::vector<uint32_t> order(ds.n);
  for (uint32_t i = 0; i < ds.n; ++i) order[i] = i;
  std::vector<float> xb(bs * ds.d), hb(bs * hidden), ob(bs * ds.classes), gh(bs * hidden), go(bs * ds.classes);
  std::vector<float> gw1(m.w1.size()), gw2(m.w2.size()), gb1(hidden);
  for (uint32_t e = 0; e < epochs; ++e) {
    std::shuffle(order.begin(), order.end(), rng);
    for (uint32_t s = 0; s + bs <= ds.n; s += bs) {
      for (uint32_t i = 0; i < bs; ++i)
        std::copy_n(&ds.x[size_t(order[s + i]) * ds.d], ds.d, &xb[size_t(i) * ds.d]);
      matmul(xb, m.w1, hb, bs, hidden, ds.d);
      for (uint32_t i = 0; i < bs; ++i)
        for (uint32_t k = 0; k < hidden; ++k) hb[i * hidden + k] = std::max(0.f, hb[i * hidden + k] + m.b1[k]);
      matmul(hb, m.w2, ob, bs, ds.classes, hidden);
      for (uint32_t i = 0; i < bs; ++i) {
        float mx = -1e30f, sum = 0;
        for (uint32_t c = 0; c < ds.classes; ++c) mx = std::max(mx, ob[i * ds.classes + c] += m.b2[c]);
        for (uint32_t c = 0; c < ds.classes; ++c) sum += go[i * ds.classes + c] = std::exp(ob[i * ds.classes + c] - mx);
        for (uint32_t c = 0; c < ds.classes; ++c)
          go[i * ds.classes + c] = (go[i * ds.classes + c] / sum - (c == ds.y[order[s + i]])) / bs;
      }
      std::fill(gw2.begin(), gw2.end(), 0.f);
      std::fill(gh.begin(), gh.end(), 0.f);
      for (uint32_t i = 0; i < bs; ++i)
        for (uint32_t k = 0; k < hidden; ++k)
          for (uint32_t c = 0; c < ds.classes; ++c) {
            gw2[k * ds.classes + c] += hb[i * hidden + k] * go[i * ds.classes + c];
            gh[i * hidden + k] += m.w2[k * ds.classes + c] * go[i * ds.classes + c];
          }
      std::fill(gw1.begin(), gw1.end(), 0.f);
      std::fill(gb1.begin(), gb1.end(), 0.f);
      for (uint32_t i = 0; i < bs; ++i)
        for (uint32_t k = 0; k < hidden; ++k) {
          if (hb[i * hidden + k] <= 0.f) continue;
          const float g = gh[i * hidden + k];
          for (uint32_t j = 0; j < ds.d; ++j) gw1[j * hidden + k] += xb[i * ds.d + j] * g;
          gb1[k] += g;
        }
      for (size_t i = 0; i < m.w1.size(); ++i) m.w1[i] -= lr * (vw1[i] = mom * vw1[i] + gw1[i]);
      for (size_t i = 0; i < m.w2.size(); ++i) m.w2[i] -= lr * (vw2[i] = mom * vw2[i] + gw2[i]);
      for (uint32_t k = 0; k < hidden; ++k) m.b1[k] -= lr * (vb1[k] = mom * vb1[k] + gb1[k]);
      for (uint32_t c = 0; c < ds.classes; ++c) {
        float g = 0;
        for (uint32_t i = 0; i < bs; ++i) g += go[i * ds.classes + c];
        m.b2[c] -= lr * (vb2[c] = mom * vb2[c] + g);
      }
    }
  }
  return m;
}

enum class Precision { Fp32, Lns8, Int8 };

// GEMM hooks for the quantised paths: (A, B, m, n, k) -> C, row-major, as the card does it.
struct Gemms {
  std::function<std::vector<int32_t>(std::span<const uint8_t>, std::span<const uint8_t>, uint32_t, uint32_t, uint32_t)> lns8 =
      [](auto a, auto b, uint32_t m, uint32_t n, uint32_t k) { return gemm_lns8_ref(a, b, m, n, k); };
  std::function<std::vector<int32_t>(std::span<const int8_t>, std::span<const int8_t>, uint32_t, uint32_t, uint32_t)> i8 =
      [](auto a, auto b, uint32_t m, uint32_t n, uint32_t k) { return gemm_i8_ref(a, b, m, n, k); };
};

inline float absmax(std::span<const float> x) {
  float m = 0;
  for (float v : x) m = std::max(m, std::fabs(v));
  return m > 0 ? m : 1.f;
}

// y = x @ w in the given precision, per-tensor scales chosen from the data.
inline std::vector<float> linear(std::span<const float> x, std::span<const float> w, uint32_t m, uint32_t n, uint32_t k,
                                 Precision p, const Gemms& g) {
  std::vector<float> y(size_t(m) * n);
  if (p == Precision::Fp32) {
    matmul(x, w, y, m, n, k);
    return y;
  }
  if (p == Precision::Lns8) {
    // LNS8 covers 2^-7.875 .. 2^7.875: put each tensor's largest magnitude at 2^7
    const float sx = absmax(x) / 128.f, sw = absmax(w) / 128.f;
    auto c = g.lns8(quantize_lns8(x, sx), quantize_lns8(w, sw), m, n, k);
    for (size_t i = 0; i < y.size(); ++i) y[i] = float(c[i] / 256.0 * sx * sw);
    return y;
  }
  const float sx = absmax(x) / 127.f, sw = absmax(w) / 127.f;
  auto c = g.i8(quantize_i8(x, sx), quantize_i8(w, sw), m, n, k);
  for (size_t i = 0; i < y.size(); ++i) y[i] = float(double(c[i]) * sx * sw);
  return y;
}

inline std::vector<uint32_t> predict(const Model& md, const Dataset& ds, Precision p, const Gemms& g = {}) {
  auto h = linear(ds.x, md.w1, ds.n, md.h, md.d, p, g);
  for (uint32_t i = 0; i < ds.n; ++i)
    for (uint32_t k = 0; k < md.h; ++k) h[size_t(i) * md.h + k] = std::max(0.f, h[size_t(i) * md.h + k] + md.b1[k]);
  auto o = linear(h, md.w2, ds.n, md.classes, md.h, p, g);
  std::vector<uint32_t> out(ds.n);
  for (uint32_t i = 0; i < ds.n; ++i) {
    uint32_t best = 0;
    for (uint32_t c = 1; c < md.classes; ++c)
      if (o[size_t(i) * md.classes + c] + md.b2[c] > o[size_t(i) * md.classes + best] + md.b2[best]) best = c;
    out[i] = best;
  }
  return out;
}

inline double accuracy(const std::vector<uint32_t>& pred, const Dataset& ds) {
  uint32_t ok = 0;
  for (uint32_t i = 0; i < ds.n; ++i) ok += pred[i] == ds.y[i];
  return double(ok) / ds.n;
}

inline double agreement(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
  uint32_t ok = 0;
  for (size_t i = 0; i < a.size(); ++i) ok += a[i] == b[i];
  return a.empty() ? 1.0 : double(ok) / a.size();
}

}  // namespace lacc::mlp
