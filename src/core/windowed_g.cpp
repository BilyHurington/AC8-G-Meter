#include "core/windowed_g.h"

#include <cmath>

namespace ft {

std::vector<WindowedG> computeWindowedAverages(const MissionTrace& trace, double tracedEndS,
                                               std::span<const double> windowsS) {
  std::vector<WindowedG> out;
  out.reserve(windowsS.size());
  const std::size_t n = trace.size();
  // prefix[i] = sum of the first i bucket means (every full bucket stands for 1 / hz).
  std::vector<double> prefix(n + 1, 0.0);
  for (std::size_t i = 0; i < n; ++i) prefix[i + 1] = prefix[i] + static_cast<double>(trace.gMean[i]);
  // Share of the last bucket that was really traced, in (0, 1].
  double lastShare = 1.0;
  if (n > 0 && trace.hz > 0.0 && tracedEndS > trace.timeAt(n - 1) && tracedEndS < trace.endS())
    lastShare = (tracedEndS - trace.timeAt(n - 1)) * trace.hz;
  const bool partial = lastShare < 1.0 - 1e-6;
  for (double windowS : windowsS) {
    WindowedG w;
    w.windowS = windowS;
    const double buckets = trace.hz > 0.0 && windowS > 0.0 ? std::round(windowS * trace.hz) : 0.0;
    // Traced buckets: n, or n - 1 + lastShare (the window must fit in the traced time).
    const double traced = static_cast<double>(n) - (partial ? 1.0 - lastShare : 0.0);
    if (buckets >= 1.0 && buckets <= traced + 1e-6) {
      const auto len = static_cast<std::size_t>(buckets);
      w.valid = true;
      w.maxAvgG = -INFINITY;
      w.minAvgG = INFINITY;
      auto consider = [&](double avg, double startS) {
        // Strict comparisons: the earliest window wins a tie.
        if (avg > w.maxAvgG) {
          w.maxAvgG = avg;
          w.maxAvgStartS = startS;
        }
        if (avg < w.minAvgG) {
          w.minAvgG = avg;
          w.minAvgStartS = startS;
        }
      };
      // Windows of whole buckets; with a partial last bucket only those before it.
      const std::size_t whole = partial ? n - 1 : n;
      for (std::size_t i = 0; i + len <= whole; ++i) consider((prefix[i + len] - prefix[i]) / buckets, trace.timeAt(i));
      if (partial) {
        // The window ending at tracedEndS: the last (partial) bucket at its share, the len - 1
        // buckets before it, and the remaining (1 - share) of the bucket before those.
        // (len <= n - 1 + share, so with share < 1 that bucket exists.)
        const std::size_t first = n - len;  // first whole bucket of the window
        const double sum = lastShare * static_cast<double>(trace.gMean[n - 1]) + (prefix[n - 1] - prefix[first]) +
                           (1.0 - lastShare) * static_cast<double>(trace.gMean[first - 1]);
        consider(sum / buckets, tracedEndS - buckets / trace.hz);
      }
    }
    out.push_back(w);
  }
  return out;
}

}  // namespace ft
