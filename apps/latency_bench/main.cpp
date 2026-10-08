// latency_bench: G display latency and error vs. sample rate and velocity noise.
//
//   latency_bench [--duration S] [--scenario level|turn|pullpush|dogfight] [--seed N] [--csv]
//
// Flies the simulated scenario at each sample rate and noise level, pushes it through
// TelemetryPipeline with adaptive filtering (the defaults) and with the legacy fixed windows
// (80 ms LS window, 5-sample median, tau 50 ms), and compares the G (Nz) with the truth:
//   rms0   zero-lag RMS error: what the pilot sees, delay included
//   lag    delay (0 - 300 ms) that best aligns the estimate with the truth
//   rmsAl  RMS error at that delay: the estimation quality with the delay removed
//   peak   highest estimated G minus highest true G
//   win    adaptive LS window at the end of the run (FilterState::accelWindowS)
// A benchmark, not a test: nothing is asserted.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/pipeline.h"
#include "sources/simulated_source.h"

namespace {

using namespace ft;

struct Result {
  double rms0 = 0.0, lagS = 0.0, rmsAligned = 0.0, peakErr = 0.0, windowS = 0.0;
};

struct Run {
  std::vector<RawFlightSample> samples;
  std::vector<double> truthT, truthNz;
};

Run simulate(Scenario scenario, double fps, double noise, double duration, std::uint32_t seed) {
  SimulatedSourceOptions o;
  o.scenario = scenario;
  o.frameRateHz = fps;
  o.physicsHz = std::max(2000.0, fps);
  o.dirty.velocityNoiseMS = noise;
  o.seed = seed;
  SimulatedSource sim(o);
  Run run;
  std::vector<FlightState> truth;
  sim.generate(duration, run.samples, &truth);
  for (const FlightState& s : truth) {
    run.truthT.push_back(s.t);
    run.truthNz.push_back(s.trueNz);
  }
  return run;
}

double truthAt(const Run& run, double t) {
  const auto it = std::lower_bound(run.truthT.begin(), run.truthT.end(), t);
  if (it == run.truthT.begin()) return run.truthNz.front();
  if (it == run.truthT.end()) return run.truthNz.back();
  const std::size_t k = static_cast<std::size_t>(it - run.truthT.begin());
  const double w = (t - run.truthT[k - 1]) / (run.truthT[k] - run.truthT[k - 1]);
  return run.truthNz[k - 1] + w * (run.truthNz[k] - run.truthNz[k - 1]);
}

Result evaluate(const Run& run, const PipelineConfig& cfg) {
  TelemetryPipeline pipeline(cfg);
  std::vector<double> t, g;
  for (const RawFlightSample& s : run.samples) {
    pipeline.push(s);
    const FlightMetrics& m = pipeline.snapshot().current;
    if (m.valid && s.t > 1.0) {  // skip the warm-up
      t.push_back(s.t);
      g.push_back(m.g);
    }
  }
  auto rmsAt = [&](double lag) {
    double sumSq = 0.0;
    for (std::size_t i = 0; i < t.size(); ++i) {
      const double e = g[i] - truthAt(run, t[i] - lag);
      sumSq += e * e;
    }
    return std::sqrt(sumSq / static_cast<double>(std::max<std::size_t>(t.size(), 1)));
  };
  Result r;
  r.rms0 = r.rmsAligned = rmsAt(0.0);
  for (double lag = 0.0025; lag <= 0.3 + 1e-9; lag += 0.0025) {
    const double e = rmsAt(lag);
    if (e < r.rmsAligned) {
      r.rmsAligned = e;
      r.lagS = lag;
    }
  }
  if (!g.empty())
    r.peakErr = *std::max_element(g.begin(), g.end()) -
                *std::max_element(run.truthNz.begin(), run.truthNz.end());
  r.windowS = pipeline.snapshot().filter.accelWindowS;
  return r;
}

int usage() {
  std::fprintf(stderr, "usage: latency_bench [--duration S] [--scenario level|turn|pullpush|dogfight] "
                       "[--seed N] [--csv]\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  double duration = 120.0;
  std::uint32_t seed = 7;
  Scenario scenario = Scenario::DogfightMix;
  bool csv = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const bool hasValue = i + 1 < argc;
    if (a == "--csv") csv = true;
    else if (a == "--duration" && hasValue) duration = std::atof(argv[++i]);
    else if (a == "--seed" && hasValue) seed = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    else if (a == "--scenario" && hasValue) {
      const std::string s = argv[++i];
      if (s == "level") scenario = Scenario::LevelCruise;
      else if (s == "turn") scenario = Scenario::SustainedTurn;
      else if (s == "pullpush") scenario = Scenario::PullUpPushOver;
      else if (s == "dogfight") scenario = Scenario::DogfightMix;
      else return usage();
    } else return usage();
  }
  if (!(duration >= 3.0)) return usage();

  PipelineConfig adaptive;
  PipelineConfig fixed;
  fixed.adaptiveFiltering = false;
  fixed.accelWindowS = 0.08;
  fixed.medianWindow = 5;
  fixed.smoothingTauS = 0.05;

  if (csv) {
    std::printf("fps,noise_ms,mode,rms0_g,lag_ms,rms_aligned_g,peak_err_g,window_ms\n");
  } else {
    std::printf("%s, %.0f s, seed %u. Errors in G, lag in ms, noise in m/s (per axis).\n",
                scenarioName(scenario), duration, seed);
    std::printf("%5s %5s | %-33s | %s\n", "", "", "adaptive (default)", "fixed (80 ms, median 5, tau 50 ms)");
    std::printf("%5s %5s | %6s %5s %6s %6s %5s | %6s %5s %6s %6s\n", "fps", "noise", "rms0", "lag", "rmsAl",
                "peak", "win", "rms0", "lag", "rmsAl", "peak");
  }
  for (double fps : {30.0, 60.0, 120.0, 144.0, 240.0, 480.0, 1000.0}) {
    for (double noise : {0.0, 0.02, 0.05, 0.1, 0.2}) {
      const Run run = simulate(scenario, fps, noise, duration, seed);
      const Result a = evaluate(run, adaptive);
      const Result f = evaluate(run, fixed);
      if (csv) {
        for (const auto& [mode, r] : {std::pair{"adaptive", a}, std::pair{"fixed", f}})
          std::printf("%.0f,%.2f,%s,%.4f,%.1f,%.4f,%.4f,%.1f\n", fps, noise, mode, r.rms0, r.lagS * 1000.0,
                      r.rmsAligned, r.peakErr, r.windowS * 1000.0);
      } else {
        std::printf("%5.0f %5.2f | %6.3f %5.1f %6.3f %+6.3f %5.1f | %6.3f %5.1f %6.3f %+6.3f\n", fps, noise,
                    a.rms0, a.lagS * 1000.0, a.rmsAligned, a.peakErr, a.windowS * 1000.0, f.rms0,
                    f.lagS * 1000.0, f.rmsAligned, f.peakErr);
      }
      std::fflush(stdout);
    }
  }
  return 0;
}
