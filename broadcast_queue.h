#ifndef THEARTFUL_BROADCAST_QUEUE
#define THEARTFUL_BROADCAST_QUEUE

#include <atomic>             // for atomic data types
#include <chrono>             // for time
#include <condition_variable> // for condition variables obviously
#include <cstdint>            // for int types
#include <cstring>            // for memcpy
#include <memory>             // for smart pointers
#include <mutex>              // for mutexes obviously
#include <thread>             // for yielding the thread
#include <type_traits>        // for all sorts of type operations

#if __linux__
#include "futex_waiting_strategy.h"
#else
#include "condition_variable_waiting_strategy.h"
#endif

#include "bitmap_allocator.h"

// implements a fixed-size single producer multiple consumer fan-out circular
// queue of POD structs where new data is sent to all consumers.
//
// see: "Can Seqlocks Get Along With Programming Language Memory Models?" by
// Hans Bohem (https://www.hpl.hp.com/techreports/2012/HPL-2012-68.pdf)

#ifndef BROADCAST_QUEUE_CACHE_LINE_SIZE
#define BROADCAST_QUEUE_CACHE_LINE_SIZE 64
#endif

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define _BROADCAST_QUEUE_TSAN
#endif
#endif

#if defined(__amd64__) || defined(__x86_64__) || defined(_M_AMD64) ||          \
    defined(_M_X64) || defined(_M_IX86)
#define _BROADCAST_QUEUE_TSO_MODEL
#endif

#if !defined(_MSC_VER)
#define _BROADCAST_QUEUE_NO_UNIQUE_ADDRESS [[no_unique_address]]
#else
#define _BROADCAST_QUEUE_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#endif

