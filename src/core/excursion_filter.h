#pragma once
// ExcursionFilter: removes excursions shorter than a time width W from a sampled signal,
// keeping everything sustained for at least W exactly.
//
//   out = closing(opening(in)),  opening = dilation(erosion(in)),  closing = erosion(dilation(.))
//
// with flat structuring elements W wide in *time* (uneven sample intervals are fine):
//   opening removes peaks narrower than W ("the highest level held for at least W"),
//   closing removes dips narrower than W; plateaus and steps of at least W are preserved.
// A sample stands for the interval since the previous one, so a plateau of N samples at
// interval dt lasts N * dt. Each stage is a sliding min / max over a monotonic deque
// (amortised O(1) per sample). The two dilation / erosion stages that look ahead delay the
// output by up to 2 W; every pushed sample comes out exactly once, in order, with its own
// timestamp. Windows are truncated at the start of the stream and at flush(), so an
// excursion within W of either end is only filtered by the part of the window that exists.
//
// Used for the mission statistics and trace (see TelemetryPipeline), never for the live G.
#include <cstddef>
#include <vector>

namespace ft {

// Growable FIFO / deque on a power-of-two ring: no allocation once it has reached its
// working size.
template <typename T>
class RingQueue {
 public:
  bool empty() const { return count_ == 0; }
  std::size_t size() const { return count_; }
  T& front() { return buf_[head_]; }
  T& back() { return buf_[(head_ + count_ - 1) & (buf_.size() - 1)]; }
  void push_back(const T& v) {
    if (count_ == buf_.size()) grow();
    buf_[(head_ + count_) & (buf_.size() - 1)] = v;
    ++count_;
  }
  void pop_front() {
    head_ = (head_ + 1) & (buf_.size() - 1);
    --count_;
  }
  void pop_back() { --count_; }
  void clear() { head_ = count_ = 0; }

 private:
  void grow() {
    std::vector<T> bigger(buf_.empty() ? 16 : buf_.size() * 2);
    for (std::size_t i = 0; i < count_; ++i) bigger[i] = buf_[(head_ + i) & (buf_.size() - 1)];
    buf_.swap(bigger);
    head_ = 0;
  }
  std::vector<T> buf_;
  std::size_t head_ = 0, count_ = 0;
};

class ExcursionFilter {
 public:
  struct Sample {
    double t = 0.0, v = 0.0;
  };

  // Samples a look-ahead stage may hold before it completes the oldest one early (an absurd
  // input rate: 0.5 s at 8 kHz). Keeps memory bounded whatever the input.
  static constexpr std::size_t kMaxPending = 4096;

  explicit ExcursionFilter(double widthS = 0.0) {
    s2_.max = s3_.max = true;
    setWidth(widthS);
  }

  // W in seconds; <= 0 passes samples straight through (no delay). Changing it while samples
  // are pending flushes them first, with the old width.
  void setWidth(double widthS);
  double width() const { return w_; }

  // t must not decrease.
  void push(double t, double v);
  // Next filtered sample, in input order; false when none is ready yet.
  bool pop(Sample& out);
  // Completes every pending sample (truncated look-ahead windows) and starts afresh, as if
  // the stream ended here. Call at the end of a mission.
  void flush();
  // Drops everything, pending samples included.
  void reset();
  // Samples pushed but not yet available to pop().
  std::size_t pending() const { return pending2_.size() + pending4_.size(); }

 private:
  // Sliding extremum over a deque of (t, v) kept monotonic in v.
  struct Window {
    RingQueue<Sample> dq;
    bool max = false;
    void add(const Sample& s);
    void dropBefore(double t, bool inclusive);  // removes samples with t < (or <=) the bound
  };

  void emit2();  // completes the oldest sample of stage 2 and feeds it on
  void emit4();
  void feed3(const Sample& s);

  double w_ = 0.0;
  Window s1_, s2_, s3_, s4_;  // erosion (trailing), dilation (leading), dilation (trailing), erosion (leading)
  RingQueue<double> pending2_, pending4_;  // timestamps waiting for their look-ahead window
  RingQueue<Sample> out_;
};

}  // namespace ft
