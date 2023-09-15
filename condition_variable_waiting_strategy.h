#ifndef THEARTFUL_BROADCAST_QUEUE_CONDVAR_WAITER
#define THEARTFUL_BROADCAST_QUEUE_CONDVAR_WAITER

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace broadcast_queue {

class condition_variable_waiting_strategy {
public:
  condition_variable_waiting_strategy() {}

  void notify(const std::atomic<uint32_t> &) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cv.notify_all();
  }

  template <typename Rep, typename Period>
  bool wait(std::atomic<uint32_t> &sequence_number,
            uint32_t old_sequence_number,
            const std::chrono::duration<Rep, Period> &timeout) {
    // this means that we're at the tip of the queue, so we just have to
    // wait until m_cursor is updated
    if (sequence_number.load(std::memory_order_relaxed) ==
        old_sequence_number) {
      std::unique_lock<std::mutex> lock{m_mutex};
      return m_cv.wait_for(lock, timeout, [&]() {
        // the condition variable is on m_cursor not on the sequence
        // numbers, but if the cursor has gone over `pos` then it has to
        // have updated the sequence number before changing the cursor value
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
