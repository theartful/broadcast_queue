#include "broadcast_queue.h"
#include <array>
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

using MyTypes =
    ::testing::Types<int, float, std::array<char, 16>, std::array<char, 1024>,
                     std::array<char, 2048>>;

template <typename T> class MultiThreaded : public testing::Test {};

TYPED_TEST_SUITE(MultiThreaded, MyTypes);

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

TYPED_TEST(MultiThreaded, PushThenDequeue) {
  broadcast_queue::sender<TypeParam> sender{3};
  broadcast_queue::receiver<TypeParam> receiver = sender.subscribe();

  std::thread sender_thread{[&]() {
    sender.push(new_value<TypeParam>(0));
    sender.push(new_value<TypeParam>(1));
    sender.push(new_value<TypeParam>(2));
  }};

  std::thread receiver_thread{[&]() {
    broadcast_queue::Error error;
    TypeParam result;

    error = receiver.wait_dequeue_timed(&result, std::chrono::hours(1));
    EXPECT_EQ(error, broadcast_queue::Error::None);
    EXPECT_EQ(result, new_value<TypeParam>(0));

    error = receiver.wait_dequeue_timed(&result, std::chrono::hours(1));
    EXPECT_EQ(error, broadcast_queue::Error::None);
    EXPECT_EQ(result, new_value<TypeParam>(1));

    error = receiver.wait_dequeue_timed(&result, std::chrono::hours(1));
    EXPECT_EQ(error, broadcast_queue::Error::None);
    EXPECT_EQ(result, new_value<TypeParam>(2));
  }};

  sender_thread.join();
  receiver_thread.join();
}

TYPED_TEST(MultiThreaded, LaggedReceiver) {
  broadcast_queue::sender<TypeParam> sender{100};
  broadcast_queue::receiver<TypeParam> receiver = sender.subscribe();

  std::chrono::microseconds sender_latency{100};
  std::chrono::microseconds receiver_latency{200};

  std::atomic<bool> should_stop = false;

  std::thread sender_thread{[&]() {
    int idx = 0;
    while (!should_stop.load(std::memory_order_relaxed)) {
      sender.push(new_value<TypeParam>(idx++));
      std::this_thread::sleep_for(sender_latency);
    }
  }};

  std::thread receiver_thread{[&]() {
    int idx = 0;
    broadcast_queue::Error error;
    TypeParam result = {};
    TypeParam expected = {};

    do {
      EXPECT_EQ(result, expected);

      expected = new_value<TypeParam>(idx++);
      error = receiver.wait_dequeue_timed(&result, std::chrono::hours(1));

      std::this_thread::sleep_for(receiver_latency);
    } while (error == broadcast_queue::Error::None);

    EXPECT_EQ(error, broadcast_queue::Error::Lagged);
    should_stop = true;
  }};

  sender_thread.join();
  receiver_thread.join();
}
