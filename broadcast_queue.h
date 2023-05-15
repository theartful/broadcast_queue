#ifndef THEARTFUL_BROADCAST_QUEUE
#define THEARTFUL_BROADCAST_QUEUE

#include <atomic>             // for atomic data types
#include <chrono>             // for time
#include <condition_variable> // for condition variables obviously
#include <cstdint>            // for int types
#include <memory>             // for smart pointers
#include <mutex>              // for mutexes obviously
#include <thread>             // for yielding the thread
#include <type_traits>        // for all sorts of type operations

// implements a fixed-size single producer multiple consumer fan-out circular
// queue of POD structs where new data is sent to all consumers.
//
// see: "Can Seqlocks Get Along With Programming Language Memory Models?" by
// Hans Bohem (https://www.hpl.hp.com/techreports/2012/HPL-2012-68.pdf)

#if defined(__amd64__) || defined(__x86_64__) || defined(_M_AMD64) ||          \
    defined(_M_X64) || defined(_M_IX86)
#define _BROADCAST_QUEUE_TSO_MODEL
#endif

#ifndef BROADCAST_QUEUE_CACHE_LINE_SIZE
#ifdef __cpp_lib_hardware_interference_size
#include <new>
#define BROADCAST_QUEUE_CACHE_LINE_SIZE                                        \
  std::hardware_destructive_interference_size
#else
#define BROADCAST_QUEUE_CACHE_LINE_SIZE 64
#endif
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

template <typename T> static constexpr bool is_always_lock_free() {
#ifdef __cpp_lib_atomic_is_always_lock_free
  return std::atomic<T>::is_always_lock_free;
#else
  return sizeof(T) < 8 && std::is_trivially_destructible<T>::value &&
         std::is_trivially_copyable<T>::value;
#endif
}

template <typename T, typename = void> class storage_block;

template <typename T, typename WaitingStrategy> class queue_data;

