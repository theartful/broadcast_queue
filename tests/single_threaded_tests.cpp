#include "broadcast_queue.h"
#include <array>
#include <gtest/gtest.h>

using MyTypes =
    ::testing::Types<int, float, std::array<char, 16>, std::array<char, 1024>,
                     std::array<char, 2048>>;

template <typename T> class SingleThreaded : public testing::Test {};

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
  broadcast_queue::sender<TypeParam> sender{3};
  broadcast_queue::receiver<TypeParam> receiver = sender.subscribe();

  sender.push(new_value<TypeParam>(0));
  sender.push(new_value<TypeParam>(1));
  sender.push(new_value<TypeParam>(2));

  broadcast_queue::Error error;
  TypeParam result;

  error = receiver.try_dequeue(&result);
  EXPECT_EQ(error, broadcast_queue::Error::None);
  EXPECT_EQ(result, new_value<TypeParam>(0));

  error = receiver.try_dequeue(&result);
  EXPECT_EQ(error, broadcast_queue::Error::None);
  EXPECT_EQ(result, new_value<TypeParam>(1));

  error = receiver.try_dequeue(&result);
  EXPECT_EQ(error, broadcast_queue::Error::None);
  EXPECT_EQ(result, new_value<TypeParam>(2));
}

TYPED_TEST(SingleThreaded, LaggedReceiver) {
  broadcast_queue::sender<TypeParam> sender{3};
  broadcast_queue::receiver<TypeParam> receiver = sender.subscribe();

  sender.push(new_value<TypeParam>(0));
  sender.push(new_value<TypeParam>(1));
  sender.push(new_value<TypeParam>(2));
  sender.push(new_value<TypeParam>(3));

  broadcast_queue::Error error;
  TypeParam result;

  error = receiver.try_dequeue(&result);
  EXPECT_EQ(error, broadcast_queue::Error::Lagged);

  // lagging caused the receiver to resubscribe losing all previous data
  sender.push(new_value<TypeParam>(4));
  sender.push(new_value<TypeParam>(5));

  error = receiver.try_dequeue(&result);
  EXPECT_EQ(error, broadcast_queue::Error::None);
  EXPECT_EQ(result, new_value<TypeParam>(4));

  error = receiver.try_dequeue(&result);
  EXPECT_EQ(error, broadcast_queue::Error::None);
  EXPECT_EQ(result, new_value<TypeParam>(5));
}

TYPED_TEST(SingleThreaded, ClosedSender) {
  broadcast_queue::receiver<TypeParam> receiver{nullptr};

  broadcast_queue::Error error;
  TypeParam result;

  error = receiver.try_dequeue(&result);
  EXPECT_EQ(error, broadcast_queue::Error::Closed);

  do {
    broadcast_queue::sender<TypeParam> sender(2);
    receiver = sender.subscribe();
    sender.push(new_value<TypeParam>(0));
    sender.push(new_value<TypeParam>(1));

    error = receiver.try_dequeue(&result);
    EXPECT_EQ(error, broadcast_queue::Error::None);
    EXPECT_EQ(result, new_value<TypeParam>(0));
  } while (false);

  error = receiver.try_dequeue(&result);
  EXPECT_EQ(error, broadcast_queue::Error::Closed);
}

TYPED_TEST(SingleThreaded, MultipleReceivers) {
  broadcast_queue::sender<TypeParam> sender{3};

  std::array<broadcast_queue::receiver<TypeParam>, 10> receivers;
  std::fill(receivers.begin(), receivers.end(), sender.subscribe());

  sender.push(new_value<TypeParam>(0));
  sender.push(new_value<TypeParam>(1));
  sender.push(new_value<TypeParam>(2));

  broadcast_queue::Error error;
  TypeParam result;

  for (auto &receiver : receivers) {
    error = receiver.try_dequeue(&result);
    EXPECT_EQ(error, broadcast_queue::Error::None);
    EXPECT_EQ(result, new_value<TypeParam>(0));

    error = receiver.try_dequeue(&result);
    EXPECT_EQ(error, broadcast_queue::Error::None);
    EXPECT_EQ(result, new_value<TypeParam>(1));

    error = receiver.try_dequeue(&result);
    EXPECT_EQ(error, broadcast_queue::Error::None);
    EXPECT_EQ(result, new_value<TypeParam>(2));
  }
}
