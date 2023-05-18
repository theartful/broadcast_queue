#define BITMAP_ALLOCATOR_DEBUG

#include <gtest/gtest.h>

#include "bitmap_allocator.h"
#include <array>
#include <chrono>
#include <random>
#include <thread>

template <int Alignment, int Size> struct alignas(Alignment) AlignedStruct {
  std::array<char, Size> data;

  bool operator==(const AlignedStruct &other) const {
    return data == other.data;
  }
};

using MyTypes =
    ::testing::Types<float, double, std::array<char, 128>,
                     std::array<char, 256>, std::array<char, 512>,
                     AlignedStruct<16, 1>, AlignedStruct<32, 8>,
                     AlignedStruct<1, 16>, AlignedStruct<8, 32>, std::string>;

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

template <> inline std::string new_value<std::string>(int idx) {
  return std::string("string number: ") + std::to_string(idx);
}

template <typename T, typename Alloc> T *allocate_and_construct(Alloc alloc) {
  using ReboundAlloc =
      typename std::allocator_traits<Alloc>::template rebind_alloc<T>;

  auto rebound_alloc = ReboundAlloc(alloc);

  T *pointer = std::allocator_traits<ReboundAlloc>::allocate(rebound_alloc, 1);
  std::allocator_traits<ReboundAlloc>::construct(rebound_alloc, pointer);

  return pointer;
}

template <typename T, typename Alloc>
void destroy_and_deallocate(Alloc alloc, T *pointer) {
  using ReboundAlloc =
      typename std::allocator_traits<Alloc>::template rebind_alloc<T>;

  auto rebound_alloc = ReboundAlloc(alloc);

  std::allocator_traits<ReboundAlloc>::destroy(rebound_alloc, pointer);
  std::allocator_traits<ReboundAlloc>::deallocate(rebound_alloc, pointer, 1);
}

template <typename T> bool is_properly_aligned(T *pointer) {
  void *pointer_void = static_cast<void *>(pointer);
  size_t space = sizeof(T);

  return std::align(alignof(T),   // alignment
                    sizeof(T),    // size
                    pointer_void, // ptr
                    space         // space
                    ) == static_cast<void *>(pointer);
}

TYPED_TEST(BitmapAllocator, BasicOperationSingleThreaded) {
  using namespace broadcast_queue;

  constexpr size_t STORAGE_SIZE = 1024;

  auto storage = bitmap_allocator_storage::create<TypeParam>(STORAGE_SIZE);
  auto allocator = bitmap_allocator<void *>{&storage};

  std::vector<TypeParam *> vec;
  std::vector<TypeParam> mirror;

  auto check_vec_mirror_equality = [&]() {
    EXPECT_EQ(vec.size(), mirror.size());
    for (size_t i = 0; i < vec.size(); i++) {
      EXPECT_EQ(*vec[i], mirror[i]);
    }
  };

  for (size_t i = 0; i < STORAGE_SIZE; i++) {
    TypeParam *value = allocate_and_construct<TypeParam>(allocator);

    EXPECT_TRUE(is_properly_aligned(value))
        << "required alignment is " << alignof(TypeParam)
        << ", but instead we got the pointer: " << (void *)value
        << " which is not properly aligned!";

    *value = new_value<TypeParam>(i);
    vec.push_back(value);
    mirror.push_back(new_value<TypeParam>(i));
    EXPECT_EQ(storage.num_allocated(), i + 1);
  }

  check_vec_mirror_equality();

  std::random_device rd;
  std::mt19937 prng{rd()};

  for (size_t i = 0; i < STORAGE_SIZE; i++) {
    std::uniform_int_distribution<size_t> dist(0ULL, vec.size() - 1);
    auto diff = dist(prng);
    destroy_and_deallocate(allocator, vec[diff]);
    vec.erase(vec.begin() + diff);
    mirror.erase(mirror.begin() + diff);

    EXPECT_EQ(storage.num_allocated(), 1024 - i - 1);
  }

  for (int i = 0; i < STORAGE_SIZE; i++) {
    TypeParam *value = allocate_and_construct<TypeParam>(allocator);

    EXPECT_TRUE(is_properly_aligned(value))
        << "required alignment is " << alignof(TypeParam)
        << ", but instead we got the pointer: " << (void *)value
        << " which is not properly aligned!";

    *value = new_value<TypeParam>(i);
    vec.push_back(value);
    mirror.push_back(new_value<TypeParam>(i));

    EXPECT_EQ(storage.num_allocated(), i + 1);
  }
  check_vec_mirror_equality();

  // now this should use the fallback allocator
  for (int i = 0; i < STORAGE_SIZE; i++) {
    TypeParam *value = allocate_and_construct<TypeParam>(allocator);

    // std::allocator does not respect the alignment requirements
    // EXPECT_TRUE(is_properly_aligned(value))
    //     << "required alignment is " << alignof(TypeParam)
    //     << ", but instead we got the pointer: " << (void *)value
    //     << " which is not properly aligned!";

    *value = new_value<TypeParam>(i);
    vec.push_back(value);
    mirror.push_back(new_value<TypeParam>(i));
  }

  EXPECT_EQ(storage.num_allocated(), 1024);
  check_vec_mirror_equality();

  // TODO: I shuold actually check  that the fallback allocator does the
  // deallocation and doesn't leak
  for (int i = 0; i < STORAGE_SIZE * 2; i++) {
    std::uniform_int_distribution<size_t> dist(0ULL, vec.size() - 1);
    auto diff = dist(prng);
    destroy_and_deallocate(allocator, vec[diff]);
    vec.erase(vec.begin() + diff);
    mirror.erase(mirror.begin() + diff);
  }

  EXPECT_EQ(storage.num_allocated(), 0);
  check_vec_mirror_equality();
}

