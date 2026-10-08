#include "ui/layout.h"

#include <algorithm>

namespace ft {

float resolutionScale(float screenHeight) {
  return screenHeight > 0.0f ? screenHeight / kReferenceScreenHeight : 1.0f;
}

OverlayLayout computeOverlayLayout(const OverlaySettings& s, float screenW, float screenH) {
  OverlayLayout l;
  l.x = s.posX * screenW;
  l.y = s.posY * screenH;
  l.fontPx = s.fontSizeAt1080p * resolutionScale(screenH);
  l.lineHeightPx = l.fontPx * s.lineSpacing;
  return l;
}

void storeOverlayPosition(OverlaySettings& s, float pixelX, float pixelY, float boxW, float boxH,
                          float screenW, float screenH) {
  if (screenW <= 0.0f || screenH <= 0.0f) return;
  // A box larger than the screen pins to the top-left edge.
  const float maxX = std::max(0.0f, screenW - boxW);
  const float maxY = std::max(0.0f, screenH - boxH);
  s.posX = std::clamp(pixelX, 0.0f, maxX) / screenW;
  s.posY = std::clamp(pixelY, 0.0f, maxY) / screenH;
}

}  // namespace ft
