// Unit tests for the small building blocks: math, metrics containers, filters, kinematics.
#include <doctest/doctest.h>

#include <random>

#include "core/filters.h"
#include "core/kinematics.h"
#include "core/math.h"
#include "core/metrics.h"
#include "core/path_utf8.h"

using namespace ft;
using doctest::Approx;

namespace {
void checkVec(const Vec3& a, const Vec3& b, double eps = 1e-9) {
  CHECK(a.x == Approx(b.x).epsilon(eps));
  CHECK(a.y == Approx(b.y).epsilon(eps));
  CHECK(a.z == Approx(b.z).epsilon(eps));
}
void checkVecAbs(const Vec3& a, const Vec3& b, double tol) {
  CHECK(length(a - b) < tol);
}
}  // namespace

TEST_CASE("Basis::fromUERotator identity and single-axis rotations") {
  SUBCASE("identity") {
    const Basis b = Basis::fromUERotator(0, 0, 0);
    checkVecAbs(b.forward, {1, 0, 0}, 1e-12);
    checkVecAbs(b.right, {0, 1, 0}, 1e-12);
    checkVecAbs(b.up, {0, 0, 1}, 1e-12);
  }
  SUBCASE("pitch 90: nose straight up, canopy facing back") {
    const Basis b = Basis::fromUERotator(90, 0, 0);
    checkVecAbs(b.forward, {0, 0, 1}, 1e-12);
    checkVecAbs(b.right, {0, 1, 0}, 1e-12);
    checkVecAbs(b.up, {-1, 0, 0}, 1e-12);
  }
  SUBCASE("roll 90: right wing down") {
    const Basis b = Basis::fromUERotator(0, 0, 90);
    checkVecAbs(b.forward, {1, 0, 0}, 1e-12);
    checkVecAbs(b.right, {0, 0, -1}, 1e-12);
    checkVecAbs(b.up, {0, 1, 0}, 1e-12);
  }
  SUBCASE("yaw 90: nose east (+Y)") {
    const Basis b = Basis::fromUERotator(0, 90, 0);
    checkVecAbs(b.forward, {0, 1, 0}, 1e-12);
    checkVecAbs(b.right, {-1, 0, 0}, 1e-12);
    checkVecAbs(b.up, {0, 0, 1}, 1e-12);
  }
}

TEST_CASE("Basis::fromUERotator is orthonormal and toBody/toWorld round-trip") {
  std::mt19937 rng(7);
  std::uniform_real_distribution<double> deg(-180.0, 180.0);
  for (int i = 0; i < 200; ++i) {
    const Basis b = Basis::fromUERotator(deg(rng), deg(rng), deg(rng));
    REQUIRE(b.isOrthonormal(1e-9));
    const Vec3 w{deg(rng), deg(rng), deg(rng)};
    checkVec(b.toWorld(b.toBody(w)), w, 1e-9);
  }
}

TEST_CASE("gBinIndex bucket edges") {
  CHECK(gBinIndex(-5.0) == 0);
  CHECK(gBinIndex(-1.0) == 1);
  CHECK(gBinIndex(-0.5) == 1);
  CHECK(gBinIndex(0.0) == 2);
  CHECK(gBinIndex(1.0) == 3);
  CHECK(gBinIndex(2.99) == 3);
  CHECK(gBinIndex(5.0) == 5);
  CHECK(gBinIndex(9.0) == 7);
  CHECK(gBinIndex(11.99) == 7);
  CHECK(gBinIndex(12.0) == 8);
  CHECK(gBinIndex(15.0) == 9);
  CHECK(kGBinCount == 10);
  CHECK(gBinIndex(50.0) == kGBinCount - 1);
}

