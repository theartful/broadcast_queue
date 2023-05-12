#include <array>
#include <gtest/gtest.h>

#include "broadcast_queue.h"
#ifdef __unix__
#include "futex_waiting_strategy.h"
#endif

template <typename T, template <typename> typename WaitingStrategy>
struct TestTypes {
  using value_type = T;
  using waiting_strategy = WaitingStrategy<T>;
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

#ifdef __unix__
    TestTypes<int, broadcast_queue::futex_waiting_strategy>,
    TestTypes<float, broadcast_queue::futex_waiting_strategy>,
    TestTypes<std::array<char, 16>, broadcast_queue::futex_waiting_strategy>,
    TestTypes<std::array<char, 1024>, broadcast_queue::futex_waiting_strategy>,
    TestTypes<std::array<char, 2048>, broadcast_queue::futex_waiting_strategy>,
    TestTypes<int, broadcast_queue::futex_waiting_strategy>,
    TestTypes<float, broadcast_queue::futex_waiting_strategy>,
    TestTypes<std::array<char, 16>, broadcast_queue::futex_waiting_strategy>,
    TestTypes<std::array<char, 1024>, broadcast_queue::futex_waiting_strategy>,
    TestTypes<std::array<char, 2048>, broadcast_queue::futex_waiting_strategy>
#endif
    >;
template <typename T> class SingleThreaded : public testing::Test {};

#define VALUE_TYPE typename TypeParam::value_type
#define WAITER_TYPE typename TypeParam::waiting_strategy

TYPED_TEST_SUITE(SingleThreaded, MyTypes);

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

TYPED_TEST(SingleThreaded, PushThenDequeue) {
  broadcast_queue::sender<VALUE_TYPE, WAITER_TYPE> sender{3};
  broadcast_queue::receiver<VALUE_TYPE, WAITER_TYPE> receiver =
      sender.subscribe();

  sender.push(new_value<VALUE_TYPE>(0));
  sender.push(new_value<VALUE_TYPE>(1));
  sender.push(new_value<VALUE_TYPE>(2));

  broadcast_queue::Error error;
  VALUE_TYPE result;

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

TYPED_TEST(SingleThreaded, LaggedReceiver) {
  broadcast_queue::sender<VALUE_TYPE, WAITER_TYPE> sender{3};
  broadcast_queue::receiver<VALUE_TYPE, WAITER_TYPE> receiver =
      sender.subscribe();

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
  broadcast_queue::receiver<VALUE_TYPE, WAITER_TYPE> receiver{nullptr};

  broadcast_queue::Error error;
  VALUE_TYPE result;

  error = receiver.try_dequeue(&result);
  EXPECT_EQ(error, broadcast_queue::Error::Closed);

  do {
    broadcast_queue::sender<VALUE_TYPE, WAITER_TYPE> sender(2);
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
  broadcast_queue::sender<VALUE_TYPE, WAITER_TYPE> sender{3};

  std::array<broadcast_queue::receiver<VALUE_TYPE, WAITER_TYPE>, 10> receivers;
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
