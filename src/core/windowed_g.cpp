#include "core/windowed_g.h"

#include <cmath>

namespace ft {

std::vector<WindowedG> computeWindowedAverages(const MissionTrace& trace, std::span<const double> windowsS) {
  std::vector<WindowedG> out;
  out.reserve(windowsS.size());
  const std::size_t n = trace.size();
  // prefix[i] = sum of the first i bucket means (every bucket stands for 1 / hz).
  std::vector<double> prefix(n + 1, 0.0);
  for (std::size_t i = 0; i < n; ++i) prefix[i + 1] = prefix[i] + static_cast<double>(trace.gMean[i]);
  for (double windowS : windowsS) {
    WindowedG w;
    w.windowS = windowS;
    const double buckets = trace.hz > 0.0 && windowS > 0.0 ? std::round(windowS * trace.hz) : 0.0;
    if (buckets >= 1.0 && buckets <= static_cast<double>(n)) {
      const auto len = static_cast<std::size_t>(buckets);
      w.valid = true;
      w.maxAvgG = -INFINITY;
      w.minAvgG = INFINITY;
      for (std::size_t i = 0; i + len <= n; ++i) {
        const double avg = (prefix[i + len] - prefix[i]) / buckets;
        // Strict comparisons: the earliest window wins a tie.
        if (avg > w.maxAvgG) {
          w.maxAvgG = avg;
          w.maxAvgStartS = trace.timeAt(i);
        }
        if (avg < w.minAvgG) {
          w.minAvgG = avg;
          w.minAvgStartS = trace.timeAt(i);
        }
      }
    }
    out.push_back(w);
  }
  return out;
}

}  // namespace ft
