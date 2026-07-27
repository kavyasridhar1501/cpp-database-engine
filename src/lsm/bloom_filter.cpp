#include "lsm/bloom_filter.h"

#include <algorithm>
#include <cmath>

namespace dbengine {

namespace {

size_t OptimalNumBits(size_t expected_keys, double false_positive_rate) {
  if (expected_keys == 0) {
    expected_keys = 1;
  }
  double m = -static_cast<double>(expected_keys) * std::log(false_positive_rate) /
             (std::log(2.0) * std::log(2.0));
  return std::max<size_t>(8, static_cast<size_t>(std::ceil(m)));
}

int OptimalNumHashes(size_t num_bits, size_t expected_keys) {
  if (expected_keys == 0) {
    expected_keys = 1;
  }
  double k = (static_cast<double>(num_bits) / static_cast<double>(expected_keys)) * std::log(2.0);
  return std::clamp(static_cast<int>(std::round(k)), 1, 32);
}

}  // namespace

BloomFilter::BloomFilter(size_t expected_keys, double false_positive_rate)
    : num_bits_(OptimalNumBits(expected_keys, false_positive_rate)),
      num_hashes_(OptimalNumHashes(num_bits_, expected_keys)) {
  bits_.assign((num_bits_ + 7) / 8, 0);
}

BloomFilter::BloomFilter(std::vector<uint8_t> bytes, size_t num_bits, int num_hashes)
    : bits_(std::move(bytes)), num_bits_(num_bits), num_hashes_(num_hashes) {}

uint64_t BloomFilter::Mix(uint64_t x, uint64_t seed) {
  x ^= seed;
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  x = x ^ (x >> 31);
  return x;
}

size_t BloomFilter::BitIndex(uint64_t h1, uint64_t h2, int i) const {
  return static_cast<size_t>((h1 + static_cast<uint64_t>(i) * h2) % num_bits_);
}

void BloomFilter::Add(KeyType key) {
  uint64_t h1 = Mix(static_cast<uint64_t>(key), 0x9E3779B97F4A7C15ULL);
  uint64_t h2 = Mix(static_cast<uint64_t>(key), 0xC2B2AE3D27D4EB4FULL);
  for (int i = 0; i < num_hashes_; ++i) {
    size_t bit = BitIndex(h1, h2, i);
    bits_[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
  }
}

bool BloomFilter::MightContain(KeyType key) const {
  uint64_t h1 = Mix(static_cast<uint64_t>(key), 0x9E3779B97F4A7C15ULL);
  uint64_t h2 = Mix(static_cast<uint64_t>(key), 0xC2B2AE3D27D4EB4FULL);
  for (int i = 0; i < num_hashes_; ++i) {
    size_t bit = BitIndex(h1, h2, i);
    if ((bits_[bit / 8] & static_cast<uint8_t>(1u << (bit % 8))) == 0) {
      return false;
    }
  }
  return true;
}

}  // namespace dbengine
