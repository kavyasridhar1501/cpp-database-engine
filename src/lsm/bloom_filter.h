#pragma once

#include <cstdint>
#include <vector>

#include "common/config.h"

namespace dbengine {

// Bit-array Bloom filter over KeyType keys, using double hashing (Kirsch &
// Mitzenmacher) to derive k hash functions from two 64-bit mixes. Fronts
// each SSTable so a "definitely absent" lookup can skip a disk read.
class BloomFilter {
 public:
  // Sizes the filter for `expected_keys` entries at `false_positive_rate`
  // (e.g. 0.01 for 1%), using the standard optimal-size formulas.
  BloomFilter(size_t expected_keys, double false_positive_rate);

  // Reconstructs a filter from previously-serialized bits (see ToBytes()),
  // for reopening an SSTable without rebuilding its filter.
  BloomFilter(std::vector<uint8_t> bytes, size_t num_bits, int num_hashes);

  void Add(KeyType key);

  // False means `key` is definitely absent. True means "probably present"
  // (or a false positive at rate ~false_positive_rate).
  bool MightContain(KeyType key) const;

  size_t NumBits() const { return num_bits_; }
  int NumHashes() const { return num_hashes_; }
  const std::vector<uint8_t>& ToBytes() const { return bits_; }

 private:
  static uint64_t Mix(uint64_t x, uint64_t seed);
  size_t BitIndex(uint64_t h1, uint64_t h2, int i) const;

  std::vector<uint8_t> bits_;
  size_t num_bits_;
  int num_hashes_;
};

}  // namespace dbengine
