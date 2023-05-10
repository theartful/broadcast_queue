# A single-producer, multiple-consumer broadcast queue for POD datatypes for C++11

This repository contains an implementation of a fixed-size seqlock broadcast queue
based on the paper [Can Seqlocks Get Along With Programming Language Memory Models?][1]
by Hans Bohem.

## Reasons to Use

The main setting for using this kind of implementation is real-time applications
where slow receivers are not tolerated. Here, the `sender` doesn't care about
`receiver`s not being able to catch up, and writes into the queue without giving
them any regard. For examaple, my personal use case was writing a server that
serves real-time audio data for many clients. I can't have the server waiting
on clients, and I can't create a queue for each client since the memory usage
would be suboptimal. So instead, all clients share the same queue, and any lagging
client will simply drop elements after being notified.

## Example

``` C++
#include <broadcast_queue.h>
#include <cassert>

int main()
{
  static constexpr size_t CAPACITY = 3;
  broadcast_queue::sender<int> sender{CAPACITY};

  auto receiver = sender.subscribe();

  sender.push(1); // always succeeds since this is a circular queue without any
                  // notion of being full since we don't care about receivers!
  sender.push(2);

  auto receiver2 = sender.subscribe(); // will receive data after the point of subscription
  sender.push(3);

  int result;
  broadcast_queue::Error error;

  error = receiver.try_dequeue(&result);
  assert(error == broadcast_queue::Error::None);
  assert(result == 1);

  error = receiver.try_dequeue(&result);
  assert(error == broadcast_queue::Error::None);
  assert(result == 2);

  error = receiver.try_dequeue(&result);
  assert(error == broadcast_queue::Error::None);
  assert(result == 3);

  error = receiver2.try_dequeue(&result);
  assert(error == broadcast_queue::Error::None);
  assert(result == 3);
}
```

## Use

Just copy "broadcast_queue.h" into your application and you're good to go.

You can also use CMake to use this library by adding these lines into your
CMakeLists.txt file:
```cmake
include(FetchContent)

FetchContent_Declare(
  broadcast_queue
  GIT_REPOSITORY    https://github.com/theartful/broadcast_queue
  GIT_TAG           master
)

FetchContent_MakeAvailable(broadcast_queue)

target_link_libraries(target PUBLIC broadcast_queue)
```

[1]: https://www.hpl.hp.com/techreports/2012/HPL-2012-68.pdf 

