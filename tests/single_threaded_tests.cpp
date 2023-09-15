#define BITMAP_ALLOCATOR_DEBUG

#include <array>
#include <gtest/gtest.h>

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
    TestTypes<int, broadcast_queue::default_waiting_strategy>,
    TestTypes<float, broadcast_queue::default_waiting_strategy>,
    TestTypes<std::array<char, 16>, broadcast_queue::default_waiting_strategy>,
    TestTypes<std::array<char, 1024>,
              broadcast_queue::default_waiting_strategy>,
    TestTypes<std::array<char, 2048>,
              broadcast_queue::default_waiting_strategy>,
    TestTypes<int, broadcast_queue::default_waiting_strategy>,
    TestTypes<float, broadcast_queue::default_waiting_strategy>,
    TestTypes<std::array<char, 16>, broadcast_queue::default_waiting_strategy>,
    TestTypes<std::array<char, 1024>,
              broadcast_queue::default_waiting_strategy>,
    TestTypes<std::array<char, 2048>,
              broadcast_queue::default_waiting_strategy>,
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
template <typename T> class SingleThreaded : public testing::Test {};

#define VALUE_TYPE typename TypeParam::value_type

TYPED_TEST_SUITE(SingleThreaded, MyTypes);

TYPED_TEST(SingleThreaded, LaggedReceiver) {
  typename TypeParam::sender sender{3};
  typename TypeParam::receiver receiver = sender.subscribe();

  sender.push(new_value<VALUE_TYPE>(0));
  sender.push(new_value<VALUE_TYPE>(1));
  sender.push(new_value<VALUE_TYPE>(2));
  sender.push(new_value<VALUE_TYPE>(3));

  broadcast_queue::Error error;
  VALUE_TYPE result;

  error = receiver.try_dequeue(&result);
  EXPECT_EQ(error, broadcast_queue::Error::Lagged);

  // lagging caused the receiver to resubscribe losing all previous data
  sender.push(new_value<VALUE_TYPE>(4));
  sender.push(new_value<VALUE_TYPE>(5));

  error = receiver.try_dequeue(&result);
  EXPECT_EQ(error, broadcast_queue::Error::None);
  EXPECT_EQ(result, new_value<VALUE_TYPE>(4));

  error = receiver.try_dequeue(&result);
  EXPECT_EQ(error, broadcast_queue::Error::None);
  EXPECT_EQ(result, new_value<VALUE_TYPE>(5));
}

TYPED_TEST(SingleThreaded, ClosedSender) {
  typename TypeParam::receiver receiver{nullptr};

  broadcast_queue::Error error;
  VALUE_TYPE result;

  error = receiver.try_dequeue(&result);
  EXPECT_EQ(error, broadcast_queue::Error::Closed);

  do {
    typename TypeParam::sender sender(2);
    receiver = sender.subscribe();
    sender.push(new_value<VALUE_TYPE>(0));
    sender.push(new_value<VALUE_TYPE>(1));

    error = receiver.try_dequeue(&result);
    EXPECT_EQ(error, broadcast_queue::Error::None);
    EXPECT_EQ(result, new_value<VALUE_TYPE>(0));
  } while (false);

  error = receiver.try_dequeue(&result);
  EXPECT_EQ(error, broadcast_queue::Error::Closed);
}

TYPED_TEST(SingleThreaded, MultipleReceivers) {
  typename TypeParam::sender sender{3};

  std::array<typename TypeParam::receiver, 10> receivers;
  std::fill(receivers.begin(), receivers.end(), sender.subscribe());

  sender.push(new_value<VALUE_TYPE>(0));
  sender.push(new_value<VALUE_TYPE>(1));
  sender.push(new_value<VALUE_TYPE>(2));

  broadcast_queue::Error error;
  VALUE_TYPE result;

  for (auto &receiver : receivers) {
    error = receiver.try_dequeue(&result);
    EXPECT_EQ(error, broadcast_queue::Error::None);
    EXPECT_EQ(result, new_value<VALUE_TYPE>(0));

    error = receiver.try_dequeue(&result);
    EXPECT_EQ(error, broadcast_queue::Error::None);
    EXPECT_EQ(result, new_value<VALUE_TYPE>(1));

    error = receiver.try_dequeue(&result);
    EXPECT_EQ(error, broadcast_queue::Error::None);
    EXPECT_EQ(result, new_value<VALUE_TYPE>(2));
  }
}