namespace broadcast_queue {

enum class Error {
  None,
  Timeout,
  Lagged,
  Closed,
};

namespace details {

template <typename T> struct alignas(uint64_t) value_with_sequence_number {
  uint32_t sequence_number;
  T value;
};

template <typename T> struct is_always_lock_free {
#ifdef __cpp_lib_atomic_is_always_lock_free
  static constexpr bool value = std::atomic<T>::is_always_lock_free;
#else
  static constexpr bool value = sizeof(T) < 8 &&
                                std::is_trivially_destructible<T>::value &&
                                std::is_trivially_copyable<T>::value &&
                                std::is_trivially_constructible<T>::value;
#endif
};

template <typename T, typename WaitingStrategy, typename = void>
class storage_block {};

template <typename T, typename WaitingStrategy, typename = void>
class queue_data {};

template <typename T, typename WaitingStrategy>
class storage_block<T, WaitingStrategy,
                    typename std::enable_if<is_always_lock_free<
                        value_with_sequence_number<T>>::value>::type> {
public:
  using waiting_strategy = WaitingStrategy;

public:
  storage_block() { m_storage.store({0, 0}, std::memory_order_relaxed); }

  void store(const T &value) {
    auto old_storage = m_storage.load(std::memory_order_relaxed);
    uint32_t old_sn = old_storage.sequence_number;
    value_with_sequence_number<T> new_storage = {old_sn + 2, value};
    m_storage.store(new_storage, std::memory_order_relaxed);
  }

  void notify() {
    // is this ok?
    std::atomic<uint32_t> *sequence_number =
        reinterpret_cast<std::atomic<uint32_t> *>(&m_storage);

    m_waiter.notify(*sequence_number);
  }

  template <typename Rep, typename Period>
  bool wait(uint32_t old_sequence_number,
            const std::chrono::duration<Rep, Period> &timeout) {

    constexpr int atomic_spin_count = 1024;

    for (int i = 0; i < atomic_spin_count; i++) {
      auto sequence_number =
          m_storage.load(std::memory_order_relaxed).sequence_number;

      if (sequence_number != old_sequence_number)
        return true;

      std::this_thread::yield();
    }

    // is this ok?
    std::atomic<uint32_t> *sequence_number =
        reinterpret_cast<std::atomic<uint32_t> *>(&m_storage);

    return m_waiter.wait(*sequence_number, old_sequence_number, timeout);
  }

  bool try_load(T *value, uint32_t *read_sequence_number,
                const std::chrono::steady_clock::time_point /* until */) {
    auto old_storage = m_storage.load(std::memory_order_relaxed);
    *value = old_storage.value;
    *read_sequence_number = old_storage.sequence_number;

    return true;
  }

  void load_nosync(T *value) const {
    *value = m_storage.load(std::memory_order_relaxed).value;
  }

  void store_nosync(const T &value) {
    m_storage.store(value, std::memory_order_relaxed);
  }

  uint32_t
  sequence_number(std::memory_order order = std::memory_order_relaxed) const {
    return m_storage.load(order).sequence_number;
  }

  const void *sequence_number_address() const {
    return &reinterpret_cast<const value_with_sequence_number<T> *>(&m_storage)
                ->sequence_number;
  }

private:
  std::atomic<value_with_sequence_number<T>> m_storage;
  waiting_strategy m_waiter;
};

template <typename T, typename WaitingStrategy>
class storage_block<T, WaitingStrategy,
                    typename std::enable_if<!is_always_lock_free<
                        value_with_sequence_number<T>>::value>::type> {
public:
  using waiting_strategy = WaitingStrategy;

public:
  storage_block() { m_sequence_number.store(0, std::memory_order_relaxed); }

  void store(const T &value) {
    uint32_t sn = m_sequence_number.load(std::memory_order_relaxed);

    m_sequence_number.store(sn + 1, std::memory_order_relaxed);

#if defined(_BROADCAST_QUEUE_TSO_MODEL) && !defined(_BROADCAST_QUEUE_TSAN)
    // in TSO architectures (such as x86), we can use normal store operations
    // since store-store opreations will always be ordered the same in memory
    // as in the program, so this fixes the problem in the cpu level, and the
    // signal fence prevents reordering in the compiler level, so we're good
    std::atomic_signal_fence(std::memory_order_release);

    m_storage = value;
#else
    // enforce a happens-before relationship
    // that is: we're sure that the sequence number is incremented before
    // writing the data
    std::atomic_thread_fence(std::memory_order_release);

    const storage_type *value_as_storage =
        reinterpret_cast<const storage_type *>(&value);

    for (size_t i = 0; i < storage_per_element; i++) {
      m_storage[i].store(*(value_as_storage++), std::memory_order_relaxed);
    }
#endif

    // now we're sure that the changes in the storage all happens before the
    // change in the sequence number
    m_sequence_number.store(sn + 2, std::memory_order_release);
  }

  bool try_load(T *value, uint32_t *read_sequence_number,
                const std::chrono::steady_clock::time_point until) {

    do {
      uint32_t sequence_number_before =
          m_sequence_number.load(std::memory_order_acquire);

      // if the writer is in the middle of writing a new value
      if (sequence_number_before & 1) {
        std::this_thread::yield();
        continue;
      }

#if defined(_BROADCAST_QUEUE_TSO_MODEL) && !defined(_BROADCAST_QUEUE_TSAN)
      // in TSO architectures (such as x86), we can use normal load operations
      // since load-load opreations will always be ordered the same in memory
      // as in the program, so this fixes the problem in the cpu level, and the
      // signal fence prevents reordering in the compiler level, so we're good!
      *value = m_storage;

      std::atomic_signal_fence(std::memory_order_acquire);
#else
      storage_type *result_as_storage = reinterpret_cast<storage_type *>(value);
      for (size_t i = 0; i < storage_per_element; i++) {
        result_as_storage[i] = m_storage[i].load(std::memory_order_relaxed);
      }

      // this is used to synchronizes with the thread fence in push
      // this means that any store operation that happened before the value of
      // m_storage is stored will be seen after the fence in subsequent loads
      // more specifically if the value of m_storage has changed while we're
      // reading it, then we have to see the value of m_sequence_number changed
      // after this fence
      std::atomic_thread_fence(std::memory_order_acquire);
#endif

      uint32_t sequence_number_after =
          m_sequence_number.load(std::memory_order_acquire);

      if (sequence_number_after == sequence_number_before) {
        *read_sequence_number = sequence_number_after;
        return true;
      }

    } while (std::chrono::steady_clock::now() < until);

    return false;
  }

  void load_nosync(T *value) const {
#if defined(_BROADCAST_QUEUE_TSO_MODEL) && !defined(_BROADCAST_QUEUE_TSAN)
    *value = m_storage;
#else
    std::memcpy((void *)value, (void *)m_storage, sizeof(T));
#endif
  }

  void store_nosync(const T &value) {
#if defined(_BROADCAST_QUEUE_TSO_MODEL) && !defined(_BROADCAST_QUEUE_TSAN)
    m_storage = value;
#else
    std::memcpy((void *)m_storage, (void *)&value, sizeof(T));
#endif
  }

  uint32_t
  sequence_number(std::memory_order order = std::memory_order_relaxed) const {
    return m_sequence_number.load(order);
  }

  const void *sequence_number_address() const { return &m_sequence_number; }

  void notify() { m_waiter.notify(m_sequence_number); }

  template <typename Rep, typename Period>
  bool wait(uint32_t old_sequence_number,
            const std::chrono::duration<Rep, Period> &timeout) {

    constexpr int atomic_spin_count = 1024;

    for (int i = 0; i < atomic_spin_count; i++) {
      auto sequence_number = m_sequence_number.load(std::memory_order_relaxed);

      if (sequence_number != old_sequence_number)
        return true;

      std::this_thread::yield();
    }

    return m_waiter.wait(m_sequence_number, old_sequence_number, timeout);
  }

private:
#if defined(_BROADCAST_QUEUE_TSO_MODEL) && !defined(_BROADCAST_QUEUE_TSAN)
  T m_storage;
#else
  using storage_type = char;
  static constexpr size_t storage_per_element =
      sizeof(T) / sizeof(storage_type);
  static_assert(sizeof(T) % sizeof(storage_type) == 0,
                "storage_type has to have size multiple of the size of T");
  std::atomic<storage_type> m_storage[storage_per_element];
#endif

  std::atomic<uint32_t> m_sequence_number;
  waiting_strategy m_waiter;
};

struct alignas(uint64_t) Cursor {
  uint32_t pos;             // the position the writer will write on next
  uint32_t sequence_number; // the sequence number of the element on which the
                            // writer will write on next
};

// we don't really check for "podness" but this naming is short and sweet
template <typename T> struct is_pod {
  static constexpr bool value = std::is_trivially_copyable<T>::value &&
                                std::is_trivially_destructible<T>::value;
};

template <typename T, typename WaitingStrategy>
class queue_data<T, WaitingStrategy,
                 typename std::enable_if<is_pod<T>::value>::type> {
public:
  using value_type = T;
  using pointer = T *;
  using waiting_strategy = WaitingStrategy;

  queue_data(size_t capacity_)
      : m_capacity{capacity_}, m_subscribers{0}, m_closed{false},
        m_cursor{Cursor{0, 0}} {

    // uninititalized storage
    m_storage_blocks =
        new storage_block<value_type, waiting_strategy>[m_capacity];
  }

  void push(const value_type &value) {
    Cursor cur = m_cursor.load(std::memory_order_relaxed);
    uint32_t pos = cur.pos;

    auto &block = m_storage_blocks[pos];

    uint32_t sequence_number = block.sequence_number(std::memory_order_relaxed);

    // update cursor indicating we're in the middle of writing
    cur.sequence_number = sequence_number + 1;
    m_cursor.store(cur, std::memory_order_relaxed);

    block.store(value);
    block.notify();

    // update cursor to the next element
    cur.pos = (pos + 1) % m_capacity;
    cur.sequence_number =
        m_storage_blocks[cur.pos].sequence_number(std::memory_order_relaxed);
    m_cursor.store(cur, std::memory_order_relaxed);
  }

  template <typename Rep, typename Period>
  Error read(pointer result, uint32_t *reader_pos,
             uint32_t *reader_sequence_number,
             const std::chrono::duration<Rep, Period> &timeout) {

    if (is_closed())
      return Error::Closed;

    std::chrono::steady_clock::time_point until =
        std::chrono::steady_clock::now() + timeout;

    auto &block = m_storage_blocks[*reader_pos];

    // first wait until sequence number is not the same as reader sequence
    // number
    auto old_sequence_number = *reader_sequence_number - 2;

    if (!block.wait(old_sequence_number, timeout))
      return Error::Timeout;

    uint32_t sequence_number;

    if (block.try_load(result, &sequence_number, until)) {
      if (sequence_number != *reader_sequence_number) {
        Cursor cur = m_cursor.load(std::memory_order_relaxed);

        // lagging will effectively cause resubscription
        *reader_pos = cur.pos;
        *reader_sequence_number = cur.sequence_number;
        if (*reader_sequence_number & 1)
          *reader_sequence_number += 1;
        else
          *reader_sequence_number += 2;

        return Error::Lagged;
      } else {
        *reader_pos = (*reader_pos + 1) % m_capacity;

        if (*reader_pos == 0) {
          // new sequeuce number!
          *reader_sequence_number = sequence_number + 2;
        } else {
          *reader_sequence_number = sequence_number;
        }

        return Error::None;
      }
    } else {
      return Error::Timeout;
    }
  }

  template <typename Rep, typename Period>
  Error read(pointer result, Cursor *cursor,
             const std::chrono::duration<Rep, Period> &timeout) {
    return read(result, &cursor->pos, &cursor->sequence_number, timeout);
  }

  uint32_t
  sequence_number(uint32_t pos,
                  std::memory_order order = std::memory_order_relaxed) {
    return m_storage_blocks[pos].sequence_number(order);
  }

  const storage_block<value_type, waiting_strategy> &block(uint32_t pos) const {
    return m_storage_blocks[pos];
  }

  // I know we can const overload, but I don't like it
  storage_block<value_type, waiting_strategy> &block_nonconst(uint32_t pos) {
    return m_storage_blocks[pos];
  }

  Cursor cursor() { return m_cursor.load(std::memory_order_relaxed); }
  size_t capacity() { return m_capacity; }
  size_t subscribers() { return m_subscribers.load(std::memory_order_relaxed); }
  void subscribe() { m_subscribers.fetch_add(1, std::memory_order_relaxed); }
  void unsubscribe() { m_subscribers.fetch_sub(1, std::memory_order_relaxed); }
  void close() { m_closed.store(true, std::memory_order_relaxed); }
  bool is_closed() { return m_closed.load(std::memory_order_relaxed); }

  ~queue_data() { delete[] m_storage_blocks; }

private:
  size_t m_capacity;
  storage_block<value_type, waiting_strategy> *m_storage_blocks;
  std::atomic<size_t> m_subscribers;
  std::atomic<bool> m_closed;

  // prevents false sharing between the writer who constantly updates this
  // m_cursor and the readers who constantly read m_storage_blocks
  // note the usage of double the cache line, which is to combat the prefetcher
  // which works with pairs of cache lines
  // alignas(2 * BROADCAST_QUEUE_CACHE_LINE_SIZE) is also a possibility, but it
  // would change the alignment of the class, and complicate heap allocation
  // specially in C++11 which doesn't have a standard way for aligned allocation
  char m_padding[2 * BROADCAST_QUEUE_CACHE_LINE_SIZE];

  std::atomic<Cursor> m_cursor;
};

template <typename T> struct nonpod_storage_block {
  T value;
  std::atomic<uint64_t> sequence_number;
  std::atomic<uint64_t> refcount;
};

template <typename T, typename WaitingStrategy>
class queue_data<T, WaitingStrategy,
                 typename std::enable_if<!is_pod<T>::value>::type> {
public:
  using value_type = T;
  using pointer = T *;
  using waiting_strategy = WaitingStrategy;
  using allocator =
      bitmap_allocator<nonpod_storage_block<value_type>,
                       null_allocator<nonpod_storage_block<value_type>>>;
  using allocator_storage = bitmap_allocator_storage;

  queue_data(size_t capacity)
      : m_internal_queue{capacity},
        m_storage{
            bitmap_allocator_storage::create<nonpod_storage_block<value_type>>(
                capacity * 2)},
        m_allocator{&m_storage} {

    for (size_t i = 0; i < m_internal_queue.capacity(); i++) {
      m_internal_queue.block_nonconst(i).store_nosync(nullptr);
    }
  }

  ~queue_data() {
    // go over each block and destroy the value inside
    // even though we have 2 * capacity allocated values, at the end only
    // `capacity` items will be alive, since the others are only extra in the
    // case a reader locks a block, and once the reader is finished, the item
    // will be destroyed
    for (size_t i = 0; i < m_internal_queue.capacity(); i++) {
      nonpod_storage_block<value_type> *block_ptr;
      m_internal_queue.block(i).load_nosync(&block_ptr);

      if (block_ptr) {
        // we don't need to call deallocate since the bitmap allocator doesn't
        // give anything back to the OS anyways until its storage dies
        std::allocator_traits<allocator>::destroy(m_allocator, block_ptr);
      }
    }
  }

  void push(const value_type &value) {
    nonpod_storage_block<value_type> *block = reconstruct(next_block(), value);
    m_internal_queue.push(block);
  }

  template <typename Rep, typename Period>
  Error read(pointer result, Cursor *cursor,
             const std::chrono::duration<Rep, Period> &timeout) {
    nonpod_storage_block<value_type> *block;

    auto old_sequence_number = cursor->sequence_number;
    Error error = m_internal_queue.read(&block, cursor, timeout);

    if (error != Error::None)
      return error;

    auto refcount = block->refcount.load(std::memory_order_relaxed);
    // the block died from underneath us!
    if (refcount == 0) {
      return Error::Timeout;
    }

    // try to increment refcount so that no one will dare destroy the object
    // while we're reading it!
    while (!block->refcount.compare_exchange_weak(refcount, refcount + 1,
                                                  std::memory_order_relaxed)) {
      // the block died from underneath us!
      if (refcount == 0) {
        return Error::Timeout;
      }
    }

    // check the sequence number once more, since the writer might have rewrote
    // on the block from the time we read the block pointer until the time we
    // successfully locked the block
    if (block->sequence_number != old_sequence_number) {
      // we decrement the refcount to unlock the block
      auto old_refcount =
          block->refcount.fetch_sub(1, std::memory_order_acq_rel);

      // the last one that uses a block has to deallocate it
      if (old_refcount == 1) {
        destroy_and_deallocate_block(block);
      }

      return Error::Lagged;
    }

    // okay, we've locked the block and we made sure that it has the sequence
    // number we want, now we copy the value
    *result = block->value;

    // we decrement the refcount to unlock the block, and we set the memory
    // order to release to make sure that the copy constructor finished copying
    // before we decrement the refcount
    auto old_refcount = block->refcount.fetch_sub(1, std::memory_order_acq_rel);

    // the last one that uses a block has to deallocate it
    if (old_refcount == 1) {
      destroy_and_deallocate_block(block);
    }

    return Error::None;
  }

  Cursor cursor() { return m_internal_queue.cursor(); }
  size_t capacity() { return m_internal_queue.capacity(); }
  size_t subscribers() { return m_internal_queue.subscribers(); }
  void subscribe() { m_internal_queue.subscribe(); }
  void unsubscribe() { m_internal_queue.unsubscribe(); }
  void close() { m_internal_queue.close(); }
  bool is_closed() { return m_internal_queue.is_closed(); }

private:
  nonpod_storage_block<value_type> *next_block() {
    // get the data the cursor is pointing at
    auto cursor = m_internal_queue.cursor();
    // we  don't need to synchronize since we're the only writer
    nonpod_storage_block<value_type> *block_ptr;
    m_internal_queue.block(cursor.pos).load_nosync(&block_ptr);

    // this means that this memory block is unallocated yet
    if (!block_ptr ||
        block_ptr->refcount.load(std::memory_order_relaxed) == 0) {
      // we're sure that the allocation will succeed, since we already found
      // an unallocated block, and we're the only ones doing allocations
      return std::allocator_traits<allocator>::allocate(m_allocator, 1);
    } else {
      // refcount is greater than 0
      // the writer is responsible for an increment, so we decrement it to
      // indicate that the writer is done caring about this block
      auto old_refcount =
          block_ptr->refcount.fetch_sub(1, std::memory_order_acq_rel);

      // life is good, nobody cares about this block anymore
      // so we destroy the value inside, and reuse the block
      if (old_refcount == 1) {
        block_ptr->value.~value_type();
        return block_ptr;
      }
      // one of the readers is holding us back and keeping the refcount up
      // so we ignore this block and find another
      else {
        nonpod_storage_block<value_type> *new_block;
        // repeatedly try to allocate a new block
        // the allocation might fail in the extremely rare case that there
        // are a ton of readers each one locking a block, so we just wait until
        // one of them finishes
        // some other solutions include:
        // 1. using the waiting strategy to wait
        // 2. augmenting the bitmap allocator so that it would allocate extra
        //    space when it is full so that it cannot fail
        while ((new_block = std::allocator_traits<allocator>::allocate(
                    m_allocator, 1)) == nullptr) {
          std::this_thread::yield();
        }

        return new_block;
      }
    }
  }

  nonpod_storage_block<value_type> *
  reconstruct(nonpod_storage_block<value_type> *block,
              const value_type &value) {
    auto cursor = m_internal_queue.cursor();

    // we don't construct since we want to keep the value inside the block
    // uninititalized
    // std::allocator_traits<allocator>::construct(m_allocator, block);

    // constructors may do all sorts of crazy things!
    ::new (&block->value) value_type{value};

    block->sequence_number.store(cursor.sequence_number + 2,
                                 std::memory_order_relaxed);

    // with the reference counter set to a positive value, this block is
    // considered valid and up to speed
    block->refcount.store(1, std::memory_order_release);

    return block;
  }

  void destroy_and_deallocate_block(nonpod_storage_block<value_type> *block) {
    std::allocator_traits<allocator>::destroy(m_allocator, block);
    std::allocator_traits<allocator>::deallocate(m_allocator, block, 1);
  }

private:
  queue_data<nonpod_storage_block<value_type> *, waiting_strategy>
      m_internal_queue;

  allocator_storage m_storage;
  allocator m_allocator;
};

} // namespace details

#if __linux__
using default_waiting_strategy = futex_waiting_strategy;
#else
using default_waiting_strategy = condition_variable_waiting_strategy;
#endif

template <typename T, typename WaitingStrategy = default_waiting_strategy>
class receiver {
  using queue_data = details::queue_data<T, WaitingStrategy>;

public:
  receiver(std::shared_ptr<queue_data> internal_ = nullptr)
      : m_internal{internal_} {

    if (!m_internal)
      return;

    m_internal->subscribe();
    m_cursor = m_internal->cursor();

    if (m_cursor.sequence_number & 1)
      m_cursor.sequence_number += 1;
    else
      m_cursor.sequence_number += 2;
  }

