#pragma once
// Interactive G-over-mission-time chart for the mission summary (ImGui draw-list based).
// Shows the gMin..gMax band and the gMean line of a MissionTrace, the high-G threshold with
// shaded high-G episodes, the 1 G reference, max / min G markers and, optionally, the onset
// rate or AoA on a secondary axis. Mouse wheel zooms around the cursor, dragging pans,
// double-click resets; hovering shows the values under the cursor.
#include <cstddef>
#include <cstdint>
#include <vector>

#include <imgui.h>

#include "core/metrics.h"
#include "ui/chart_math.h"

namespace ft {

class GChart {
 public:
  // Draws into a box of `size` at the cursor (size.x <= 0: available width). Zoom, pan and the
  // overlay choice persist across frames; the view resets when a different report is shown.
  void draw(const MissionReport& report, ImVec2 size);
  void resetView() { viewValid_ = false; }

  ChartSeries overlay() const { return overlay_; }
  void setOverlay(ChartSeries s) { overlay_ = s; }

 private:
  struct ReportKey {
    const MissionReport* ptr = nullptr;
    std::uint32_t id = 0;
    std::size_t size = 0;
    double startS = 0.0, durationS = 0.0;
    bool operator==(const ReportKey&) const = default;
  };

  ChartSeries overlay_ = ChartSeries::None;
  ReportKey key_;
  bool viewValid_ = false;
  TimeView view_;
  // Decimation cache: recomputed only when the view, width or overlay changes.
  std::vector<ChartPoint> points_;
  TimeView pointsView_;
  int pointsColumns_ = -1;
  ChartSeries pointsOverlay_ = ChartSeries::None;
};

}  // namespace ft
