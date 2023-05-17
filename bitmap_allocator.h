#ifndef THEARTFUL_BITMAP_ALLOCATOR_H
#define THEARTFUL_BITMAP_ALLOCATOR_H

#include <atomic>     // for atomics needed for lock free CAS operations
#include <cstdint>    // for fixed width integer types such as uint64_t
#include <memory>     // for std::align
                      // and for std::allocator
#include <cassert>    // for assertions
#include <functional> // for greater_equal and less_equal
#if defined(__cpp_lib_bitops)
#include <bit> // for countr_zero
#elif defined(_MSC_VER)
#include <intrin.h> // for _BitScanForward
#endif

#ifdef BITMAP_ALLOCATOR_DEBUG
#include <cstdio>
#define BITMAP_ALLOCATOR_DEBUG_LOG(...)                                        \
  do {                                                                         \
    printf(__VA_ARGS__);                                                       \
  } while (false)
#else
#define BITMAP_ALLOCATOR_DEBUG_LOG(...)
#endif

namespace broadcast_queue {

static inline int ctz_fallback(uint64_t x) {
  // TODO
  return 0;
}

// count trailing zeros from the least significant bit
static inline int ctz(uint64_t x) {
#if defined(__cpp_lib_bitops)
  return std::countr_zero(x);
#elif defined(__GNUC__)
  if (x == 0) {
    return 64; // __builtin_ctz is undefined when x is 0
  }
  return __builtin_ctzll(x);
#elif defined(_MSC_VER) && defined(_M_IX86)  // x86
  unsigned long result;
  uint32_t low = x & 0x00000000FFFFFFFF;
  if (!_BitScanForward(&result, low)) {
    return static_cast<int>(result);
  }
  uint32_t high = (x & 0xFFFFFFFF00000000) >> 32;
  if (!_BitScanForward(&result, high)) {
    return 64;
  } else {
    return static_cast<int>(result) + 32;
  }
#elif defined(_MSC_VER) && !defined(_M_IX86) // x64
  unsigned long result;
  if (!_BitScanForward64(&result, x)) {
    return 64;
  } else {
    return static_cast<int>(result);
  }
#else
  return ctz_fallback(x);
#endif
}

class bitmap_allocator_storage {
  static_assert(sizeof(std::atomic<uint64_t>) == sizeof(uint64_t),
                "Expected sizeof(std::atomic<uint64_t>) == sizeof(uint64_t)");
#ifdef __cpp_lib_atomic_is_always_lock_free
  static_assert(std::atomic<uint64_t>::is_always_lock_free,
                "Expected std::atomic<uint64_t> to be always lock-free");
#endif

public:
  template <typename T>
  static bitmap_allocator_storage create(size_t capacity) {
    return bitmap_allocator_storage(sizeof(T), alignof(T), capacity);
  }

  bitmap_allocator_storage(size_t block_size, size_t block_alignment,
                           size_t capacity)
      : m_block_size{block_size}, m_block_alignment{block_alignment},
        m_capacity{capacity} {

    // every element occupies one bit
    m_bitmap_size = m_capacity / 64;

    // add the remaining elements
    if (m_capacity % 64 != 0)
      m_bitmap_size++;

    assert(m_bitmap_size * 64 >= m_capacity);

    // block size has to be a multiple of the alignment
    m_block_size += (m_block_alignment - m_block_size % m_block_alignment) %
                    m_block_alignment;

    assert(m_block_size % m_block_alignment == 0);

    size_t storage_size = alignof(std::atomic<uint64_t>) +
                          m_bitmap_size * sizeof(std::atomic<uint64_t>) +
                          block_alignment + m_block_size * m_capacity;

    m_storage = new unsigned char[storage_size];

    void *storage_ptr = static_cast<void *>(m_storage);
    size_t space = storage_size;

    m_bitmap = static_cast<std::atomic<uint64_t> *>(
        std::align(alignof(std::atomic<uint64_t>),                // alignment
                   m_bitmap_size * sizeof(std::atomic<uint64_t>), // size
                   storage_ptr,                                   // ptr
                   space                                          // space
                   ));

    void *m_bitmap_end = m_bitmap + m_bitmap_size;
    space -= m_bitmap_size * sizeof(std::atomic<uint64_t>);

    m_blocks = static_cast<unsigned char *>(
        std::align(m_block_alignment,         // alignment
                   m_block_size * m_capacity, // size
                   m_bitmap_end,              // ptr
                   space                      // space
                   ));

    assert(m_blocks != nullptr);
    assert(m_bitmap != nullptr);

    // 1 is free, 0 is full
    for (size_t i = 0; i < m_bitmap_size; i++)
      ::new (m_bitmap + i) std::atomic<uint64_t>{0xFFFFFFFFFFFFFFFF};

#ifdef BITMAP_ALLOCATOR_DEBUG
    m_num_allocated.store(0, std::memory_order_relaxed);
#endif
  }