TYPED_TEST(BitmapAllocator, BasicOperationMultiThreaded) {
  using namespace broadcast_queue;
  constexpr size_t STORAGE_SIZE = 16384;
  constexpr size_t NUM_THREADS = 16;
  constexpr size_t STORAGE_PER_THREAD = STORAGE_SIZE / NUM_THREADS;
  constexpr auto TEST_DURATION = std::chrono::seconds(10);

  auto storage = bitmap_allocator_storage::create<TypeParam>(STORAGE_SIZE);
  auto allocator = bitmap_allocator<void *>{&storage};

  std::vector<std::thread> threads;
  std::atomic<bool> should_stop{false};

  auto thread_lambda = [&]() {
    std::vector<TypeParam *> vec;
    std::vector<TypeParam> mirror;

    auto check_vec_mirror_equality = [&]() {
      EXPECT_EQ(vec.size(), mirror.size());
      for (size_t i = 0; i < vec.size(); i++) {
        EXPECT_EQ(*vec[i], mirror[i]);
      }
    };

    int new_value_idx = 0;

    std::random_device rd;
    std::mt19937 prng{rd()};

    while (!should_stop) {
      for (int i = 0; i < STORAGE_PER_THREAD; i++) {
        TypeParam *value = allocate_and_construct<TypeParam>(allocator);

        EXPECT_TRUE(is_properly_aligned(value))
            << "required alignment is " << alignof(TypeParam)
            << ", but instead we got the pointer: " << (void *)value
            << " which is not properly aligned!";

        *value = new_value<TypeParam>(new_value_idx);
        vec.push_back(value);
        mirror.push_back(new_value<TypeParam>(new_value_idx));
        new_value_idx++;
      }
      check_vec_mirror_equality();

      for (int i = 0; i < STORAGE_PER_THREAD; i++) {
        std::uniform_int_distribution<size_t> dist(0ULL, vec.size() - 1);
        auto diff = dist(prng);
        destroy_and_deallocate(allocator, vec[diff]);
        vec.erase(vec.begin() + diff);
        mirror.erase(mirror.begin() + diff);
        check_vec_mirror_equality();
      }
    }
  };

  for (size_t _ = 0; _ < NUM_THREADS; _++) {
    threads.push_back(std::thread{thread_lambda});
  }

  std::this_thread::sleep_for(TEST_DURATION);
  should_stop = true;
  for (auto &thread : threads)
    thread.join();
}
