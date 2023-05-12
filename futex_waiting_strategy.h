#ifndef THEARTFUL_BROADCAST_QUEUE_FUTEX_WAITER
#define THEARTFUL_BROADCAST_QUEUE_FUTEX_WAITER
#if __unix__

#include <chrono>
#include <climits>
#include <cerrno>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <system_error>
#include <unistd.h>

#include "broadcast_queue.h"

namespace broadcast_queue {

template <typename T> class futex_waiting_strategy {
  using self = futex_waiting_strategy<T>;

public:
  futex_waiting_strategy(details::queue_data<T, self> *queue)
      : m_queue{queue} {}

  void notify(uint32_t pos, uint32_t sequence_number) {
    long result =
        ::syscall(SYS_futex,
                  static_cast<const void *>(
                      &m_queue->m_storage_blocks[pos].sequence_number),
                  FUTEX_WAKE, INT_MAX, NULL, NULL, 0);

    if (result == -1)
      throw std::system_error(errno, std::system_category());
  }

  template <typename Rep, typename Period>
  bool wait(uint32_t reader_pos, uint32_t reader_sequence_number,
            const std::chrono::duration<Rep, Period> &timeout) {

    uint32_t old_sequence_number = reader_sequence_number - 2;

    if (timeout.count() == 0)
      return m_queue->sequence_number(reader_pos) != old_sequence_number;

    auto until = std::chrono::steady_clock::now() + timeout;

    struct timespec timeout_spec;
    timeout_spec.tv_sec =
        std::chrono::duration_cast<std::chrono::seconds>(timeout).count();
    timeout_spec.tv_nsec =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            timeout - std::chrono::seconds(timeout_spec.tv_sec))
            .count();

    do {
      if (m_queue->sequence_number(reader_pos) != old_sequence_number)
        break;

      long result =
          ::syscall(SYS_futex,
                    static_cast<const void *>(
                        &m_queue->m_storage_blocks[reader_pos].sequence_number),
                    FUTEX_WAIT, reader_sequence_number, &timeout_spec, NULL, 0);

      if (result == -1) {
        switch (errno) {
        case EAGAIN:
          // the value of the sequence number has changed
          return true;
        case ETIMEDOUT:
          // timeout
          return false;
        case EINTR:
          // we got interrupted
          continue;
        default:
          throw std::system_error(errno, std::system_category());
        }
      }
    } while (std::chrono::steady_clock::now() < until);

    return m_queue->sequence_number(reader_pos) != old_sequence_number;
  }

private:
  details::queue_data<T, self> *m_queue;
};

} // namespace broadcast_queue

#endif // __unix__
#endif // THEARTFUL_BROADCAST_QUEUE_FUTEX_WAITER
