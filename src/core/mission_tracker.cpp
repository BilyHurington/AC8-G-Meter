#include "core/mission_tracker.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace ft {

void MissionTracker::start(double traceHz) {
  trace_.hz = traceHz;
  open_ = true;
  clearStats();
}

void MissionTracker::restartTrace(double startS) {
  trace_.clear();
  trace_.startS = startS;
  bucket_ = {};
  tracedToS_ = startS;
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

void MissionTracker::add(const FlightMetrics& m, double dtS, bool highGEntered, double atS) {
  MissionStats& s = stats_;
  const double now = std::min(atS, s.durationS);

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

  if (highGEntered) ++s.highGEpisodes;
  // The episode clock starts at 0 on the entering sample, so that dt is not counted.
  if (m.inHighG && !highGEntered) s.totalHighGS += dtS;
  s.longestHighGS = std::max(s.longestHighGS, m.highGDurationS);

  traceSample(m, now);
}

// The sample covers (tracedToS_, now]: split it over the buckets that span overlaps, each
// weighted by its share of the time. A sample with no duration (first one, or right after a
// gap that added no time) still counts for the extremes of the current bucket. kEpsS absorbs
// the round-off of summed frame times, so a sample ending a hair past a bucket boundary does
// not leak its value into the next bucket.
void MissionTracker::traceSample(const FlightMetrics& m, double now) {
  constexpr double kEpsS = 1e-9;
  if (!(trace_.hz > 0.0)) return;
  double from = tracedToS_;
  tracedToS_ = std::max(tracedToS_, now);
  if (now <= from + kEpsS) {
    if (trace_.size() < kMaxTracePoints) mergeIntoBucket(m, 0.0);
    return;
  }
  while (trace_.size() < kMaxTracePoints) {
    const double bucketEnd = trace_.timeAt(trace_.size() + 1);
    const double to = std::min(now, bucketEnd);
    if (to > from + kEpsS || !bucket_.any) mergeIntoBucket(m, std::max(to - from, 0.0));
    if (now < bucketEnd - kEpsS) break;
    closeBucket();
    from = bucketEnd;
    if (now <= from + kEpsS) break;
  }
}

void MissionTracker::mergeIntoBucket(const FlightMetrics& m, double weightS) {
  Bucket& b = bucket_;
  const float g = static_cast<float>(m.g);
  if (!b.any) {
    b.any = true;
    b.p.gMin = b.p.gMax = g;
    b.p.onset = static_cast<float>(m.onsetRate);
    b.p.aoaDeg = std::numeric_limits<float>::quiet_NaN();
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
  b.p.highG = b.p.highG || m.inHighG;
}

void MissionTracker::closeBucket() {
  if (!bucket_.any) return;
  TracePoint p = bucket_.p;
  if (bucket_.weightS > 0.0) {
    // Rounding can put the mean a hair outside [min, max]; keep the band consistent.
    p.gMean = std::clamp(static_cast<float>(bucket_.gSum / bucket_.weightS), p.gMin, p.gMax);
  }
  trace_.push(p);
  bucket_ = {};
}

MissionReport MissionTracker::finish(bool aborted) {
  open_ = false;
  if (trace_.size() < kMaxTracePoints) closeBucket();  // the last, partial bucket
  MissionReport r;
  r.id = nextId_++;
  r.aborted = aborted;
  r.stats = stats_;
  r.trace = std::move(trace_);
  trace_ = {};
  trace_.hz = r.trace.hz;
  restartTrace(0.0);
  return r;
}

}  // namespace ft
