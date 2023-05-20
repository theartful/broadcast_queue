#include <atomic>
#include <chrono>
#include <stdio.h>
#include <thread>
#include <vector>

#include "broadcast_queue.h"
#include "semaphore_waiting_strategy.h"

#ifdef __linux__
#include "futex_waiting_strategy.h"
#endif

#include <concurrentqueue.h>

struct BenchResult {
  size_t num_readers;
  size_t written_messages;
  std::vector<size_t> read_messages;
  std::chrono::milliseconds duration;
};

using namespace broadcast_queue;

// pretty much std::iota
template <typename T> T new_value(int idx) {
  T data;
  char *ptr = (char *)&data;
  int cur = idx;
  char *cur_bytes = (char *)(&cur);
  int cur_bytes_idx = 0;

  for (int i = 0; i < sizeof(data); i++) {
    ptr[i] = cur_bytes[cur_bytes_idx++];

    if (cur_bytes_idx == sizeof(cur)) {
      cur++;
      cur_bytes_idx = 0;
    }
  }
  return data;
}

template <> inline std::string new_value<std::string>(int idx) {
  return std::string("string number: ") + std::to_string(idx);
}
template <typename WaitingStrategy = default_waiting_strategy,
          typename T = uint64_t>
BenchResult run_broadcast_queue_bench(size_t capacity, size_t num_readers,
                                      std::chrono::milliseconds duration) {
  sender<T, WaitingStrategy> sender(capacity);

  std::atomic<bool> should_stop{false};

  size_t written_messages = 0;

  std::thread sender_thread{[&]() {
    while (!should_stop) {
      sender.push(new_value<T>(written_messages++));
    }
  }};

  std::vector<size_t> read_messages;
  std::vector<std::thread> reader_threads;

  read_messages.resize(num_readers);

  for (size_t i = 0; i < num_readers; i++) {
    reader_threads.push_back(std::thread{[&, i]() {
      auto receiver = sender.subscribe();

      while (!should_stop) {
        Error error;
        T msg;

        error = receiver.try_dequeue(&msg);
        if (error == Error::None)
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

template <typename T = uint64_t>
BenchResult run_moody_bench(size_t capacity, size_t num_readers,
                            std::chrono::milliseconds duration) {

  moodycamel::ConcurrentQueue<T> queue{capacity};

  std::atomic<bool> should_stop{false};

  size_t written_messages = 0;

  std::thread sender_thread{[&]() {
    while (!should_stop) {
      queue.enqueue(new_value<T>(written_messages++));
    }
  }};

  std::vector<size_t> read_messages;
  std::vector<std::thread> reader_threads;

  read_messages.resize(num_readers);

  for (size_t i = 0; i < num_readers; i++) {
    reader_threads.push_back(std::thread{[&, i]() {
      while (!should_stop) {
        T msg;
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

  printf("duration: \t\t %zu millseconds\n", results.duration.count());
  printf("num_readers: \t\t %zu reader\n", results.num_readers);
  printf("written_msgs: \t\t %zu message/sec\n",
         results.written_messages / (results.duration.count() / 1000));
  printf("avg_read_msgs: \t\t %zu message/sec\n",
         (tot_read_messages / results.num_readers) /
             (results.duration.count() / 1000));
  printf("\n");
}

int main() {
  printf("broadcast_queue<std::string, default_waiting_strategy>:\n");
  printf("-------------------------------------------------------\n");
  print_results(
      run_broadcast_queue_bench<default_waiting_strategy, std::string>(
          1024, 1, std::chrono::seconds(10)));
  print_results(
      run_broadcast_queue_bench<default_waiting_strategy, std::string>(
          1024, 5, std::chrono::seconds(10)));
  print_results(
      run_broadcast_queue_bench<default_waiting_strategy, std::string>(
          1024, 10, std::chrono::seconds(10)));
  printf("\n");

  printf("broadcast_queue<uint64_t, default_waiting_strategy>:\n");
  printf("----------------------------------------------------\n");
  print_results(run_broadcast_queue_bench(1024, 1, std::chrono::seconds(10)));
  print_results(run_broadcast_queue_bench(1024, 5, std::chrono::seconds(10)));
  print_results(run_broadcast_queue_bench(1024, 10, std::chrono::seconds(10)));
  printf("\n");

  printf("broadcast_queue<std::string, semaphore_waiting_strategy>:\n");
  printf("---------------------------------------------------------\n");
  print_results(
      run_broadcast_queue_bench<semaphore_waiting_strategy, std::string>(
          1024, 1, std::chrono::seconds(10)));
  print_results(
      run_broadcast_queue_bench<semaphore_waiting_strategy, std::string>(
          1024, 5, std::chrono::seconds(10)));
  print_results(
      run_broadcast_queue_bench<semaphore_waiting_strategy, std::string>(
          1024, 10, std::chrono::seconds(10)));
  printf("\n");

  printf("broadcast_queue<uint64_t, semaphore_waiting_strategy>:\n");
  printf("------------------------------------------------------\n");
  print_results(run_broadcast_queue_bench<semaphore_waiting_strategy>(
      1024, 1, std::chrono::seconds(10)));
  print_results(run_broadcast_queue_bench<semaphore_waiting_strategy>(
      1024, 5, std::chrono::seconds(10)));
  print_results(run_broadcast_queue_bench<semaphore_waiting_strategy>(
      1024, 10, std::chrono::seconds(10)));
  printf("\n");

  printf("moodycamel::ConcurrentQueue<std::string>:\n");
  printf("-----------------------------------------\n");
  print_results(
      run_moody_bench<std::string>(1024, 1, std::chrono::seconds(10)));
  print_results(
      run_moody_bench<std::string>(1024, 5, std::chrono::seconds(10)));
  print_results(
      run_moody_bench<std::string>(1024, 10, std::chrono::seconds(10)));

  printf("moodycamel::ConcurrentQueue<uint64_t>:\n");
  printf("-------------------------------------\n");
  print_results(run_moody_bench(1024, 1, std::chrono::seconds(10)));
  print_results(run_moody_bench(1024, 5, std::chrono::seconds(10)));
  print_results(run_moody_bench(1024, 10, std::chrono::seconds(10)));

#ifdef __linux__
  printf("broadcast_queue<uint64_t, futex_waiting_strategy>:\n");
  printf("--------------------------------------------------\n");
  print_results(run_broadcast_queue_bench<futex_waiting_strategy>(
      1024, 1, std::chrono::seconds(10)));
  print_results(run_broadcast_queue_bench<futex_waiting_strategy>(
      1024, 5, std::chrono::seconds(10)));
  print_results(run_broadcast_queue_bench<futex_waiting_strategy>(
      1024, 10, std::chrono::seconds(10)));
  printf("\n");
#endif
}
