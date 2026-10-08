#include "core/metrics.h"

#include <initializer_list>

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

void MissionTrace::push(const TracePoint& p) {
  gMean.push_back(p.gMean);
  gMin.push_back(p.gMin);
  gMax.push_back(p.gMax);
  gSustained.push_back(p.gSustained);
  onset.push_back(p.onset);
  aoaDeg.push_back(p.aoaDeg);
  speed.push_back(p.speed);
  altitude.push_back(p.altitude);
  highG.push_back(p.highG ? 1 : 0);
}

TracePoint MissionTrace::at(std::size_t i) const {
  return {gMean[i], gMin[i], gMax[i], gSustained[i], onset[i], aoaDeg[i], speed[i], altitude[i], highG[i] != 0};
}

void MissionTrace::clear() {
  for (auto* column : {&gMean, &gMin, &gMax, &gSustained, &onset, &aoaDeg, &speed, &altitude}) column->clear();
  highG.clear();
}

}  // namespace ft
