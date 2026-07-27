#include "lsm/bloom_filter.h"

#include <gtest/gtest.h>

#include <random>
#include <unordered_set>

namespace dbengine {
namespace {

TEST(BloomFilterTest, AddedKeysAlwaysMightContain) {
  BloomFilter filter(1000, 0.01);
  for (KeyType k = 0; k < 1000; ++k) {
    filter.Add(k);
  }
  for (KeyType k = 0; k < 1000; ++k) {
    EXPECT_TRUE(filter.MightContain(k)) << "false negative for key " << k;
  }
}

TEST(BloomFilterTest, FalsePositiveRateIsRoughlyAsConfigured) {
  constexpr size_t kNumKeys = 5000;
  constexpr double kTargetRate = 0.01;
  BloomFilter filter(kNumKeys, kTargetRate);

  for (KeyType k = 0; k < static_cast<KeyType>(kNumKeys); ++k) {
    filter.Add(k);
  }

  // Query keys guaranteed not to have been added.
  int false_positives = 0;
  constexpr int kNumQueries = 20000;
  for (KeyType k = static_cast<KeyType>(kNumKeys); k < static_cast<KeyType>(kNumKeys) + kNumQueries; ++k) {
    if (filter.MightContain(k)) {
      ++false_positives;
    }
  }
  double observed_rate = static_cast<double>(false_positives) / kNumQueries;
  // Generous bound: real false-positive rate should be in the right
  // ballpark, not exact (it's a probabilistic structure).
  EXPECT_LT(observed_rate, kTargetRate * 5) << "observed rate " << observed_rate;
}

TEST(BloomFilterTest, SerializeThenReconstructPreservesBehavior) {
  BloomFilter filter(500, 0.01);
  for (KeyType k = 0; k < 500; ++k) {
    filter.Add(k * 2);  // even keys only
  }

  BloomFilter reconstructed(filter.ToBytes(), filter.NumBits(), filter.NumHashes());
  for (KeyType k = 0; k < 500; ++k) {
    EXPECT_TRUE(reconstructed.MightContain(k * 2));
  }
}

}  // namespace
}  // namespace dbengine
