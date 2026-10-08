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
