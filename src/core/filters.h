#pragma once
// Small signal-processing building blocks used by the pipeline. All of them work with
// variable frame time and allocate nothing per sample.
#include <algorithm>
#include <array>

namespace ft {

// Running median over the last N samples. Removes single-sample spikes without the lag
// a low-pass would need to suppress them. N is forced odd and clamped to [1, kMaxWindow].
class MedianFilter {
 public:
  static constexpr int kMaxWindow = 15;

  explicit MedianFilter(int window = 5) { setWindow(window); }
  void setWindow(int window);  // also resets
  int window() const { return window_; }
  void reset() { count_ = 0; head_ = 0; }
  // Median of the samples seen so far (up to `window`), including x.
  double update(double x);

 private:
  std::array<double, kMaxWindow> buf_{};
  int window_ = 1;
  int count_ = 0;
  int head_ = 0;
};

// First-order low-pass with time constant tau. alpha = 1 - exp(-dt / tau) makes the
// response independent of frame rate. tau <= 0 passes input through. The first sample
// after reset() initialises the state (no ramp-up from zero).
class Ema {
 public:
  void setTau(double tauS) { tau_ = tauS; }
  void reset() { initialised_ = false; }
  double update(double x, double dtS);
  double value() const { return value_; }

 private:
  double tau_ = 0.0;
  double value_ = 0.0;
  bool initialised_ = false;
};

// Median (kills isolated spikes) followed by EMA (removes frame-to-frame jitter).
class Smoother {
 public:
  void configure(int medianWindow, double tauS) {
    median_.setWindow(medianWindow);
    ema_.setTau(tauS);
    ema_.reset();
  }
  void setTau(double tauS) { ema_.setTau(tauS); }
  void reset() {
    median_.reset();
    ema_.reset();
  }
  double update(double x, double dtS) { return ema_.update(median_.update(x), dtS); }

 private:
  MedianFilter median_;
  Ema ema_;
};

// Fixed-capacity ring of timestamped values with a least-squares slope. T needs
// T{} == zero, T + T, T - T and T * double (double and Vec3 both qualify).
// Capacity bounds memory; at very high sample rates the window is shortened to fit.
template <typename T, int Capacity = 256>
class TimedWindow {
 public:
  void reset() { count_ = 0; head_ = 0; }

  void push(double t, const T& v) {
    times_[head_] = t;
    values_[head_] = v;
    head_ = (head_ + 1) % Capacity;
    if (count_ < Capacity) ++count_;
  }

  // Drops samples older than windowS before the newest, but always keeps at least minCount
  // (so a low frame rate still yields an estimate), and keeps one sample older than windowS
  // while the others span less than minSpanS (so irregular frame times never leave a window
  // too short for its estimate).
  void trim(double windowS, int minCount, double minSpanS = 0.0) {
    if (count_ == 0) return;
    const double newest = time(count_ - 1);
    while (count_ > std::max(minCount, 1) && newest - time(0) > windowS && newest - time(1) >= minSpanS)
      --count_;
  }
  // Drops samples older than t.
  void trimBefore(double t) {
    while (count_ > 0 && time(0) < t) --count_;
  }
  void popNewest() {
    if (count_ == 0) return;
    head_ = (head_ - 1 + Capacity) % Capacity;
    --count_;
  }

  int size() const { return count_; }
  // i in [0, size()): 0 = oldest.
  double time(int i) const { return times_[index(i)]; }
  const T& value(int i) const { return values_[index(i)]; }

  // Mean time of the oldest n samples (default: all).
  double meanTime(int n = -1) const {
    n = clampCount(n);
    double sum = 0.0;
    for (int i = 0; i < n; ++i) sum += time(i);
    return n > 0 ? sum / n : 0.0;
  }

  // Least-squares dv/dt over the oldest n samples (default: all). Times are centred on their
  // mean, so uneven spacing is handled exactly and large absolute timestamps lose no precision.
  T slope(int n = -1) const {
    n = clampCount(n);
    if (n < 2) return T{};
    const double tMean = meanTime(n);
    T vMean{};
    for (int i = 0; i < n; ++i) vMean = vMean + value(i);
    vMean = vMean * (1.0 / n);
    double stt = 0.0;
    T stv{};
    for (int i = 0; i < n; ++i) {
      const double dt = time(i) - tMean;
      stt += dt * dt;
      stv = stv + (value(i) - vMean) * dt;
    }
    return stt > 1e-12 ? stv * (1.0 / stt) : T{};
  }

 private:
  int index(int i) const { return (head_ - count_ + i + Capacity) % Capacity; }
  int clampCount(int n) const { return n < 0 || n > count_ ? count_ : n; }

  std::array<double, Capacity> times_{};
  std::array<T, Capacity> values_{};
  int count_ = 0;
  int head_ = 0;
};

}  // namespace ft
