#ifndef BROADCAST_QUEUE_TEST_UTILS
#define BROADCAST_QUEUE_TEST_UTILS

#include <string>

// pretty much std::iota
template <typename T> inline T new_value(int idx) {
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
  // making a string long to avoid small string optimization and force an
  // allocation
  return std::string("this is a string having the index number: ") +
         std::to_string(idx);
}

#endif
