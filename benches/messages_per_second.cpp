#include <atomic>
#include <bits/chrono.h>
#include <chrono>
#include <stdio.h>
#include <thread>
#include <vector>

#include "broadcast_queue.h"
#ifdef __unix__
#include "futex_waiting_strategy.h"
#include "semaphore_waiting_strategy.h"
#endif

#include <moodycamel/concurrentqueue.h>

struct BenchResult {
  size_t num_readers;
  size_t written_messages;
  std::vector<size_t> read_messages;
  std::chrono::milliseconds duration;
};

template <template <typename>
          typename WaitingStrategy = broadcast_queue::default_waiting_strategy>
BenchResult run_broadcast_queue_bench(size_t capacity, size_t num_readers,
                                      std::chrono::milliseconds duration) {
  broadcast_queue::sender<uint64_t, WaitingStrategy<uint64_t>> sender(capacity);

  std::vector<broadcast_queue::receiver<uint64_t, WaitingStrategy<uint64_t>>>
      receivers;
  receivers.reserve(num_readers);

  for (size_t i = 0; i < num_readers; i++) {
    receivers.push_back(sender.subscribe());
  }

  std::atomic<bool> should_stop{false};

  size_t written_messages = 0;

  std::thread sender_thread{[&]() {
    while (!should_stop) {
      sender.push(written_messages++);
    }
  }};

  std::vector<size_t> read_messages;
  std::vector<std::thread> reader_threads;

  read_messages.resize(num_readers);

  for (size_t i = 0; i < num_readers; i++) {
    reader_threads.push_back(std::thread{[&, i]() {
      auto &receiver = receivers[i];

      while (!should_stop) {
        broadcast_queue::Error error;
        uint64_t msg;

        error = receiver.try_dequeue(&msg);
        if (error == broadcast_queue::Error::None)
          read_messages[i]++;
      }
    }});
  }

  std::this_thread::sleep_for(duration);
  should_stop = true;

  sender_thread.join();
  for (auto &reader_thread : reader_threads) {
    reader_thread.join();
  }

  return BenchResult{num_readers, written_messages, read_messages, duration};
}

BenchResult run_moody_bench(size_t capacity, size_t num_readers,
                            std::chrono::milliseconds duration) {

  moodycamel::ConcurrentQueue<uint64_t> queue{capacity};

  std::atomic<bool> should_stop{false};

  size_t written_messages = 0;

  std::thread sender_thread{[&]() {
    while (!should_stop) {
      queue.enqueue(written_messages++);
    }
  }};

  std::vector<size_t> read_messages;
  std::vector<std::thread> reader_threads;

  read_messages.resize(num_readers);

  for (size_t i = 0; i < num_readers; i++) {
    reader_threads.push_back(std::thread{[&, i]() {
      while (!should_stop) {
        uint64_t msg;
        bool success = queue.try_dequeue(msg);
        if (success)
          read_messages[i]++;
      }
    }});
  }

  std::this_thread::sleep_for(duration);
  should_stop = true;

  sender_thread.join();
  for (auto &reader_thread : reader_threads) {
    reader_thread.join();
  }

  return BenchResult{num_readers, written_messages, read_messages, duration};
}

void print_results(const BenchResult &results) {
  size_t tot_read_messages = 0;
  for (size_t n : results.read_messages)
    tot_read_messages += n;

  printf("duration: \t\t %li millseconds\n", results.duration.count());
  printf("num_readers: \t\t %zu reader\n", results.num_readers);
  printf("written_msgs: \t\t %zu message/sec\n",
         results.written_messages / (results.duration.count() / 1000));
  printf("avg_read_msgs: \t\t %zu message/sec\n",
         (tot_read_messages / results.num_readers) /
             (results.duration.count() / 1000));
  printf("\n");
}

int main() {
  printf("broadcast_queue<default_waiting_strategy>:\n");
  printf("------------------------------------------\n");
  print_results(run_broadcast_queue_bench(1024, 1, std::chrono::seconds(10)));
  print_results(run_broadcast_queue_bench(1024, 5, std::chrono::seconds(10)));
  print_results(run_broadcast_queue_bench(1024, 10, std::chrono::seconds(10)));
  printf("\n");

#ifdef __unix__
  printf("broadcast_queue<semaphore_waiting_strategy>:\n");
  printf("----------------------------------------\n");
  print_results(
      run_broadcast_queue_bench<broadcast_queue::semaphore_waiting_strategy>(
          1024, 1, std::chrono::seconds(10)));
  print_results(
      run_broadcast_queue_bench<broadcast_queue::semaphore_waiting_strategy>(
          1024, 5, std::chrono::seconds(10)));
  print_results(
      run_broadcast_queue_bench<broadcast_queue::semaphore_waiting_strategy>(
          1024, 10, std::chrono::seconds(10)));
  printf("\n");

  printf("broadcast_queue<futex_waiting_strategy>:\n");
  printf("----------------------------------------\n");
  print_results(
      run_broadcast_queue_bench<broadcast_queue::futex_waiting_strategy>(
          1024, 1, std::chrono::seconds(10)));
  print_results(
      run_broadcast_queue_bench<broadcast_queue::futex_waiting_strategy>(
          1024, 5, std::chrono::seconds(10)));
  print_results(
      run_broadcast_queue_bench<broadcast_queue::futex_waiting_strategy>(
          1024, 10, std::chrono::seconds(10)));
  printf("\n");
#endif

  printf("moodycamel::ConcurrentQueue:\n");
  printf("----------------------------\n");
  print_results(run_moody_bench(1024, 1, std::chrono::seconds(10)));
  print_results(run_moody_bench(1024, 5, std::chrono::seconds(10)));
  print_results(run_moody_bench(1024, 10, std::chrono::seconds(10)));
}
