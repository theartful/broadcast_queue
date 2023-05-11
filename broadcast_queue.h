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

namespace broadcast_queue {

enum class Error {
  None,
  Timeout,
  Lagged,
  Closed,
};

namespace details {

struct alignas(uint64_t) Cursor {
  uint32_t m_pos;             // the position the writer will write on next
  uint32_t m_sequence_number; // the sequence number of the element on which the
                              // writer will write on next
};

template <typename T> class queue_data {
  static_assert(std::is_trivially_copyable<T>::value,
                "Type T of broadcast_queue has to be trivially copyable!");

  static_assert(std::is_trivially_destructible<T>::value,
                "Type T of broadcast_queue has to be trivially destructible!");

  using storage_type = typename std::conditional<
      sizeof(T) % 8 == 0, uint64_t,
      typename std::conditional<
          sizeof(T) % 4 == 0, uint32_t,
          typename std::conditional<sizeof(T) % 2 == 0, uint16_t,
                                    uint8_t>::type>::type>::type;

  static_assert(sizeof(T) % sizeof(storage_type) == 0,
                "storage_type has to have size multiple of the size of T");

  static constexpr size_t storage_per_element =
      sizeof(T) / sizeof(storage_type);

public:
  using value_type = T;

  queue_data(size_t capacity_) : m_capacity{capacity_}, m_cursor{Cursor{0, 0}} {
    // uninititalized storage
    m_storage = new std::atomic<storage_type>[m_capacity * storage_per_element];

    // zero inititalize sequence numbers
    m_sequence_numbers = new std::atomic<uint32_t>[m_capacity];
    for (size_t i = 0; i < m_capacity; i++)
      m_sequence_numbers[i].store(0, std::memory_order_relaxed);
  }

  void push(const T &value) {
    Cursor cur = m_cursor.load(std::memory_order_relaxed);
    uint32_t pos = cur.m_pos;
    size_t storage_pos = pos * storage_per_element;

    size_t sequence_number =
        m_sequence_numbers[pos].load(std::memory_order_relaxed);

    m_sequence_numbers[pos].store(sequence_number + 1,
                                  std::memory_order_release);

    cur.m_sequence_number = sequence_number + 1;
    m_cursor.store(cur, std::memory_order_release);

    const storage_type *value_as_storage =
        reinterpret_cast<const storage_type *>(&value);

    // enforce a happens-before relationship
    // the change in the sequence number has to happen before all the writes
    // in the data
    std::atomic_thread_fence(std::memory_order_release);
    for (size_t i = 0; i < storage_per_element; i++) {
      m_storage[storage_pos++].store(*(value_as_storage++),
                                     std::memory_order_relaxed);
    }

    m_sequence_numbers[pos].store(sequence_number + 2,
                                  std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(cv_mutex);
      cur.m_pos = (pos + 1) % m_capacity;
      cur.m_sequence_number =
          m_sequence_numbers[cur.m_pos].load(std::memory_order_relaxed);
      m_cursor.store(cur, std::memory_order_relaxed);
    }

    cv.notify_all();
  }

  template <typename Rep, typename Period>
  Error read(T *result, uint32_t *reader_pos, uint32_t *reader_sequence_number,
             const std::chrono::duration<Rep, Period> &timeout) {

    size_t storage_pos = *reader_pos * storage_per_element;
    storage_type *result_as_storage = reinterpret_cast<storage_type *>(result);

    std::chrono::steady_clock::time_point until =
        std::chrono::steady_clock::now() + timeout;

    // first wait until sequence number is not the same as reader sequence
    // number
    if (!wait_for_new_data(until, *reader_pos, *reader_sequence_number))
      return Error::Timeout;

    // we assume that the request timed-out by default
    Error error = Error::Timeout;

    size_t sequence_number_after;
    do {
      size_t sequence_number_before =
          m_sequence_numbers[*reader_pos].load(std::memory_order_acquire);

      // if the writer is in the middle of writing a new value
      if (sequence_number_before & 1) {
        std::this_thread::yield();
        continue;
      }

      for (size_t i = 0; i < storage_per_element; i++) {
        result_as_storage[i] =
            m_storage[storage_pos + i].load(std::memory_order_relaxed);
      }

      // synchronizes with the thread fence in push
      // now we're sure that everything that happened before the store
      // operations in push is seen after this fence
      // this means that if the sequence number after is the same as the
      // sequence number before, then we're sure that we read the data
      // without any data races, since otherwise, it would mean that the
      // writer modified the data, which necessarily means that the writer
      // has changed the sequence number before writing, and we would have
      // necessarily seen this thanks to the fence!
      std::atomic_thread_fence(std::memory_order_acquire);

      sequence_number_after =
          m_sequence_numbers[*reader_pos].load(std::memory_order_acquire);

      if (sequence_number_after == sequence_number_before) {
        error = Error::None;
        break;
      }

    } while (std::chrono::steady_clock::now() < until);

    if (error != Error::Timeout) {
      if (sequence_number_after != *reader_sequence_number) {
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
          *reader_sequence_number = sequence_number_after + 2;
        } else {
          *reader_sequence_number = sequence_number_after;
        }
      }
    }

    return error;
  }