  ~receiver() {
    if (m_internal)
      m_internal->unsubscribe();
  }

  template <typename Rep, typename Period>
  Error wait_dequeue_timed(T *result,
                           const std::chrono::duration<Rep, Period> &timeout) {

    if (!m_internal)
      return Error::Closed;

    return m_internal->read(result, &m_cursor, timeout);
  }

  Error try_dequeue(T *result) {
    return wait_dequeue_timed(result, std::chrono::seconds(0));
  }

  void reset() { m_internal.reset(); }

private:
  std::shared_ptr<queue_data> m_internal;
  details::Cursor m_cursor;
};

template <typename T, typename WaitingStrategy = default_waiting_strategy>
class sender {
  static_assert(std::is_copy_assignable<T>::value,
                "Queue type has to be copy assignable!");

  using queue_data = details::queue_data<T, WaitingStrategy>;

public:
  sender(size_t capacity)
      : m_internal{std::make_shared<queue_data>(capacity)} {}

  sender(sender &&other) : m_internal{std::move(other.m_internal)} {}

  void push(const T &value) { m_internal->push(value); }

  receiver<T, WaitingStrategy> subscribe() {
    return receiver<T, WaitingStrategy>(m_internal);
  }

  ~sender() {
    if (m_internal)
      m_internal->close();
  }

private:
  std::shared_ptr<queue_data> m_internal;
};

} // namespace broadcast_queue

#endif // THEARTFUL_BROADCAST_QUEUE
