#include "core/metrics.h"

namespace ft {

int gBinIndex(double g) {
  int bin = 0;
  while (bin < static_cast<int>(kGBinEdges.size()) && g >= kGBinEdges[bin]) ++bin;
  return bin;
}

void RecentHistory::push(float v) {
  values[head] = v;
  head = (head + 1) % kCapacity;
  if (count < kCapacity) ++count;
}

float RecentHistory::at(int i) const {
  const int oldest = (head - count + kCapacity) % kCapacity;
  return values[(oldest + i) % kCapacity];
}

}  // namespace ft
