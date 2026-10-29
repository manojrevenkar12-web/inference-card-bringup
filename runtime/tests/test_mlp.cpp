// End-to-end accuracy of the quantised datapaths on a small classifier (reference GEMMs).
#include <gtest/gtest.h>

#include "mlp.hpp"

using namespace lacc::mlp;

TEST(Mlp, QuantisedPathsKeepAccuracy) {
  auto train_set = make_dataset(4096, 1), test_set = make_dataset(2048, 2);
  auto model = train(train_set);
  auto fp = predict(model, test_set, Precision::Fp32);
  auto lns = predict(model, test_set, Precision::Lns8);
  auto i8 = predict(model, test_set, Precision::Int8);
  const double a_fp = accuracy(fp, test_set), a_lns = accuracy(lns, test_set), a_i8 = accuracy(i8, test_set);
  RecordProperty("fp32", std::to_string(a_fp));
  RecordProperty("lns8", std::to_string(a_lns));
  RecordProperty("int8", std::to_string(a_i8));
  std::printf("accuracy fp32 %.4f lns8 %.4f int8 %.4f; agreement with fp32: lns8 %.4f int8 %.4f\n", a_fp, a_lns,
              a_i8, agreement(fp, lns), agreement(fp, i8));
  EXPECT_GT(a_fp, 0.80);
  EXPECT_GT(agreement(fp, lns), 0.97);
  EXPECT_GT(agreement(fp, i8), 0.95);
  EXPECT_GT(a_lns, a_fp - 0.02);
  EXPECT_GT(a_i8, a_fp - 0.05);
}

TEST(Mlp, TrainingIsDeterministic) {
  auto ds = make_dataset(512, 3);
  auto a = train(ds, 16, 2), b = train(ds, 16, 2);
  EXPECT_EQ(a.w1, b.w1);
  EXPECT_EQ(a.w2, b.w2);
}