  bitmap_allocator_storage(bitmap_allocator_storage &&other) {
    m_block_size = other.m_block_size;
    m_block_alignment = other.m_block_alignment;
    m_capacity = other.m_capacity;
    m_bitmap_size = other.m_bitmap_size;
    m_storage = other.m_storage;
    m_bitmap = other.m_bitmap;
    m_blocks = other.m_blocks;

    other.m_capacity = 0;
    other.m_storage = nullptr;
    other.m_bitmap = nullptr;
    other.m_blocks = nullptr;
  }

  bitmap_allocator_storage(const bitmap_allocator_storage &) = delete;
  bitmap_allocator_storage &
  operator=(const bitmap_allocator_storage &) = delete;

  size_t alignment() { return m_block_alignment; }
  size_t capacity() { return m_capacity; }
  size_t block_size() { return m_block_size; }

  ~bitmap_allocator_storage() { delete[] m_storage; }

  void *allocate_block() {
    // TODO: we can use some sort of heuristic that would do better than blindly
    // traversing the whole bitmap each time we allocate

    // we only try one pass
    for (size_t i = 0; i < m_bitmap_size; i++) {
      void *ptr = try_allocate(i);
      if (ptr)
        return ptr;
    }

    return nullptr;
  }

  bool is_one_of_ours(void *block) {
    // FIXME: this is not exactly kosher, and is not correct on non flat memory
    // architectures
    // I'm bargaining on cppreference which claims:
    // "A specialization of std::less_equal for any pointer type yields the
    // implementation-defined strict total order, even if the built-in <=
    // operator does not."
    // see: https://en.cppreference.com/w/cpp/utility/functional/less_equal
    // now any sane implementation of this would enforce this total ordering
    // using the actual pointer address in the flat memory
    return std::greater_equal<void *>{}(block, static_cast<void *>(m_blocks)) &&
           std::less<void *>{}(
               block,
               static_cast<void *>(m_blocks + m_block_size * m_capacity));
  }

  bool deallocate_block(void *block) {
    if (!is_one_of_ours(block))
      return false;

    // now we're sure that we did allocate this block
    size_t idx = std::distance(m_blocks, static_cast<unsigned char *>(block)) /
                 m_block_size;

    return deallocate_block(idx);
  }

  bool deallocate_block(size_t idx) {
    size_t field_idx = idx / 64;
    size_t bit_idx = idx % 64;

    std::atomic<uint64_t> &field = m_bitmap[field_idx];

    // we're assuming that only one thread deallocates a block
    while (true) {
      uint64_t value = field.load(std::memory_order_relaxed);
      uint64_t new_value = value | (1ULL << bit_idx);

      // TODO: check which is better the strong or the weak version
      if (field.compare_exchange_strong(value, new_value,
                                        std::memory_order_relaxed)) {
#ifdef BITMAP_ALLOCATOR_DEBUG
        m_num_allocated.fetch_sub(1, std::memory_order_relaxed);
#endif
        return true;
      }
    }
  }

