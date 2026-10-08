// NoiseEstimator: robust per-axis velocity noise and sample interval.
#include <doctest/doctest.h>

#include <memory>
#include <random>

#include "core/noise_estimator.h"
#include "flight_helpers.h"

using namespace ft;
using namespace ft::test;
using doctest::Approx;

namespace {

// Feeds `traj`'s velocity plus gaussian noise at a fixed rate for durationS.
void feed(NoiseEstimator& est, const Trajectory& traj, double fps, double sigma, double durationS,
          std::uint32_t seed = 1) {
  Gaussian noise(seed);
  for (int i = 0; i < static_cast<int>(durationS * fps); ++i) {
    const double t = i / fps;
    est.add(t, traj(t).velocity + noise.vec(sigma));
  }
}

}  // namespace

TEST_CASE("NoiseEstimator measures white velocity noise on a manoeuvring trajectory") {
  const Trajectory loop = verticalLoop(600.0);  // ~7.8 G, attitude and velocity rotating
  for (double fps : {60.0, 1000.0}) {
    CAPTURE(fps);
    auto est = std::make_unique<NoiseEstimator>();
    CHECK_FALSE(est->ready());
    feed(*est, loop, fps, 0.05, 5.0);
    CHECK(est->ready());
    CHECK(est->sigma() == Approx(0.05).epsilon(0.2));
    CHECK(est->sampleInterval() == Approx(1.0 / fps).epsilon(1e-6));
  }
}

TEST_CASE("NoiseEstimator reads ~0 on clean data, even in a manoeuvre") {
  auto est = std::make_unique<NoiseEstimator>();
  feed(*est, verticalLoop(600.0), 60.0, 0.0, 3.0);
  CHECK(est->ready());
  CHECK(est->sigma() < 0.005);  // only the motion's jerk * dt^2 / 2 is left (~2 mm/s here)
}

TEST_CASE("NoiseEstimator handles uneven frame time") {
  // Frame time uniform in [1/144, 1/30] s: the normalised residual stays ~sigma per axis and
  // the interval is the median frame time.
  auto est = std::make_unique<NoiseEstimator>();
  const Trajectory turn = levelTurn(70.0);
  Gaussian noise(5);
  std::mt19937 rng(11);
  std::uniform_real_distribution<double> dist(1.0 / 144.0, 1.0 / 30.0);
  for (double t = 0.0; t < 10.0; t += dist(rng)) est->add(t, turn(t).velocity + noise.vec(0.05));
  CHECK(est->sigma() == Approx(0.05).epsilon(0.2));
  CHECK(est->sampleInterval() == Approx(0.5 * (1.0 / 144.0 + 1.0 / 30.0)).epsilon(0.1));
}

TEST_CASE("NoiseEstimator ignores sparse glitches and discontinuities") {
  auto est = std::make_unique<NoiseEstimator>();
  const Trajectory level = levelFlight();
  Gaussian noise(3);
  for (int i = 0; i < 600; ++i) {
    const double t = i / 60.0;
    Vec3 v = level(t).velocity + noise.vec(0.02);
    if (i % 50 == 25) v.z += 80.0;  // 2% glitches
    est->add(t, v);
    if (i == 300) est->restartSequence();  // keeps the statistics
  }
  CHECK(est->ready());
  CHECK(est->sigma() == Approx(0.02).epsilon(0.2));

  est->reset();
  CHECK_FALSE(est->ready());
  CHECK(est->sigma() == 0.0);
  CHECK(est->sampleInterval() == 0.0);
}

TEST_CASE("NoiseEstimator: fast attack, slow release") {
  // 1000 Hz, sigma 0.005 -> 0.5 m/s at t = 3 s and back to 0.005 at t = 6 s. More noise must
  // show within ~0.1 s (it widens the spike gate and the window); less noise only after ~1 s.
  auto est = std::make_unique<NoiseEstimator>();
  const Trajectory turn = levelTurn(60.0);
  Gaussian noise(21);
  double sigmaAt3_1 = 0.0, sigmaAt6_2 = 0.0, sigmaAt8 = 0.0;
  for (int i = 0; i < 8000; ++i) {
    const double t = i / 1000.0;
    est->add(t, turn(t).velocity + noise.vec(t >= 3.0 && t < 6.0 ? 0.5 : 0.005));
    if (i == 3100) sigmaAt3_1 = est->sigma();
    if (i == 6200) sigmaAt6_2 = est->sigma();
    if (i == 7999) sigmaAt8 = est->sigma();
  }
  CHECK(sigmaAt3_1 == Approx(0.5).epsilon(0.3));
  CHECK(sigmaAt6_2 == Approx(0.5).epsilon(0.3));  // still the 2 s median
  CHECK(sigmaAt8 < 0.01);
}

TEST_CASE("NoiseEstimator: a cluster of glitches is not a noise step") {
  // 10 % of the samples off by 3 m/s (each disturbs three residuals): the short window uses
  // a quartile, so only a sustained change moves the estimate.
  auto est = std::make_unique<NoiseEstimator>();
  const Trajectory level = levelFlight();
  std::mt19937 rng(5);
  double worst = 0.0;
  for (int i = 0; i < 600; ++i) {
    const double t = i / 60.0;
    Vec3 v = level(t).velocity;
    if (rng() % 10 == 0) v.z += 3.0;
    est->add(t, v);
    if (est->ready()) worst = std::max(worst, est->sigma());
  }
  CHECK(worst < 0.05);
}

TEST_CASE("NoiseEstimator: the sample interval follows a rate drop within 0.5 s") {
  auto est = std::make_unique<NoiseEstimator>();
  const Trajectory turn = levelTurn(60.0);
  double t = 0.0;
  for (; t < 4.0; t += 1.0 / 240.0) est->add(t, turn(t).velocity);
  CHECK(est->sampleInterval() == Approx(1.0 / 240.0).epsilon(0.01));
  const double drop = t;
  for (; t < drop + 0.5; t += 1.0 / 60.0) est->add(t, turn(t).velocity);
  CHECK(est->sampleInterval() == Approx(1.0 / 60.0).epsilon(0.01));  // took ~1.6 s before
}

TEST_CASE("NoiseEstimator: near-duplicate timestamps are not the sample rate") {
  // Every 60 Hz frame delivered twice, 1 us apart.
  auto est = std::make_unique<NoiseEstimator>();
  const Trajectory turn = levelTurn(60.0);
  for (int i = 0; i < 180; ++i) {
    const double t = i / 60.0;
    est->add(t, turn(t).velocity);
    est->add(t + 1e-6, turn(t + 1e-6).velocity);
  }
  CHECK(est->sampleInterval() == Approx(1.0 / 60.0).epsilon(0.01));
  CHECK(est->sigma() < 0.005);
}
