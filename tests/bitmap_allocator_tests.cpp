#include <array>
#include <gtest/gtest.h>
#include <list>
#include <random>
#include <iterator>

#define BITMAP_ALLOCATOR_DEBUG
#include "bitmap_allocator.h"

using MyTypes = ::testing::Types<int, float, double>;

template <typename T> class BitmapAllocator : public testing::Test {};

TYPED_TEST_SUITE(BitmapAllocator, MyTypes);

// pretty much std::iota
template <typename T> T new_value(int idx) {
  T data;
  char *ptr = (char *)&data;
  int cur = idx;
  char *cur_bytes = (char *)(&cur);
  int cur_bytes_idx = 0;

  for (int i = 0; i < sizeof(data); i++) {
    ptr[i] = cur_bytes[cur_bytes_idx++];

    if (cur_bytes_idx == sizeof(cur)) {
      cur++;
      cur_bytes_idx = 0;
    }
  }

  return data;
}

TYPED_TEST(BitmapAllocator, BasicOperation) {
  using namespace broadcast_queue;

  struct ListNode {
    TypeParam data;
    void *prev;
    void *next;
  };

  auto storage = bitmap_allocator_storage::create<ListNode>(1024);

  std::list<TypeParam, bitmap_allocator<TypeParam>> list{
      bitmap_allocator<TypeParam>{&storage}};
  std::vector<TypeParam> mirror;

  auto check_list_mirror_equality = [&]() {
      int i = 0;
      for (auto it = list.begin(); it != list.end(); ++it) {
        EXPECT_EQ(*it, mirror[i++]);
      }
  };

  for (int i = 0; i < 1024; i++) {
    list.push_back(new_value<TypeParam>(i));
    mirror.push_back(new_value<TypeParam>(i));
  }

  EXPECT_EQ(storage.num_allocated(), 1024);
  check_list_mirror_equality();

  std::random_device rd;
  std::mt19937 prng{rd()};

  for (int i = 0; i < 1024; i++) {
    std::uniform_int_distribution<size_t> dist(0ULL, list.size() - 1);
    auto it = list.begin();
    auto diff = dist(prng);
    std::advance(it, diff);
    list.erase(it);
    mirror.erase(mirror.begin() + diff);
    EXPECT_EQ(storage.num_allocated(), 1024 - i - 1);
  }
  check_list_mirror_equality();

  for (int i = 0; i < 2048; i++) {
    list.push_back(new_value<TypeParam>(i));
    mirror.push_back(new_value<TypeParam>(i));
  }
  EXPECT_EQ(storage.num_allocated(), 1024);
  check_list_mirror_equality();

  for (int i = 0; i < 2048; i++) {
    std::uniform_int_distribution<size_t> dist(0ULL, list.size() - 1);
    auto it = list.begin();
    auto diff = dist(prng);
    std::advance(it, diff);
    list.erase(it);
    mirror.erase(mirror.begin() + diff);
  }
  EXPECT_EQ(storage.num_allocated(), 0);
  check_list_mirror_equality();
}
