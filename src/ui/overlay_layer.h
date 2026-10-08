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

// Layers that need their own live data (e.g. a physiology model computing G-LOC state) follow
// the same threading as the HUD: the model runs on the game thread next to the pipeline and
// publishes its state to its own LatestValue<State>; the layer is constructed with a reference
// to that LatestValue and read()s it in draw() on the render thread:
//
//   class PhysiologyLayer final : public IOverlayLayer {
//    public:
//     explicit PhysiologyLayer(const LatestValue<PhysiologyState>& state) : state_(state) {}
//     bool draw(OverlayFrame& f) override { const PhysiologyState s = state_.read(); ... }
//    private:
//     const LatestValue<PhysiologyState>& state_;
//   };
//   ui.addLayer(std::make_unique<PhysiologyLayer>(physiologyState));
class IOverlayLayer {
 public:
  virtual ~IOverlayLayer() = default;
  virtual const char* name() const = 0;
  // Draw with ImGui. Return true if the layer modified `frame.settings` (triggers auto-save).
  virtual bool draw(OverlayFrame& frame) = 0;
};

}  // namespace ft
