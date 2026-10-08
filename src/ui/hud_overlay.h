#pragma once
// The telemetry text block ("G  +6.3", "MAX G  +7.9", ...) drawn over the game.
// Normal mode: an input-transparent ImGui window, so it never steals clicks from the game.
// Edit mode: the block can be dragged; the position is stored normalized in the settings.
#include <array>
#include <string>
#include <vector>

#include "ui/overlay_layer.h"

struct ImFont;

namespace ft {

class HudOverlayLayer final : public IOverlayLayer {
 public:
  static constexpr double kRefreshPeriodS = 0.1;  // text refresh rate (10 Hz): readable digits

  void setFont(ImFont* font) { font_ = font; }  // nullptr -> current ImGui font
  const char* name() const override { return "hud"; }
  bool draw(OverlayFrame& frame) override;

  const std::vector<std::string>& lines() const { return lines_; }  // last displayed text

 private:
  void refreshLines(const OverlayFrame& frame);

  ImFont* font_ = nullptr;
  std::vector<std::string> lines_;
  double lastRefresh_ = -1.0e9;
  std::array<bool, kMetricCount> lastVisible_{};
  bool dragging_ = false;
  float dragStartX_ = 0.0f, dragStartY_ = 0.0f;  // text origin when the drag began, px
  float dragX_ = 0.0f, dragY_ = 0.0f;            // current (clamped) drag position, px
};

}  // namespace ft
