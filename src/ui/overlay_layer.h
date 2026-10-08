#pragma once
// Extension point for things drawn over the game each frame.
// The HUD text block is one layer; future layers: physiology screen effects
// (grey-out / blackout vignette, redout tint), G sparkline, etc.
#include "config/settings.h"
#include "core/metrics.h"

namespace ft {

struct OverlayFrame {
  const MetricsSnapshot& snapshot;
  Settings& settings;
  float screenW;
  float screenH;
  double now;
  bool editMode;
};

class IOverlayLayer {
 public:
  virtual ~IOverlayLayer() = default;
  virtual const char* name() const = 0;
  // Draw with ImGui. Return true if the layer modified `frame.settings` (triggers auto-save).
  virtual bool draw(OverlayFrame& frame) = 0;
};

}  // namespace ft
