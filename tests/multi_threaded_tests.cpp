#define BITMAP_ALLOCATOR_DEBUG

#include <array>
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

#include "broadcast_queue.h"
#include "semaphore_waiting_strategy.h"
#ifdef __linux__
#include "futex_waiting_strategy.h"
#endif
#include "condition_variable_waiting_strategy.h"
#include "utils.h"

template <typename T, typename WaitingStrategy> struct TestTypes {
  using value_type = T;
  using sender = broadcast_queue::sender<value_type, WaitingStrategy>;
  using receiver = broadcast_queue::receiver<value_type, WaitingStrategy>;
};

using MyTypes = ::testing::Types<
    TestTypes<int, broadcast_queue::condition_variable_waiting_strategy>,
    TestTypes<float, broadcast_queue::condition_variable_waiting_strategy>,
    TestTypes<std::array<char, 16>,
              broadcast_queue::condition_variable_waiting_strategy>,
    TestTypes<std::array<char, 1024>,
              broadcast_queue::condition_variable_waiting_strategy>,
    TestTypes<std::array<char, 2048>,
              broadcast_queue::condition_variable_waiting_strategy>,
    TestTypes<int, broadcast_queue::condition_variable_waiting_strategy>,
    TestTypes<float, broadcast_queue::condition_variable_waiting_strategy>,
    TestTypes<std::array<char, 16>,
              broadcast_queue::condition_variable_waiting_strategy>,
    TestTypes<std::array<char, 1024>,
              broadcast_queue::condition_variable_waiting_strategy>,
    TestTypes<std::array<char, 2048>,
              broadcast_queue::condition_variable_waiting_strategy>,
    TestTypes<std::string,
              broadcast_queue::condition_variable_waiting_strategy>,
    TestTypes<int, broadcast_queue::semaphore_waiting_strategy>,
    TestTypes<float, broadcast_queue::semaphore_waiting_strategy>,
    TestTypes<std::array<char, 16>,
              broadcast_queue::semaphore_waiting_strategy>,
    TestTypes<std::array<char, 1024>,
              broadcast_queue::semaphore_waiting_strategy>,
    TestTypes<std::array<char, 2048>,
              broadcast_queue::semaphore_waiting_strategy>,
    TestTypes<int, broadcast_queue::semaphore_waiting_strategy>,
    TestTypes<float, broadcast_queue::semaphore_waiting_strategy>,
    TestTypes<std::array<char, 16>,
              broadcast_queue::semaphore_waiting_strategy>,
    TestTypes<std::array<char, 1024>,
              broadcast_queue::semaphore_waiting_strategy>,
    TestTypes<std::array<char, 2048>,
              broadcast_queue::semaphore_waiting_strategy>,
    TestTypes<std::string, broadcast_queue::semaphore_waiting_strategy>
#ifdef __linux__
    ,
    TestTypes<int, broadcast_queue::futex_waiting_strategy>,
    TestTypes<float, broadcast_queue::futex_waiting_strategy>,
    TestTypes<std::array<char, 16>, broadcast_queue::futex_waiting_strategy>,
    TestTypes<std::array<char, 1024>, broadcast_queue::futex_waiting_strategy>,
    TestTypes<std::array<char, 2048>, broadcast_queue::futex_waiting_strategy>,
    TestTypes<int, broadcast_queue::futex_waiting_strategy>,
    TestTypes<float, broadcast_queue::futex_waiting_strategy>,
    TestTypes<std::array<char, 16>, broadcast_queue::futex_waiting_strategy>,
    TestTypes<std::array<char, 1024>, broadcast_queue::futex_waiting_strategy>,
    TestTypes<std::array<char, 2048>, broadcast_queue::futex_waiting_strategy>,
    TestTypes<std::string, broadcast_queue::futex_waiting_strategy>
#endif
    >;

template <typename T> class MultiThreaded : public testing::Test {};

#define VALUE_TYPE typename TypeParam::value_type

TYPED_TEST_SUITE(MultiThreaded, MyTypes);

TYPED_TEST(MultiThreaded, PushThenDequeue) {
  typename TypeParam::sender sender{3};
  typename TypeParam::receiver receiver = sender.subscribe();

  std::thread sender_thread{[&]() {
    EXPECT_TRUE(sender.push(new_value<VALUE_TYPE>(0)));
    EXPECT_TRUE(sender.push(new_value<VALUE_TYPE>(1)));
    EXPECT_TRUE(sender.push(new_value<VALUE_TYPE>(2)));
  }};

  std::thread receiver_thread{[&]() {
    broadcast_queue::Error error;
    VALUE_TYPE result;

    error = receiver.wait_dequeue_timed(&result, std::chrono::hours(1));
    EXPECT_EQ(error, broadcast_queue::Error::None);
    EXPECT_EQ(result, new_value<VALUE_TYPE>(0));

    error = receiver.wait_dequeue_timed(&result, std::chrono::hours(1));
    EXPECT_EQ(error, broadcast_queue::Error::None);
    EXPECT_EQ(result, new_value<VALUE_TYPE>(1));

    error = receiver.wait_dequeue_timed(&result, std::chrono::hours(1));
    EXPECT_EQ(error, broadcast_queue::Error::None);
    EXPECT_EQ(result, new_value<VALUE_TYPE>(2));
  }};

  sender_thread.join();
  receiver_thread.join();
}

TYPED_TEST(MultiThreaded, LaggedReceiver) {
  typename TypeParam::sender sender{1024};
  typename TypeParam::receiver receiver = sender.subscribe();

  std::chrono::milliseconds sender_latency{1};
  std::chrono::milliseconds receiver_latency{100};

  std::atomic<bool> should_stop{false};

  std::thread sender_thread{[&]() {
    int idx = 0;
    while (!should_stop.load(std::memory_order_relaxed)) {
      EXPECT_TRUE(sender.push(new_value<VALUE_TYPE>(idx++)));
      std::this_thread::sleep_for(sender_latency);
    }
  }};

  std::thread receiver_thread{[&]() {
    int idx = 0;
    broadcast_queue::Error error;
    VALUE_TYPE result = {};
    VALUE_TYPE expected = {};

    do {
      EXPECT_EQ(result, expected);

      expected = new_value<VALUE_TYPE>(idx++);
      error = receiver.wait_dequeue_timed(&result, std::chrono::hours(1));

      std::this_thread::sleep_for(receiver_latency);
    } while (error == broadcast_queue::Error::None);

    EXPECT_EQ(error, broadcast_queue::Error::Lagged);
    should_stop = true;
  }};

  sender_thread.join();
  receiver_thread.join();
}

TYPED_TEST(MultiThreaded, TimedWait) {
  typename TypeParam::sender sender{100};
  typename TypeParam::receiver receiver = sender.subscribe();

  std::atomic<bool> started{false};

  std::thread receiver_thread{[&]() {
    broadcast_queue::Error error;

    VALUE_TYPE result;
    started = true;
    error = receiver.wait_dequeue_timed(&result, std::chrono::seconds(5));

    EXPECT_EQ(error, broadcast_queue::Error::None);
    EXPECT_EQ(result, new_value<VALUE_TYPE>(123));
  }};

  while (!started)
    ;

  std::this_thread::sleep_for(std::chrono::seconds(2));
  EXPECT_TRUE(sender.push(new_value<VALUE_TYPE>(123)));

  receiver_thread.join();
}
