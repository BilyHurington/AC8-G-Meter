#pragma once
// Thread-safe "latest value" mailbox. In-game, the pipeline runs on the game thread
// and the overlay renders on the render thread; this is the hand-off between them.
// The prototype is single-threaded but uses the same hand-off to keep the shape.
//
// Each publish() bumps a version counter, so a consumer that only cares about changes
// (e.g. the game thread applying settings published by the UI) can poll with
// readIfNewer() instead of copying the value every frame.
#include <cstdint>
#include <mutex>

namespace ft {

template <typename T>
class LatestValue {
 public:
  void publish(const T& value) {
    std::lock_guard lock(mutex_);
    value_ = value;
    ++version_;
  }
  T read() const {
    std::lock_guard lock(mutex_);
    return value_;
  }
  // Copies the value into `out` and updates `seenVersion` only if something was published
  // since `seenVersion` (start with 0: nothing seen yet). Returns true if `out` was written.
  bool readIfNewer(std::uint64_t& seenVersion, T& out) const {
    std::lock_guard lock(mutex_);
    if (version_ == seenVersion) return false;
    out = value_;
    seenVersion = version_;
    return true;
  }

 private:
  mutable std::mutex mutex_;
  T value_{};
  std::uint64_t version_ = 0;  // number of publish() calls
};

}  // namespace ft
