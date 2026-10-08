#pragma once
// Thread-safe "latest value" mailbox. In-game, the pipeline runs on the game thread
// and the overlay renders on the render thread; this is the hand-off between them.
// The prototype is single-threaded but uses the same hand-off to keep the shape.
#include <mutex>

namespace ft {

template <typename T>
class LatestValue {
 public:
  void publish(const T& value) {
    std::lock_guard lock(mutex_);
    value_ = value;
  }
  T read() const {
    std::lock_guard lock(mutex_);
    return value_;
  }

 private:
  mutable std::mutex mutex_;
  T value_{};
};

}  // namespace ft