  void *try_allocate(size_t idx) {
    std::atomic<uint64_t> &field = m_bitmap[idx];

    while (true) {
      uint64_t value = field.load(std::memory_order_relaxed);

      if (value == 0)
        return nullptr;

      int bit_idx = ctz(value);
      uint64_t new_value = value & ~(1ULL << bit_idx);

      // TODO: check which is better the strong or the weak version
      if (field.compare_exchange_strong(value, new_value,
                                        std::memory_order_relaxed)) {
#ifdef BITMAP_ALLOCATOR_DEBUG
        m_num_allocated.fetch_add(1, std::memory_order_relaxed);
#endif
        return m_blocks + ((idx * 64 + bit_idx) * m_block_size);
      }
    }
  }

#ifdef BITMAP_ALLOCATOR_DEBUG
  size_t num_allocated() {
    return m_num_allocated.load(std::memory_order_relaxed);
  }
#endif

private:
  size_t m_block_size;
  size_t m_block_alignment;
  size_t m_capacity;

  size_t m_bitmap_size;
  unsigned char *m_storage;

  std::atomic<uint64_t> *m_bitmap;
  unsigned char *m_blocks;

#ifdef BITMAP_ALLOCATOR_DEBUG
  std::atomic<size_t> m_num_allocated;
#endif
};

template <typename T, typename FallbackAllocator = std::allocator<T>>
class bitmap_allocator {
  template <typename, typename> friend class bitmap_allocator;

  static_assert(std::is_same<typename FallbackAllocator::value_type, T>::value,
                "The fallback allocator doesn't operate on the same type");

public:
  using value_type = T;
  using size_type = size_t;
  using difference_type = std::ptrdiff_t;
  using pointer = T *;
  using const_pointer = const T *;
  using state_type = bitmap_allocator_storage;
  using fallback_allocator_type = FallbackAllocator;

  template <typename U> struct rebind {
    using other = bitmap_allocator<
        U, typename std::allocator_traits<
               fallback_allocator_type>::template rebind_alloc<U>>;
  };

  bitmap_allocator(state_type *state_) : state{state_} {}

  template <typename U, typename Alloc>
  bitmap_allocator(const bitmap_allocator<U, Alloc> &other)
      : state{other.state}, fallback_allocator{other.fallback_allocator} {}

  template <typename U, typename Alloc>
  bitmap_allocator(bitmap_allocator<U, Alloc> &&other)
      : state{other.state}, fallback_allocator{other.fallback_allocator} {}

  template <typename U, typename Alloc>
  bitmap_allocator &operator=(bitmap_allocator<U, Alloc> &&other) {
    state = other.state;
    fallback_allocator = other.fallback_allocator;
  }

  template <typename U, typename Alloc>
  bitmap_allocator &operator=(const bitmap_allocator<U, Alloc> &other) {
    state = other.state;
    fallback_allocator = other.fallback_allocator;
  }

  pointer allocate(size_type n) {
    // TOOD: relax these requirements
    if (alignof(T) != state->alignment() || sizeof(T) > state->block_size()) {
      BITMAP_ALLOCATOR_DEBUG_LOG(
          "bitmap_allocator: alignment = %zu, block_size = %zu while "
          "requested alignment = %zu, and block_size = %zu. falling back "
          "to the fallback allocator\n",
          state->alignment(), state->block_size(), alignof(T), sizeof(T));

      return fallback_allocate(n);
    } else {
      if (n != 1) {
        BITMAP_ALLOCATOR_DEBUG_LOG("bitmap_allocator: requesting %zu elements. "
                                   "falling back to the fallback allocator\n",
                                   n);
        return fallback_allocate(n);
      }

      T *ptr = static_cast<T *>(state->allocate_block());
      if (!ptr) {
        BITMAP_ALLOCATOR_DEBUG_LOG(
            "bitmap_allocator: allocator storage is full. falling back to the "
            "fallback allocator!\n");

        return fallback_allocate(n);
      }
      return ptr;
    }
  }

  void deallocate(pointer p, size_type n) {
    if (n != 1) {
      fallback_deallocate(p, n);
    } else {
      if (!state->deallocate_block(static_cast<void *>(p))) {
        fallback_deallocate(p, n);
      }
    }
  }

private:
  pointer fallback_allocate(size_type n) {
    return fallback_allocator.allocate(n);
  }

  void fallback_deallocate(pointer p, size_type n) {
    fallback_allocator.deallocate(p, n);
  }

private:
  state_type *state;
  fallback_allocator_type fallback_allocator;
};

} // namespace broadcast_queue

#undef BITMAP_ALLOCATOR_DEBUG_LOG

#endif // THEARTFUL_BITMAP_ALLOCATOR_H
