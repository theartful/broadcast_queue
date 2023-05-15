#ifndef THEARTFUL_BROADCAST_QUEUE_SEMAPHORE_WAITER
#define THEARTFUL_BROADCAST_QUEUE_SEMAPHORE_WAITER

#include <cstdlib>
#include <ctime>
#include <system_error>
#include <thread>
#include <vector>
#if __unix__
#include <semaphore.h>
#include <time.h>
#elif _WIN32
#include <windows.h>
#endif

#include "broadcast_queue.h"

namespace broadcast_queue {

#if __unix__
class semaphore {
public:
  semaphore(int initial_value = 0) { sem_init(&m_semaphore, 0, initial_value); }

  ~semaphore() { sem_destroy(&m_semaphore); }

  void release(int val = 1) {
    for (int i = 0; i < val; i++) {
      sem_post(&m_semaphore);
    }
  }

  void acquire() {
    if (sem_wait(&m_semaphore) == 0) {
      return;
    } else {
      throw std::system_error(errno, std::system_category());
    }
  }

  bool try_acquire() {
    if (sem_trywait(&m_semaphore) == 0) {
      return true;
    } else {
      if (errno == EAGAIN || errno == EINTR) {
        return false;
      } else {
        throw std::system_error(errno, std::system_category());
      }
    }
  }

  template <typename Rep, typename Period>
  bool try_acquire_for(const std::chrono::duration<Rep, Period> &timeout) {
    struct timespec timeout_spec;
    clock_gettime(CLOCK_MONOTONIC, &timeout_spec);

    auto secs = std::chrono::duration_cast<std::chrono::seconds>(timeout);

    timeout_spec.tv_sec += secs.count();
    timeout_spec.tv_nsec +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(timeout - secs)
            .count();

    // make sure that tv_nsec is less than 1e9
    if (timeout_spec.tv_nsec > 1000000000) {
      timeout_spec.tv_sec++;
      timeout_spec.tv_nsec -= 1000000000;
    }

    if (sem_clockwait(&m_semaphore, CLOCK_MONOTONIC, &timeout_spec) == 0) {
      return true;
    } else {
      if (errno == ETIMEDOUT || errno == EINTR) {
        return false;
      } else {
        throw std::system_error(errno, std::system_category());
      }
    }
  }

  template <typename Clock, typename Duration>
  bool
  try_acquire_until(const std::chrono::time_point<Clock, Duration> &until) {
    auto now = Clock::now();
    if (now > until)
      return false;
    return try_acquire_for(until - now);
  }

  int value() {
    int value;
    if (sem_getvalue(&m_semaphore, &value) == 0) {
      return value;
    } else {
      throw std::system_error(errno, std::system_category());
    }
  }

private:
  sem_t m_semaphore;
};
#elif defined(_WIN32)
class semaphore {
public:
  semaphore(int initial_value = 0) {
    m_semaphore = CreateSemaphore(NULL, initial_value, INT_MAX, NULL);
    m_semaphore_value = 0;
  }

  ~semaphore() { CloseHandle(m_semaphore); }

  void release(int val = 1) {
    ReleaseSemaphore(m_semaphore, val, NULL);
    m_semaphore_value.fetch_add(val, std::memory_order_relaxed);
  }

  void acquire() {
    switch (WaitForSingleObject(m_semaphore, INFINITE)) {
    case WAIT_OBJECT_0:
      m_semaphore_value.fetch_sub(1, std::memory_order_relaxed);
      return;
    case WAIT_ABANDONED:
    case WAIT_TIMEOUT:
    case WAIT_FAILED:
    default:
      return;
    }
  }

  bool try_acquire() {
    switch (WaitForSingleObject(m_semaphore, 0)) {
    case WAIT_OBJECT_0:
      m_semaphore_value.fetch_sub(1, std::memory_order_relaxed);
      return true;
    case WAIT_ABANDONED:
    case WAIT_TIMEOUT:
    case WAIT_FAILED:
    default:
      return false;
    }
  }

  template <typename Rep, typename Period>
  bool try_acquire_for(const std::chrono::duration<Rep, Period> &timeout) {
    DWORD millis = static_cast<DWORD>(
        std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count());

    switch (WaitForSingleObject(m_semaphore, millis)) {
    case WAIT_OBJECT_0:
      m_semaphore_value.fetch_sub(1, std::memory_order_relaxed);
      return true;
    case WAIT_ABANDONED:
    case WAIT_TIMEOUT:
    case WAIT_FAILED:
    default:
      return false;
    }
  }

  template <typename Clock, typename Duration>
  bool
  try_acquire_until(const std::chrono::time_point<Clock, Duration> &until) {
    auto now = Clock::now();
    if (now > until)
      return false;
    return try_acquire_for(until - now);
  }

  int value() { return m_semaphore_value.load(std::memory_order_relaxed); }

private:
  HANDLE m_semaphore;
  std::atomic<uint32_t> m_semaphore_value;
};
#elif defined(__MACH__)
// TODO
#endif

template <typename T> class semaphore_waiting_strategy {
  using self = semaphore_waiting_strategy<T>;

public:
  semaphore_waiting_strategy(details::queue_data<T, self> *queue)
      : m_queue{queue} {
    m_semaphores = new semaphore[m_queue->capacity()]();
  }

  ~semaphore_waiting_strategy() { delete[] m_semaphores; }

  void notify(uint32_t pos, uint32_t sequence_number) {
    int subscribers = static_cast<int>(m_queue->subscribers());

    // getting the value is ok even though it might change because if it changes
    // it would mean that a reader decremented it and read the new value that
    // we're notifying on, and it's okay if we overcount him
    int semaphore_value = m_semaphores[pos].value();
    if (subscribers > semaphore_value)
      m_semaphores[pos].release(subscribers - semaphore_value);
  }

  template <typename Rep, typename Period>
  bool wait(uint32_t reader_pos, uint32_t reader_sequence_number,
            const std::chrono::duration<Rep, Period> &timeout) {

    uint32_t old_sequence_number = reader_sequence_number - 2;

    auto &block = m_queue->block(reader_pos);

    if (block.sequence_number() != old_sequence_number)
      return true;

    constexpr int atomic_spin_count = 128;

    for (int i = 0; i < atomic_spin_count; i++) {
      if (block.sequence_number() != old_sequence_number)
        return true;

      std::this_thread::yield();
    }

    auto until = std::chrono::steady_clock::now() + timeout;
    do {
      if (m_semaphores[reader_pos].try_acquire_for(timeout)) {
        if (block.sequence_number() != old_sequence_number) {
          return true;
        }
      }
    } while (std::chrono::steady_clock::now() < until);

    // timed out
    return false;
  }

private:
  details::queue_data<T, self> *m_queue;
  semaphore *m_semaphores;
};

} // namespace broadcast_queue

#endif // THEARTFUL_BROADCAST_QUEUE_SEMAPHORE_WAITER