TEST_CASE("RecentHistory keeps the newest kCapacity values in order") {
  RecentHistory h;
  for (int i = 0; i < 10; ++i) h.push(static_cast<float>(i));
  CHECK(h.count == 10);
  CHECK(h.at(0) == 0.0f);
  CHECK(h.at(9) == 9.0f);
  for (int i = 10; i < RecentHistory::kCapacity + 25; ++i) h.push(static_cast<float>(i));
  CHECK(h.count == RecentHistory::kCapacity);
  CHECK(h.at(0) == 25.0f);
  CHECK(h.at(h.count - 1) == static_cast<float>(RecentHistory::kCapacity + 24));
  h.clear();
  CHECK(h.count == 0);
}

TEST_CASE("MedianFilter removes an isolated spike and forces an odd window") {
  MedianFilter f(4);
  CHECK(f.window() == 5);
  CHECK(MedianFilter(100).window() == MedianFilter::kMaxWindow);
  CHECK(MedianFilter(0).window() == 1);

  for (int i = 0; i < 5; ++i) f.update(1.0);
  CHECK(f.update(12.0) == 1.0);  // spike
  CHECK(f.update(1.0) == 1.0);
  // Two adjacent outliers out of five are still rejected.
  f.update(-8.0);
  CHECK(f.update(-8.0) == 1.0);
}

TEST_CASE("Ema response depends on time, not on frame rate") {
  // After exactly one time constant a step response reaches 1 - 1/e at any rate.
  for (double fps : {30.0, 60.0, 144.0, 1000.0}) {
    Ema e;
    e.setTau(0.1);
    e.update(0.0, 0.0);
    double y = 0.0;
    const int n = static_cast<int>(std::lround(0.1 * fps));
    for (int i = 0; i < n; ++i) y = e.update(1.0, 0.1 / n);
    CHECK(y == Approx(1.0 - std::exp(-1.0)).epsilon(1e-9));
  }
  Ema off;  // tau = 0 -> passthrough
  off.update(3.0, 0.01);
  CHECK(off.update(5.0, 0.01) == 5.0);
}

TEST_CASE("TimedWindow slope is exact for linear data with uneven spacing") {
  TimedWindow<double> w;
  std::mt19937 rng(3);
  std::uniform_real_distribution<double> dt(0.002, 0.04);
  double t = 1000.0;  // large absolute time must not hurt precision
  for (int i = 0; i < 40; ++i) {
    w.push(t, 2.5 * t - 7.0);
    w.trim(0.15, 3);
    t += dt(rng);
  }
  CHECK(w.slope() == Approx(2.5).epsilon(1e-9));
  CHECK(w.time(w.size() - 1) - w.time(0) <= 0.15 + 0.04);

  TimedWindow<double> sparse;  // fewer points than the time window: keeps minCount
  sparse.push(0.0, 0.0);
  sparse.push(1.0, 1.0);
  sparse.push(2.0, 2.0);
  sparse.trim(0.1, 3);
  CHECK(sparse.size() == 3);
}

TEST_CASE("TimedWindow: span-aware trim, popNewest, trimBefore, slope of the oldest n") {
  // Frames at 0, 30, 40, 50 ms with a 25 ms window: plain trimming would keep 40 and 50 only
  // (span 10 ms); with minSpanS = 15 ms it keeps one older sample (30 ms: span 20 ms).
  TimedWindow<double> w;
  for (double t : {0.0, 0.030, 0.040, 0.050}) {
    w.push(t, 2.0 * t);
    w.trim(0.025, 2, 0.015);
  }
  CHECK(w.size() == 3);
  CHECK(w.time(0) == Approx(0.030));
  w.popNewest();
  CHECK(w.size() == 2);
  CHECK(w.time(1) == Approx(0.040));
  CHECK(w.slope(1) == 0.0);
  CHECK(w.slope(2) == Approx(2.0));
  w.trimBefore(0.035);
  CHECK(w.size() == 1);
  CHECK(w.time(0) == Approx(0.040));
}

