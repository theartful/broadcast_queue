# A single-producer, multiple-consumer broadcast queue for C++11

This repository contains an implementation of a fixed-size seqlock broadcast queue
based on the paper [Can Seqlocks Get Along With Programming Language Memory Models?][1]
by Hans Bohem.

## Semantics

This is a fixed-size circular queue that consists of two parts: a sender and
one or more receivers. Its API is inspired by Golang's channels. The sender
broadcasts its values to all receivers that have subscribed to it. When a receiver
subscribes to a sender, it starts receiving data from the point it subscribed.

If the sender writes over a part of the queue before a receiver dequeues it,
the receiver is considered to be lagging behind. In this case, the next time
the receiver tries to dequeue a value, it will receive a `broadcast_queue::Error::Lagged`
error. The receiver's position in the queue is then updated to the tip of the queue,
which is the point where the sender will write the next value. Once the sender
writes the next value, the receiver will receive it as if it had just resubscribed.

If the sender dies, the channel is considered closed, and if a receiver tries
to dequeue data from it, it will receive a `broadcast_queue::Error::Closed` error.

## Reasons to use

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

## Is this queue lock-free?

It depends on the waiting strategy employed. The default strategy uses condition
variables, which requires locks but results in lower CPU usage. Alternatively,
the semaphore waiting strategy is lock-free with faster read/write speeds, at the
cost of higher CPU usage due to busy waiting.

## Use

Add the following lines to your CMakeLists.txt file:
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

## TODO

- [x] Implement the `semaphore` class on Windows.
- [x] Implement the `semaphore` class on macOS.
- [x] Support non trivially copyable and non trivially destructible data types:
    - [x] Implement a lock-free bitmap allocator.
    - [x] Implement a two-layer broadcast-queue, the first layer would consist
    of pointers employing the same strategy already used, and the second layer
    would contain the actual data, which would be allocated using the
    aforementioned lock-free bitmap allocator.
- [x] Try 128-bit atomic intrinsics on x64 architecture (CMPXCHG16B) to get rid
of seqlocks in the case when stored data is 64-bit long.
- [ ] Support multi producers. Two approaches in mind:
    - [ ] Try out [flat combining][5].
    - [ ] Or more realistically, split the writer cursor into two: pending and
    committed, where the pending one is always ahead of or equal to the committed
    one, and the elements between the pending and the committed would represent
    the ones being written by the producers at that moment.
- [ ] Provide benchmark results against java's disruptor, and similar implementations
of the disruptor pattern in C++.

## Resources

* [Can Seqlocks Get Along With Programming Language Memory Models?][1]
* [Trading at light speed: designing low latency systems in C++ - David Gross - Meeting C++ 2022][2]
* [LMAX Disruptor and the Concepts of Mechanical Sympathy][3]
* [Building a Lock-free Multi-producer, Multi-consumer Queue for Tcmalloc - Matt Kulukundis - CppCon 21][4]
* [Flat Combining and the Synchronization-Parallelism Tradeoff][5]

[1]: https://dl.acm.org/doi/10.1145/2247684.2247688
[2]: https://www.youtube.com/watch?v=8uAW5FQtcvE
[3]: https://www.youtube.com/watch?v=Qho1QNbXBso
[4]: https://www.youtube.com/watch?v=_qaKkHuHYE0
[5]: https://people.csail.mit.edu/shanir/publications/Flat%20Combining%20SPAA%2010.pdf
