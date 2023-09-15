#ifndef THEARTFUL_BROADCAST_QUEUE_FUTEX_WAITER
#define THEARTFUL_BROADCAST_QUEUE_FUTEX_WAITER
#include <cstdint>
#if __linux__

#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <system_error>
#include <unistd.h>

namespace broadcast_queue {

class futex_waiting_strategy {
public:
  futex_waiting_strategy() : m_waiters{0} {}

  void notify(std::atomic<uint32_t> &sequence_number) {
    // we wake people up only if there are people to wake in the first place!
    if (m_waiters.load(std::memory_order_relaxed) > 0) {
      long result =
          ::syscall(SYS_futex, static_cast<const void *>(&sequence_number),
                    FUTEX_WAKE, INT_MAX, NULL, NULL, 0);

      if (result == -1)
        throw std::system_error(errno, std::system_category());
    }
  }

  template <typename Rep, typename Period>
  bool wait(std::atomic<uint32_t> &sequence_number,
            uint32_t old_sequence_number,
            const std::chrono::duration<Rep, Period> &timeout) {

    if (timeout.count() == 0)
      return sequence_number.load(std::memory_order_relaxed) !=
             old_sequence_number;

    auto until = std::chrono::steady_clock::now() + timeout;

    struct timespec timeout_spec;
    timeout_spec.tv_sec =
        std::chrono::duration_cast<std::chrono::seconds>(timeout).count();
    timeout_spec.tv_nsec =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            timeout - std::chrono::seconds(timeout_spec.tv_sec))
            .count();

    // we register that we're waiting for a new value
    // each reader is responsible for registering and unregistering his interest
    // this however, might cause the writer to make additional FUTEX_WAKE calls
    // because the writer might write the next value before the reader
    // decrements the number of waiters, but I think that's okay for now
    m_waiters.fetch_add(1, std::memory_order_relaxed);

    // in golang, I would have written
    // defer waiters.fetch_sub(1, std::memory_order_relaxed);

    do {
      long result =
          ::syscall(SYS_futex, static_cast<const void *>(&sequence_number),
                    FUTEX_WAIT, old_sequence_number, &timeout_spec, NULL, 0);

      if (result == -1) {
        switch (errno) {
        case EAGAIN:
          // the value of the sequence number has changed
          m_waiters.fetch_sub(1, std::memory_order_acq_rel);
          return true;
        case ETIMEDOUT:
          // timeout
          m_waiters.fetch_sub(1, std::memory_order_acq_rel);
          return false;
        case EINTR:
          // we got interrupted
          continue;
        default:
          m_waiters.fetch_sub(1, std::memory_order_acq_rel);
          throw std::system_error(errno, std::system_category());
        }
      }
    } while (std::chrono::steady_clock::now() < until);

    // we timed out
    m_waiters.fetch_sub(1, std::memory_order_acq_rel);

    return false;
  }

private:
  std::atomic<uint32_t> m_waiters;
};

} // namespace broadcast_queue

#endif // __linux__
#endif // THEARTFUL_BROADCAST_QUEUE_FUTEX_WAITER