TEST_CASE("KinematicsEstimator: warm-up span rule, look-back helpers") {
  const Vec3 a{0.0, 0.0, 20.0};
  KinematicsEstimator k;
  k.setWindows(0.08, 0.15);
  auto v = [&](double t) { return Vec3{200.0, 0.0, 0.0} + a * t; };
  // A single sample predicts constant velocity (gates the second sample after a reset).
  k.add(0.0, {}, v(0.0));
  REQUIRE(k.predictVelocity(0.01));
  checkVec(*k.predictVelocity(0.01), v(0.0));
  CHECK_FALSE(k.predictVelocityWithoutNewest(0.01));
  // 60 Hz: no acceleration until the window spans 0.6 * 80 ms.
  for (int i = 1; i <= 2; ++i) k.add(i / 60.0, {}, v(i / 60.0));
  CHECK_FALSE(k.hasAcceleration());
  k.add(3 / 60.0, {}, v(3 / 60.0));
  CHECK(k.hasAcceleration());
  // Warm-up only: a longer window keeps reporting while it fills.
  k.setWindows(0.3, 0.15);
  k.add(4 / 60.0, {}, v(4 / 60.0));
  CHECK(k.hasAcceleration());
  checkVec(k.acceleration(), a, 1e-6);

  // Suspect samples (addHeld) are used for predictions, but the estimates stay those from
  // before them and the window is not trimmed; removing them restores the trend.
  const int size = k.size();
  const double accelT = k.accelerationTime();
  k.addHeld(5 / 60.0, v(5 / 60.0) + Vec3{0.0, 0.0, 3.0});
  k.addHeld(6 / 60.0, v(6 / 60.0) + Vec3{0.0, 0.0, 3.0});
  CHECK(k.size() == size + 2);
  checkVec(k.acceleration(), a, 1e-6);
  checkVec(k.velocity(), v(4 / 60.0), 1e-9);
  CHECK(k.accelerationTime() == accelT);
  CHECK(length(*k.predictVelocity(7 / 60.0) - v(7 / 60.0)) > 1.0);
  checkVec(*k.predictVelocityWithoutNewest(7 / 60.0, 2), v(7 / 60.0), 1e-6);
  k.removeNewest(2);
  CHECK(k.size() == size);
  checkVec(*k.predictVelocity(7 / 60.0), v(7 / 60.0), 1e-6);
  CHECK(k.lastTime() == Approx(4 / 60.0));
  // A confirmed held sample: the next add() estimates from everything.
  k.addHeld(5 / 60.0, v(5 / 60.0));
  k.add(6 / 60.0, {}, v(6 / 60.0));
  checkVec(k.velocity(), v(6 / 60.0), 1e-9);
  checkVec(k.acceleration(), a, 1e-6);
}

TEST_CASE("TimedWindow: slope and mean time of a sub-range") {
  TimedWindow<double> w;
  // v = 1 + 2 t for t < 0.3, then 10 t: the sub-range from index 3 sees only the second part.
  for (int i = 0; i < 6; ++i) {
    const double t = 0.1 * i;
    w.push(t, t < 0.3 ? 1.0 + 2.0 * t : 10.0 * t);
  }
  CHECK(w.slope(3) == Approx(2.0));
  CHECK(w.slope(3, 3) == Approx(10.0));
  CHECK(w.slope(-1, 3) == Approx(10.0));  // to the newest
  CHECK(w.meanTime(3, 3) == Approx(0.4));
  CHECK(w.slope(1, 5) == 0.0);
}

