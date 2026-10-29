// lacc-mlp: train a small classifier, then run its matmuls on the card in LNS8 and INT8.
// Checks that the card's results equal the reference bit for bit and reports accuracy for
// FP32 (host), LNS8 and INT8 (card). Prints one "@@R" line for the bring-up report.
#include <cstdio>
#include <string>

#include "lacc/device.hpp"
#include "mlp.hpp"

using namespace lacc;
using namespace lacc::mlp;

int main(int argc, char** argv) {
  const std::string path = argc > 1 ? argv[1] : "/dev/lacc0";
  auto train_set = make_dataset(4096, 1), test_set = make_dataset(2048, 2);
  auto model = train(train_set);
  bool exact = true;
  uint64_t card_gemms = 0;
  std::vector<uint32_t> lns, i8;
  try {
    Device dev(path);
    auto run = [&](auto a, auto b, uint32_t m, uint32_t n, uint32_t k, Kind kind, auto ref) {
      auto A = dev.alloc(a.size()), B = dev.alloc(b.size()), C = dev.alloc(size_t(m) * n * 4);
      dev.sync(dev.write(A, a.data(), a.size()));
      dev.sync(dev.write(B, b.data(), b.size()));
      dev.sync(dev.gemm(A, B, C, m, n, k, kind));
      std::vector<int32_t> c(size_t(m) * n);
      dev.sync(dev.read(C, c.data(), c.size() * 4));
      exact = exact && c == ref(a, b, m, n, k);
      ++card_gemms;
      return c;
    };
    Gemms card;
    card.lns8 = [&](std::span<const uint8_t> a, std::span<const uint8_t> b, uint32_t m, uint32_t n, uint32_t k) {
      return run(a, b, m, n, k, Kind::Lns8, [](auto x, auto y, auto mm, auto nn, auto kk) { return gemm_lns8_ref(x, y, mm, nn, kk); });
    };
    card.i8 = [&](std::span<const int8_t> a, std::span<const int8_t> b, uint32_t m, uint32_t n, uint32_t k) {
      return run(a, b, m, n, k, Kind::Int8, [](auto x, auto y, auto mm, auto nn, auto kk) { return gemm_i8_ref(x, y, mm, nn, kk); });
    };
    lns = predict(model, test_set, Precision::Lns8, card);
    i8 = predict(model, test_set, Precision::Int8, card);
  } catch (const std::exception& e) {
    std::printf("@@R {\"group\":\"inference\",\"name\":\"MLP classifier on the card\",\"pass\":false,\"detail\":\"%s\",\"metrics\":{}}\n", e.what());
    return 1;
  }
  auto fp = predict(model, test_set, Precision::Fp32);
  const double a_fp = accuracy(fp, test_set), a_lns = accuracy(lns, test_set), a_i8 = accuracy(i8, test_set);
  const bool pass = exact && a_lns > a_fp - 0.02 && a_i8 > a_fp - 0.05;
  std::printf("@@R {\"group\":\"inference\",\"name\":\"MLP classifier on the card: LNS8 and INT8 vs FP32\",\"pass\":%s,"
              "\"detail\":\"%llu card GEMMs %s the reference; accuracy FP32 %.2f%%, LNS8 %.2f%%, INT8 %.2f%% on %u held-out samples\","
              "\"metrics\":{\"fp32_accuracy\":%.4f,\"lns8_accuracy\":%.4f,\"int8_accuracy\":%.4f,\"lns8_agreement\":%.4f,\"int8_agreement\":%.4f}}\n",
              pass ? "true" : "false", static_cast<unsigned long long>(card_gemms), exact ? "equal" : "DIFFER from",
              a_fp * 100, a_lns * 100, a_i8 * 100, test_set.n, a_fp, a_lns, a_i8, agreement(fp, lns), agreement(fp, i8));
  return pass ? 0 : 1;
}