template <typename T>
class storage_block<T, typename std::enable_if<is_always_lock_free<
                           value_with_sequence_number<T>>()>::type> {
public:
  storage_block() { m_storage.store({0, 0}, std::memory_order_relaxed); }

  void store(const T &value) {
    auto old_storage = m_storage.load(std::memory_order_relaxed);
    uint32_t old_sn = old_storage.sequence_number;
    value_with_sequence_number<T> new_storage = {old_sn + 2, value};
    m_storage.store(new_storage, std::memory_order_relaxed);
  }

  bool try_load(T *value, uint32_t *read_sequence_number,
                const std::chrono::steady_clock::time_point /* until */) {
    auto old_storage = m_storage.load(std::memory_order_relaxed);
    *value = old_storage.value;
    *read_sequence_number = old_storage.sequence_number;

    return true;
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
};

template <typename T>
class storage_block<T, typename std::enable_if<!is_always_lock_free<
                           value_with_sequence_number<T>>()>::type> {

public:
  storage_block() { m_sequence_number.store(0, std::memory_order_relaxed); }

  void store(const T &value) {
    uint32_t sn = m_sequence_number.load(std::memory_order_relaxed);

    m_sequence_number.store(sn + 1, std::memory_order_release);

    // enforce a happens-before relationship
    // that is: we're sure that the sequence number is incremented before
    // writing the data
    std::atomic_thread_fence(std::memory_order_release);

#ifdef _BROADCAST_QUEUE_TSO_MODEL
    // in TSO architectures (such as x86), we can use normal store operations
    // since store-store opreations will always be ordered the same in memory
    // as in the program
    m_storage = value;
#else
    const storage_type *value_as_storage =
        reinterpret_cast<const storage_type *>(&value);

    for (size_t i = 0; i < storage_per_element; i++) {
      storage[i].store(*(value_as_storage++), std::memory_order_relaxed);
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

#ifdef _BROADCAST_QUEUE_TSO_MODEL
      // in TSO architectures (such as x86), we can use normal load operations
      // since load-load opreations will always be ordered the same in memory
      // as in the program
      *value = m_storage;
#else
      storage_type *result_as_storage = reinterpret_cast<storage_type *>(value);
      for (size_t i = 0; i < storage_per_element; i++) {
        result_as_storage[i] = storage[i].load(std::memory_order_relaxed);
      }
#endif

      // synchronizes with the thread fence in push
      // now we're sure that everything that happened before the store
      // operations in push is seen after this fence
      // this means that if the sequence_number_after is the same as the
      // sequence_number_before, then we're sure that we read the data
      // without any data races, since otherwise, it would mean that the
      // writer modified the data, which necessarily means that the writer
      // has changed the sequence number before writing, and we would have
      // necessarily seen this thanks to the fence!
      std::atomic_thread_fence(std::memory_order_acquire);

      uint32_t sequence_number_after =
          m_sequence_number.load(std::memory_order_acquire);

      if (sequence_number_after == sequence_number_before) {
        *read_sequence_number = sequence_number_after;
        return true;
      }

    } while (std::chrono::steady_clock::now() < until);

    return false;
  }

  uint32_t
  sequence_number(std::memory_order order = std::memory_order_relaxed) const {
    return m_sequence_number.load(order);
  }

  const void *sequence_number_address() const { return &m_sequence_number; }

private:
#ifdef _BROADCAST_QUEUE_TSO_MODEL
  T m_storage;
#else
  using storage_type = char;
  static constexpr size_t storage_per_element =
      sizeof(T) / sizeof(storage_type);
  static_assert(sizeof(T) % sizeof(storage_type) == 0,
                "storage_type has to have size multiple of the size of T");
  std::atomic<storage_type> storage[storage_per_element];
#endif

  std::atomic<uint32_t> m_sequence_number;
};

struct alignas(uint64_t) Cursor {
  uint32_t m_pos;             // the position the writer will write on next
  uint32_t m_sequence_number; // the sequence number of the element on which the
                              // writer will write on next
};

template <typename T, typename WaitingStrategy> class queue_data {

  friend WaitingStrategy;

  static_assert(std::is_trivially_copyable<T>::value,
                "Type T of broadcast_queue has to be trivially copyable!");

  static_assert(std::is_trivially_destructible<T>::value,
                "Type T of broadcast_queue has to be trivially destructible!");

public:
  using value_type = T;

  queue_data(size_t capacity_)
      : m_capacity{capacity_}, m_subscribers{0}, m_cursor{Cursor{0, 0}},
        m_waiter{this} {

    // uninititalized storage
    m_storage_blocks = new storage_block<T>[m_capacity];
  }

  void push(const T &value) {
    Cursor cur = m_cursor.load(std::memory_order_relaxed);
    uint32_t pos = cur.m_pos;

    auto &block = m_storage_blocks[pos];

    uint32_t sequence_number = block.sequence_number(std::memory_order_relaxed);

    // update cursor indicating we're in the middle of writing
    cur.m_sequence_number = sequence_number + 1;
    m_cursor.store(cur, std::memory_order_relaxed);

    block.store(value);

    // update cursor to the next element
    cur.m_pos = (pos + 1) % m_capacity;
    cur.m_sequence_number =
        m_storage_blocks[cur.m_pos].sequence_number(std::memory_order_relaxed);
    m_cursor.store(cur, std::memory_order_relaxed);

    m_waiter.notify(pos, sequence_number + 2);
  }

  template <typename Rep, typename Period>
  Error read(T *result, uint32_t *reader_pos, uint32_t *reader_sequence_number,
             const std::chrono::duration<Rep, Period> &timeout) {

    std::chrono::steady_clock::time_point until =
        std::chrono::steady_clock::now() + timeout;

    // first wait until sequence number is not the same as reader sequence
    // number
    if (!m_waiter.wait(*reader_pos, *reader_sequence_number, timeout))
      return Error::Timeout;

    auto &block = m_storage_blocks[*reader_pos];

    uint32_t sequence_number;

    if (block.try_load(result, &sequence_number, until)) {
      if (sequence_number != *reader_sequence_number) {
        Cursor cur = m_cursor.load(std::memory_order_relaxed);

        // lagging will effectively cause resubscription
        *reader_pos = cur.m_pos;
        *reader_sequence_number = cur.m_sequence_number;
        if (*reader_sequence_number & 1)
          *reader_sequence_number += 1;
        else
          *reader_sequence_number += 2;

        // TODO: make it optional between resubscription and resetting to the
        // oldest data
        // the problem with resetting to the oldest data is in the case of a
        // fast writer, the oldest data will be written on, and it would cause
        // the reader to lag again

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
  Error read(T *result, Cursor *cursor,
             const std::chrono::duration<Rep, Period> &timeout) {
    return read(result, &cursor->m_pos, &cursor->m_sequence_number, timeout);
  }

  Cursor cursor() { return m_cursor.load(std::memory_order_relaxed); }
  size_t capacity() { return m_capacity; }

  uint32_t
  sequence_number(uint32_t pos,
                  std::memory_order order = std::memory_order_relaxed) {
    return m_storage_blocks[pos].sequence_number(order);
  }

  const storage_block<T> &block(uint32_t pos) { return m_storage_blocks[pos]; }

  size_t subscribers() { return m_subscribers.load(std::memory_order_relaxed); }
  void subscribe() { m_subscribers.fetch_add(1, std::memory_order_relaxed); }
  void unsubscribe() { m_subscribers.fetch_sub(1, std::memory_order_relaxed); }

  ~queue_data() { delete[] m_storage_blocks; }

private:
  size_t m_capacity;
  std::atomic<size_t> m_subscribers;
  storage_block<T> *m_storage_blocks;
  WaitingStrategy m_waiter;

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

} // namespace details
  //
template <typename T> class condition_variable_waiting_strategy {
  using self = condition_variable_waiting_strategy<T>;

public:
  condition_variable_waiting_strategy(details::queue_data<T, self> *queue)
      : m_queue{queue} {}

  void notify(uint32_t pos, uint32_t sequence_number) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cv.notify_all();
  }

  template <typename Rep, typename Period>
  bool wait(uint32_t reader_pos, uint32_t reader_sequence_number,
            const std::chrono::duration<Rep, Period> &timeout) {

    uint32_t old_sequence_number = reader_sequence_number - 2;

    auto &block = m_queue->block(reader_pos);
    // this means that we're at the tip of the queue, so we just have to
    // wait until m_cursor is updated
    if (block.sequence_number() == old_sequence_number) {
      std::unique_lock<std::mutex> lock{m_mutex};
      return m_cv.wait_for(
          lock, timeout, [&block, reader_pos, old_sequence_number]() {
            // the condition variable is on m_cursor not on the sequence
            // numbers, but if the cursor has gone over `pos` then it has to
            // have updated the sequence number before changing the cursor value
            return block.sequence_number() != old_sequence_number;
          });
    }
    return true;
  }

private:
  details::queue_data<T, self> *m_queue;
  std::mutex m_mutex;
  std::condition_variable m_cv;
};

template <typename T>
using default_waiting_strategy = condition_variable_waiting_strategy<T>;

template <typename T, typename WaitingStrategy = default_waiting_strategy<T>>
class receiver {
  using queue_data = details::queue_data<T, WaitingStrategy>;

public:
  receiver(std::shared_ptr<queue_data> internal_ = nullptr)
      : m_internal{internal_} {

    if (!internal_)
      return;

    internal_->subscribe();
    m_cursor = internal_->cursor();

    if (m_cursor.m_sequence_number & 1)
      m_cursor.m_sequence_number += 1;
    else
      m_cursor.m_sequence_number += 2;
  }

  ~receiver() {
    std::shared_ptr<queue_data> internal_sptr = m_internal.lock();
    if (internal_sptr)
      internal_sptr->unsubscribe();
  }

  template <typename Rep, typename Period>
  Error wait_dequeue_timed(T *result,
                           const std::chrono::duration<Rep, Period> &timeout) {

    std::shared_ptr<queue_data> internal_sptr = m_internal.lock();

    if (!internal_sptr) {
      return Error::Closed;
    }

    return internal_sptr->read(result, &m_cursor, timeout);
  }

  Error try_dequeue(T *result) {
    return wait_dequeue_timed(result, std::chrono::seconds(0));
  }

  void reset() { m_internal.reset(); }

private:
  std::weak_ptr<queue_data> m_internal;
  details::Cursor m_cursor;
};

template <typename T, typename WaitingStrategy = default_waiting_strategy<T>>
class sender {
  using queue_data = details::queue_data<T, WaitingStrategy>;

public:
  sender(size_t capacity) : internal{std::make_shared<queue_data>(capacity)} {}

  sender(sender &&other) : internal{std::move(other.internal)} {}

  void push(const T &value) { internal->push(value); }

  receiver<T, WaitingStrategy> subscribe() {
    return receiver<T, WaitingStrategy>(internal);
  }

private:
  std::shared_ptr<queue_data> internal;
};

} // namespace broadcast_queue

#endif // THEARTFUL_BROADCAST_QUEUE