TEST_CASE("KinematicsEstimator: kept history, held samples committed as of the oldest") {
  const Vec3 a{0.0, 0.0, 20.0};
  auto v = [&](double t) { return Vec3{200.0, 0.0, 0.0} + a * t; };
  KinematicsEstimator k;
  // 30 ms window, 80 ms of history: a longer window applies at once.
  k.setWindows(0.03, 0.15, 0.08);
  for (int i = 0; i <= 12; ++i) k.add(i / 100.0, {}, v(i / 100.0));
  CHECK(k.size() == 9);  // 0.04 - 0.12 s
  CHECK(k.accelerationTime() == Approx(0.105));
  k.setWindows(0.08, 0.15, 0.08);
  k.add(0.13, {}, v(0.13));
  CHECK(k.accelerationTime() == Approx(0.09));
  checkVec(k.acceleration(), a, 1e-6);

  // A velocity step held for two samples, then confirmed: the estimate is that of the window
  // ending at the first held sample (the step in it), the second stays out until the next add.
  k.setWindows(0.025, 0.15, 0.08);
  const Vec3 step{0.0, 0.0, 2.0};
  k.addHeld(0.14, v(0.14) + step);
  k.addHeld(0.15, v(0.15) + step);
  checkVec(k.acceleration(), a, 1e-6);  // unchanged while held
  k.commitHeld(2);
  CHECK(k.accelerationTime() == Approx(0.13));  // 0.12 - 0.14
  CHECK(k.acceleration().z > a.z + 50.0);         // the step is in the window
  checkVec(k.velocity(), v(0.14) + step, 1e-9);
  k.add(0.16, {}, v(0.16) + step);
  CHECK(k.accelerationTime() == Approx(0.15));  // 0.14 - 0.16
}

TEST_CASE("KinematicsEstimator: velocity slope and position quadratic fit") {
  const Vec3 a{3.0, -2.0, 9.0};
  const Vec3 v0{200.0, 10.0, -5.0};
  const Vec3 p0{1e5, -2e5, 3000.0};
  std::mt19937 rng(11);
  std::uniform_real_distribution<double> dt(1.0 / 144.0, 1.0 / 30.0);

  KinematicsEstimator fromVel, fromPos;
  double t = 50.0;
  for (int i = 0; i < 30; ++i) {
    const double s = t - 50.0;
    fromVel.add(t, p0 + v0 * s + a * (0.5 * s * s), v0 + a * s);
    fromPos.add(t, p0 + v0 * s + a * (0.5 * s * s), std::nullopt);
    t += dt(rng);
  }
  REQUIRE(fromVel.hasAcceleration());
  REQUIRE(fromPos.hasAcceleration());
  checkVecAbs(fromVel.acceleration(), a, 1e-6);
  checkVecAbs(fromPos.acceleration(), a, 1e-3);
  checkVecAbs(fromPos.velocity(), fromVel.velocity(), 1e-3);
  // Position fit: exact data -> no residual noise, and it extrapolates the parabola.
  CHECK(fromPos.fitResidualSigma() < 1e-6);
  const double tNext = t + 0.01, sNext = tNext - 50.0;
  REQUIRE(fromPos.predictPosition(tNext));
  checkVecAbs(*fromPos.predictPosition(tNext), p0 + v0 * sNext + a * (0.5 * sNext * sNext), 1e-4);
  CHECK_FALSE(fromVel.predictPosition(tNext));  // velocity input has no position fit
  // Both refer their acceleration to the window's mean sample time.
  CHECK(fromVel.accelerationTime() < t - 0.02);
  CHECK(fromPos.accelerationTime() < t - 0.05);

  KinematicsEstimator e;
  e.add(0.0, {}, Vec3{});
  e.add(0.01, {}, Vec3{});
  CHECK_FALSE(e.hasAcceleration());  // needs 3 points
}

TEST_CASE("path_utf8: non-ASCII paths round-trip through UTF-8") {
  // "飞行/Ü.csv" spelled as UTF-8 bytes, so the test does not depend on the source encoding.
  const std::string utf8 = "\xE9\xA3\x9E\xE8\xA1\x8C/\xC3\x9C.csv";
  const std::filesystem::path p = pathFromUtf8(utf8);
  CHECK(pathToUtf8(p) == utf8);
  CHECK(pathToUtf8(p.filename()) == "\xC3\x9C.csv");
  CHECK(pathToUtf8(pathFromUtf8("")).empty());
}
