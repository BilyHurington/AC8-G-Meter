#pragma once
// Pure layout math for the overlay (no ImGui), unit-testable.
#include "config/settings.h"

namespace ft {

inline constexpr float kReferenceScreenHeight = 1080.0f;

struct OverlayLayout {
  float x = 0.0f, y = 0.0f;   // top-left in screen pixels
  float fontPx = 16.0f;
  float lineHeightPx = 21.0f;
};

// Resolution scale factor relative to 1080p (screenHeight / 1080).
float resolutionScale(float screenHeight);
OverlayLayout computeOverlayLayout(const OverlaySettings& s, float screenW, float screenH);
// Converts a dragged pixel position back to normalized settings coordinates, clamped so a
// box of size (boxW, boxH) stays fully on screen.
void storeOverlayPosition(OverlaySettings& s, float pixelX, float pixelY, float boxW, float boxH,
                          float screenW, float screenH);

}  // namespace ft
