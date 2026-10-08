#pragma once
// Post-mission summary window (auto-opens when a mission ends, or from the settings menu and
// the mission history).
#include <cstdint>
#include <string>

#include "core/metrics.h"

struct ImFont;

namespace ft {

class GChart;

// What the host knows about the shown report beyond its contents.
struct SummaryContext {
  std::string subtitle;  // label / date line under the heading ("" = none)
  std::string status;    // archive status line ("" = none)
  enum class Tone : std::uint8_t { Info, Busy, Error };
  Tone statusTone = Tone::Info;
  bool canOpenHistory = false;  // show the "Mission history..." button
};

struct SummaryResult {
  bool openHistory = false;  // "Mission history..." clicked
};

// Draws the window while `open` is true. `titleFont` (may be null) is used for the heading.
// `chart` keeps the zoom / pan state of the G chart between frames.
SummaryResult drawMissionSummary(const MissionReport& report, const SummaryContext& context, GChart& chart,
                                 bool& open, ImFont* titleFont);

}  // namespace ft
