#include "core/mission_tracker.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "core/windowed_g.h"

namespace ft {

namespace {

// Round-off of summed frame times: a sample ending a hair past a bucket boundary does not leak
// its value into the next bucket.
constexpr double kEpsS = 1e-9;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

}  // namespace

// ---- Bucketer ---------------------------------------------------------------------------

void MissionTracker::Bucketer::restart(double startS) {
  index_ = 0;
  tracedToS_ = startS;
  any_ = false;
}

template <typename Merge, typename Close>
void MissionTracker::Bucketer::add(double now, const MissionTrace& grid, Merge&& merge, Close&& close) {
  if (!(grid.hz > 0.0)) return;
  double from = tracedToS_;
  tracedToS_ = std::max(tracedToS_, now);
  if (now <= from + kEpsS) {
    if (index_ < kMaxTracePoints) {
      merge(0.0, !any_);
      any_ = true;
    }
    return;
  }
  while (index_ < kMaxTracePoints) {
    const double bucketEnd = grid.timeAt(index_ + 1);
    const double to = std::min(now, bucketEnd);
    if (to > from + kEpsS || !any_) {
      merge(std::max(to - from, 0.0), !any_);
      any_ = true;
    }
    if (now < bucketEnd - kEpsS) break;
    closeOpen(close);
    from = bucketEnd;
    if (now <= from + kEpsS) break;
  }
}

// ---- MissionTracker ---------------------------------------------------------------------

void MissionTracker::start(double traceHz) {
  trace_.hz = traceHz;
  open_ = true;
  clearStats();
}

void MissionTracker::restartTrace(double startS) {
  trace_.clear();
  trace_.startS = startS;
  bucket_ = {};
  sBucket_ = {};
  sustainedG_.clear();
  sustainedHighG_.clear();
  live_.restart(startS);
  sustained_.restart(startS);
}

void MissionTracker::clearStats() {
  stats_ = {};
  restartTrace(0.0);
}

void MissionTracker::resetGStats() {
  const MissionStats old = stats_;
  stats_ = {};
  stats_.hasData = old.hasData;
  stats_.durationS = old.durationS;
  stats_.maxAoaDeg = old.maxAoaDeg;
  stats_.maxSpeed = old.maxSpeed;
  stats_.acceptedSamples = old.acceptedSamples;
  stats_.rejectedSamples = old.rejectedSamples;
  stats_.extremeGSamples = old.extremeGSamples;  // |n|, independent of the G mode
  restartTrace(old.durationS);
}

void MissionTracker::addTime(double dtS) {
  stats_.durationS += dtS;
  stats_.hasData = true;
}

void MissionTracker::add(const FlightMetrics& m, double dtS) {
  MissionStats& s = stats_;
  const double now = s.durationS;
  if (!s.hasGData) {
    s.hasGData = true;
    s.maxG = s.minG = m.g;
    s.maxGAtS = s.minGAtS = now;
    s.maxOnset = s.minOnset = m.onsetRate;
  }
  if (m.g > s.maxG) {
    s.maxG = m.g;
    s.maxGAtS = now;
  }
  if (m.g < s.minG) {
    s.minG = m.g;
    s.minGAtS = now;
  }
  s.maxOnset = std::max(s.maxOnset, m.onsetRate);
  s.minOnset = std::min(s.minOnset, m.onsetRate);
  if (m.aoaValid) s.maxAoaDeg = std::max(s.maxAoaDeg, m.aoaDeg);
  s.maxSpeed = std::max(s.maxSpeed, m.speed);
  s.timeInGBinS[gBinIndex(m.g)] += dtS;

  live_.add(now, trace_, [&](double weightS, bool first) { mergeIntoBucket(m, weightS, first); },
            [&] { closeBucket(); });
}

void MissionTracker::addSustained(const SustainedSample& x) {
  MissionStats& s = stats_;
  const double now = std::min(x.atS, s.durationS);
  if (x.confirmed) {
    if (!s.hasSustainedData) {
      s.hasSustainedData = true;
      s.sustainedMaxG = s.sustainedMinG = x.g;
      s.sustainedMaxGAtS = s.sustainedMinGAtS = now;
    }
    if (x.g > s.sustainedMaxG) {
      s.sustainedMaxG = x.g;
      s.sustainedMaxGAtS = now;
    }
    if (x.g < s.sustainedMinG) {
      s.sustainedMinG = x.g;
      s.sustainedMinGAtS = now;
    }
  }
  if (x.highGEntered) ++s.highGEpisodes;
  // The episode clock starts at 0 on the entering sample, so that dt is not counted.
  if (x.inHighG && !x.highGEntered) s.totalHighGS += x.dtS;
  s.longestHighGS = std::max(s.longestHighGS, x.highGDurationS);

  if (x.confirmed)
    sustained_.add(now, trace_, [&](double weightS, bool) { mergeSustained(x, weightS); }, [&] { closeSustained(); });
}

void MissionTracker::mergeIntoBucket(const FlightMetrics& m, double weightS, bool first) {
  Bucket& b = bucket_;
  const float g = static_cast<float>(m.g);
  if (first) {
    b.p.gMin = b.p.gMax = g;
    b.p.onset = static_cast<float>(m.onsetRate);
    b.p.aoaDeg = kNaN;
  }
  b.weightS += weightS;
  b.gSum += m.g * weightS;
  b.p.gMean = g;  // used as-is when the bucket only saw zero-length samples
  b.p.gMin = std::min(b.p.gMin, g);
  b.p.gMax = std::max(b.p.gMax, g);
  if (std::abs(m.onsetRate) > std::abs(static_cast<double>(b.p.onset))) b.p.onset = static_cast<float>(m.onsetRate);
  if (m.aoaValid) {
    const float aoa = static_cast<float>(m.aoaDeg);
    b.p.aoaDeg = std::isnan(b.p.aoaDeg) ? aoa : std::max(b.p.aoaDeg, aoa);
  }
  b.p.speed = static_cast<float>(m.speed);
  b.p.altitude = static_cast<float>(m.altitude);
}

void MissionTracker::closeBucket() {
  TracePoint p = bucket_.p;
  if (bucket_.weightS > 0.0) {
    // Rounding can put the mean a hair outside [min, max]; keep the band consistent.
    p.gMean = std::clamp(static_cast<float>(bucket_.gSum / bucket_.weightS), p.gMin, p.gMax);
  }
  p.gSustained = kNaN;  // both filled from the sustained columns at finish()
  p.highG = false;
  trace_.push(p);
  bucket_ = {};
}

void MissionTracker::mergeSustained(const SustainedSample& s, double weightS) {
  SustainedBucket& b = sBucket_;
  b.weightS += weightS;
  b.gSum += s.g * weightS;
  b.lastG = s.g;
  b.highG = b.highG || s.inHighG;
}

void MissionTracker::closeSustained() {
  const SustainedBucket& b = sBucket_;
  sustainedG_.push_back(static_cast<float>(b.weightS > 0.0 ? b.gSum / b.weightS : b.lastG));
  sustainedHighG_.push_back(b.highG ? 1 : 0);
  sBucket_ = {};
}

MissionReport MissionTracker::finish(bool aborted) {
  open_ = false;
  live_.finish([&] { closeBucket(); });  // the last, partial buckets
  sustained_.finish([&] { closeSustained(); });
  // Buckets the sustained path did not reach (its lag at the end of the mission, when it was
  // not flushed) stay NaN / not high-G.
  for (std::size_t i = 0; i < trace_.size() && i < sustainedG_.size(); ++i) {
    trace_.gSustained[i] = sustainedG_[i];
    trace_.highG[i] = sustainedHighG_[i];
  }
  MissionReport r;
  r.id = nextId_++;
  r.aborted = aborted;
  r.stats = stats_;
  r.trace = std::move(trace_);
  r.windowed = computeWindowedAverages(r.trace, r.stats.durationS);
  trace_ = {};
  trace_.hz = r.trace.hz;
  restartTrace(0.0);
  return r;
}

}  // namespace ft
