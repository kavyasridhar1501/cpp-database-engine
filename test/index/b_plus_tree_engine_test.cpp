#include "index/b_plus_tree_engine.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

namespace dbengine {
namespace {

class BPlusTreeEngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    file_path_ = (std::filesystem::temp_directory_path() /
                  ("dbengine_bpt_engine_test_" + std::to_string(reinterpret_cast<uintptr_t>(this)) +
                   ".db"))
                     .string();
    std::filesystem::remove(file_path_);
  }

  void TearDown() override { std::filesystem::remove(file_path_); }

  std::string file_path_;
};

TEST_F(BPlusTreeEngineTest, PutGetDeleteRoundTrip) {
  BPlusTreeEngine engine(file_path_, 16);
  std::string value;

  EXPECT_FALSE(engine.Get(1, &value));

  engine.Put(1, "hello");
  ASSERT_TRUE(engine.Get(1, &value));
  EXPECT_EQ(value, "hello");

  engine.Put(1, "updated");
  ASSERT_TRUE(engine.Get(1, &value));
  EXPECT_EQ(value, "updated");

  EXPECT_TRUE(engine.Delete(1));
  EXPECT_FALSE(engine.Get(1, &value));
  EXPECT_FALSE(engine.Delete(1));
}

TEST_F(BPlusTreeEngineTest, ScanReturnsAscendingKeys) {
  BPlusTreeEngine engine(file_path_, 16);
  for (KeyType k = 0; k < 100; ++k) {
    engine.Put(k, "v" + std::to_string(k));
  }

  auto it = engine.Scan(INVALID_KEY);
  KeyType expected = 0;
  int count = 0;
  while (it->IsValid()) {
    EXPECT_EQ(it->GetKey(), expected);
    EXPECT_EQ(it->GetValue(), "v" + std::to_string(expected));
    ++expected;
    ++count;
    it->Next();
  }
  EXPECT_EQ(count, 100);
}

TEST_F(BPlusTreeEngineTest, ScanFromMidpoint) {
  BPlusTreeEngine engine(file_path_, 16);
  for (KeyType k = 0; k < 50; ++k) {
    engine.Put(k, "v" + std::to_string(k));
  }
  auto it = engine.Scan(25);
  ASSERT_TRUE(it->IsValid());
  EXPECT_EQ(it->GetKey(), 25);
}

TEST_F(BPlusTreeEngineTest, DataPersistsAcrossEngineReopen) {
  {
    BPlusTreeEngine engine(file_path_, 16);
    for (KeyType k = 0; k < 200; ++k) {
      engine.Put(k, "persisted-" + std::to_string(k));
    }
  }
  {
    BPlusTreeEngine engine(file_path_, 16);
    for (KeyType k = 0; k < 200; ++k) {
      std::string value;
      ASSERT_TRUE(engine.Get(k, &value)) << "missing key " << k << " after reopen";
      EXPECT_EQ(value, "persisted-" + std::to_string(k));
    }
  }
}

TEST_F(BPlusTreeEngineTest, PutValueExceedingMaxSizeThrows) {
  BPlusTreeEngine engine(file_path_, 16);
  std::string too_big(MAX_VALUE_SIZE + 1, 'x');
  EXPECT_THROW(engine.Put(1, too_big), std::invalid_argument);
}

}  // namespace
}  // namespace dbengine
