#ifndef THEARTFUL_BROADCAST_QUEUE_CONDVAR_WAITER
#define THEARTFUL_BROADCAST_QUEUE_CONDVAR_WAITER

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace broadcast_queue {

class condition_variable_waiting_strategy {
public:
  condition_variable_waiting_strategy() {}

  template <typename T> void notify(const std::atomic<T> &) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cv.notify_all();
  }

  template <typename T, typename Rep, typename Period>
  bool wait(std::atomic<T> &sequence_number, T old_sequence_number,
            const std::chrono::duration<Rep, Period> &timeout) {
    if (sequence_number.load(std::memory_order_relaxed) ==
        old_sequence_number) {
      std::unique_lock<std::mutex> lock{m_mutex};
      return m_cv.wait_for(lock, timeout, [&]() {
        return sequence_number.load(std::memory_order_relaxed) !=
               old_sequence_number;
      });
    }
    return true;
  }

private:
  std::mutex m_mutex;
  std::condition_variable m_cv;
};

} // namespace broadcast_queue

#endif // THEARTFUL_BROADCAST_QUEUE_CONDVAR_WAITER
