#ifndef THEARTFUL_BROADCAST_QUEUE_SEMAPHORE_WAITER
#define THEARTFUL_BROADCAST_QUEUE_SEMAPHORE_WAITER

#include <atomic>
#include <climits>
#include <cstdlib>
#include <ctime>
#include <system_error>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <semaphore.h>
#include <time.h>
#elif defined(_WIN32) || defined(__MSYS__)
#include <windows.h>
#endif

#include "broadcast_queue.h"

namespace broadcast_queue {

#if defined(__linux__)
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
#elif defined(_WIN32) || defined(__MSYS__)
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

class semaphore_waiting_strategy {
public:
  semaphore_waiting_strategy() : m_semaphore{0}, m_waiters{0} {}

  ~semaphore_waiting_strategy() {}

  void notify(std::atomic<uint32_t> &sequence_number) {
    // getting the value is ok even though it might change because if it changes
    // it would mean that a reader decremented it and read the new value that
    // we're notifying on, and it's okay if we overcount him
    int semaphore_value = m_semaphore.value();

    // the number of waiters isn't accurate because:
    // 1. it might decrease from the point we loaded it here, until the point
    //    we use it. this is ok since this would mean we overcounted a waiter,
    //    and it this won't cause problems
    // 2. it might increase from point we loaded it here, until the point we use
    //    it, but this is ok, since the reader will check again on the sequence
    //    number after he has increased the number of waiters, and in that case
    //    he would find that the sequence number has changed and wouldn't need
    //    to wait
    auto waiters = m_waiters.load(std::memory_order_relaxed);

    if (waiters > semaphore_value)
      m_semaphore.release(waiters - semaphore_value);
  }

  template <typename Rep, typename Period>
  bool wait(std::atomic<uint32_t> &sequence_number,
            uint32_t old_sequence_number,
            const std::chrono::duration<Rep, Period> &timeout) {

    m_waiters.fetch_add(1, std::memory_order_acq_rel);

    // we have to check again on the sequence number after we've incremented
    // m_waiters because the writer might miss our increment if he was already
    // in the notify method
    // but if he's in the notify method, then the sequence_number has changed
    if (sequence_number.load(std::memory_order_relaxed) !=
        old_sequence_number) {
      m_waiters.fetch_sub(1, std::memory_order_relaxed);
      return true;
    }

    auto until = std::chrono::steady_clock::now() + timeout;
    do {
      if (m_semaphore.try_acquire_for(timeout)) {
        if (sequence_number.load(std::memory_order_relaxed) !=
            old_sequence_number) {
          m_waiters.fetch_sub(1, std::memory_order_relaxed);
          return true;
        }
      }
    } while (std::chrono::steady_clock::now() < until);

    // timed out
    m_waiters.fetch_sub(1, std::memory_order_relaxed);
    return false;
  }

private:
  semaphore m_semaphore;
  std::atomic<uint64_t> m_waiters;
};

} // namespace broadcast_queue

#endif // THEARTFUL_BROADCAST_QUEUE_SEMAPHORE_WAITER