  template <typename Rep, typename Period>
  Error read(T *result, Cursor *cursor,
             const std::chrono::duration<Rep, Period> &timeout) {
    return read(result, &cursor->m_pos, &cursor->m_sequence_number, timeout);
  }

  Cursor cursor() { return m_cursor.load(std::memory_order_relaxed); }
  size_t capacity() { return m_capacity; }
  size_t sequence_number(size_t pos) {
    return m_sequence_numbers[pos].load(std::memory_order_relaxed);
  }

  ~queue_data() {
    delete[] m_storage;
    delete[] m_sequence_numbers;
  }

private:
  bool wait_for_new_data(const std::chrono::steady_clock::time_point &until,
                         uint32_t pos, uint32_t sn0) {
    size_t sn = sequence_number(pos);

    size_t old_sn = sn0 - 2;

    // this means that we're at the tip of the queue, so we just have to
    // wait until m_cursor is updated
    if (sn == old_sn) {
      std::unique_lock<std::mutex> lock{cv_mutex};
      cv.wait_until(lock, until, [this, pos, old_sn]() {
        // the condition variable is on m_cursor not on the sequence numbers,
        // but if the cursor has gone over `pos` then it has to have updated
        // the sequence number before changing the cursor value
        return sequence_number(pos) != old_sn;
      });
    }
    return m_sequence_numbers[pos].load(std::memory_order_relaxed) != old_sn;
  }

private:
  size_t m_capacity;
  std::atomic<Cursor> m_cursor;
  std::atomic<storage_type> *m_storage;
  std::atomic<uint32_t> *m_sequence_numbers;

  // for waiting
  std::mutex cv_mutex;
  std::condition_variable cv;
};

} // namespace details

template <typename T> class receiver {
public:
  receiver(std::shared_ptr<details::queue_data<T>> internal_ = nullptr)
      : m_internal{internal_} {

    if (!internal_)
      return;

    m_cursor = internal_->cursor();

    if (m_cursor.m_sequence_number & 1)
      m_cursor.m_sequence_number += 1;
    else
      m_cursor.m_sequence_number += 2;
  }

  template <typename Rep, typename Period>
  Error wait_dequeue_timed(T *result,
                           const std::chrono::duration<Rep, Period> &timeout) {

    std::shared_ptr<details::queue_data<T>> internal_sptr = m_internal.lock();

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
  std::weak_ptr<details::queue_data<T>> m_internal;
  details::Cursor m_cursor;
};

template <typename T> class sender {
public:
  sender(size_t capacity)
      : internal{std::make_shared<details::queue_data<T>>(capacity)} {}

  sender(sender &&other) : internal{std::move(other.internal)} {}

  void push(const T &value) { internal->push(value); }

  receiver<T> subscribe() { return receiver<T>(internal); }

private:
  std::shared_ptr<details::queue_data<T>> internal;
};

} // namespace broadcast_queue

#endif // THEARTFUL_BROADCAST_QUEUE
