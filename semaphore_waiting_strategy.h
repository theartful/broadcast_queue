#ifndef THEARTFUL_BROADCAST_QUEUE_SEMAPHORE_WAITER
#define THEARTFUL_BROADCAST_QUEUE_SEMAPHORE_WAITER

#include <cstdlib>
#include <semaphore>
#include <system_error>
#include <vector>
#if __unix__
#include <semaphore.h>
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
    timeout_spec.tv_sec =
        std::chrono::duration_cast<std::chrono::seconds>(timeout).count();
    timeout_spec.tv_nsec =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            timeout - std::chrono::seconds(timeout_spec.tv_sec))
            .count();

    if (sem_timedwait(&m_semaphore, &timeout_spec) == 0) {
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
// TODO
#elif defined(__MACH__)
// TODO
#endif

template <typename T> class semaphore_waiting_strategy {
  using self = semaphore_waiting_strategy<T>;

public:
  semaphore_waiting_strategy(details::queue_data<T, self> *queue)
      : m_queue{queue} {

    m_semaphores = (semaphore *)std::aligned_alloc(
        alignof(semaphore), sizeof(semaphore) * m_queue->capacity());

    for (size_t i = 0; i < m_queue->capacity(); i++) {
      std::construct_at(&m_semaphores[i], 0);
    }
  }

  ~semaphore_waiting_strategy() { std::free(m_semaphores); }

  void notify(uint32_t pos, uint32_t sequence_number) {
    auto subscribers = m_queue->subscribers();

    // getting the value is ok even though it might change because if it changes
    // it would mean that a reader decremented it and read the new value that
    // we're notifying on, and it's okay if we overcount him
    auto semaphore_value = m_semaphores[pos].value();
    if (subscribers > semaphore_value)
      m_semaphores[pos].release(subscribers - semaphore_value);
  }

  template <typename Rep, typename Period>
  bool wait(uint32_t reader_pos, uint32_t reader_sequence_number,
            const std::chrono::duration<Rep, Period> &timeout) {

    uint32_t old_sequence_number = reader_sequence_number - 2;

    if (m_queue->sequence_number(reader_pos) != old_sequence_number)
      return true;

    auto until = std::chrono::steady_clock::now() + timeout;
    do {
      if (m_semaphores[reader_pos].try_acquire_until(until)) {
        if (m_queue->sequence_number(reader_pos) != old_sequence_number) {
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
