#pragma once
// Post-mission summary window (auto-opens when a mission ends, or from the settings menu).
#include "config/settings.h"
#include "core/metrics.h"

struct ImFont;

namespace ft {

// Draws the window while `open` is true. `titleFont` (may be null) is used for the heading.
void drawMissionSummary(const MissionReport& report, const Settings& settings, bool& open,
                        ImFont* titleFont);

}  // namespace ft
