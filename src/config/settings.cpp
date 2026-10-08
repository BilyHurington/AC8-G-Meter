#include "config/settings.h"

#include <algorithm>
#include <cmath>

#include "config/hotkey_names.h"

namespace ft {

const char* metricKey(MetricId id) {
  switch (id) {
    case MetricId::CurrentG: return "current_g";
    case MetricId::MaxG: return "max_g";
    case MetricId::MinG: return "min_g";
    case MetricId::OnsetRate: return "onset_rate";
    case MetricId::Aoa: return "aoa";
    case MetricId::SustainedHighG: return "sustained_high_g";
    case MetricId::Sideslip: return "sideslip";
    case MetricId::Speed: return "speed";
    case MetricId::Count: break;
  }
  return "unknown";
}

const char* metricLabel(MetricId id) {
  switch (id) {
    case MetricId::CurrentG: return "Current G";
    case MetricId::MaxG: return "Max G (mission)";
    case MetricId::MinG: return "Min G (mission)";
    case MetricId::OnsetRate: return "G onset rate (G/s)";
    case MetricId::Aoa: return "Angle of attack";
    case MetricId::SustainedHighG: return "Sustained high-G time";
    case MetricId::Sideslip: return "Sideslip angle";
    case MetricId::Speed: return "Speed";
    case MetricId::Count: break;
  }
  return "Unknown";
}

Color Color::fromHex(std::uint32_t rgb, float alpha) {
  return {static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
          static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
          static_cast<float>(rgb & 0xFF) / 255.0f, alpha};
}

std::string Hotkey::toString() const {
  std::string s;
  if (ctrl) s += "Ctrl+";
  if (shift) s += "Shift+";
  if (alt) s += "Alt+";
  return s + key;
}

namespace {

// NaN-safe clamp: non-finite values fall back to `fallback` before clamping.
float clampf(float v, float lo, float hi, float fallback) {
  if (!std::isfinite(v)) v = fallback;
  return std::clamp(v, lo, hi);
}

void clampColor(Color& c) {
  c.r = clampf(c.r, 0.0f, 1.0f, 1.0f);
  c.g = clampf(c.g, 0.0f, 1.0f, 1.0f);
  c.b = clampf(c.b, 0.0f, 1.0f, 1.0f);
  c.a = 1.0f;  // text color alpha is not used; opacity is a separate setting
}

}  // namespace

// Valid ranges:
//   overlay.posX/posY       [0, 1]     fraction of the screen
//   overlay.fontSizeAt1080p [8, 64]    px at 1080p
//   overlay.lineSpacing     [1.0, 2.5] multiple of the font size
//   opacity / alphas        [0, 1]
//   metrics.smoothingTauS   [0, 1]     s
//   metrics.highGEnter      [2, 12]    G
//   metrics.highGExit       [1, highGEnter - 0.1] G (hysteresis must stay positive)
//   metrics.minExcursionS   [0, 0.5]   s (0 = off)
//   history.maxSavedMissions [0, 1000] (0 = unlimited)
//   hotkeys                 unknown key name -> default binding
void Settings::clamp() {
  const OverlaySettings od;
  overlay.posX = clampf(overlay.posX, 0.0f, 1.0f, od.posX);
  overlay.posY = clampf(overlay.posY, 0.0f, 1.0f, od.posY);
  overlay.fontSizeAt1080p = clampf(overlay.fontSizeAt1080p, 8.0f, 64.0f, od.fontSizeAt1080p);
  overlay.lineSpacing = clampf(overlay.lineSpacing, 1.0f, 2.5f, od.lineSpacing);
  clampColor(overlay.textColor);
  overlay.opacity = clampf(overlay.opacity, 0.0f, 1.0f, od.opacity);
  overlay.shadowAlpha = clampf(overlay.shadowAlpha, 0.0f, 1.0f, od.shadowAlpha);
  overlay.backgroundAlpha = clampf(overlay.backgroundAlpha, 0.0f, 1.0f, od.backgroundAlpha);

  const MetricsSettings md;
  if (metrics.gMode != GMode::Nz && metrics.gMode != GMode::Total) metrics.gMode = md.gMode;
  metrics.smoothingTauS = clampf(metrics.smoothingTauS, 0.0f, 1.0f, md.smoothingTauS);
  metrics.highGEnter = clampf(metrics.highGEnter, 2.0f, 12.0f, md.highGEnter);
  metrics.highGExit = clampf(metrics.highGExit, 1.0f, metrics.highGEnter - 0.1f, md.highGExit);
  metrics.minExcursionS = clampf(metrics.minExcursionS, 0.0f, 0.5f, md.minExcursionS);

  history.maxSavedMissions = std::clamp(history.maxSavedMissions, 0, 1000);

  const HotkeySettings hd;
  if (!isKnownHotkeyName(hotkeys.toggleMenu.key)) hotkeys.toggleMenu = hd.toggleMenu;
  if (!isKnownHotkeyName(hotkeys.toggleEditMode.key)) hotkeys.toggleEditMode = hd.toggleEditMode;

  version = kVersion;
}

PipelineConfig Settings::toPipelineConfig(PipelineConfig base) const {
  base.gMode = metrics.gMode;
  base.adaptiveFiltering = metrics.adaptiveFiltering;
  base.smoothingTauS = metrics.smoothingTauS;
  base.highGEnter = metrics.highGEnter;
  base.highGExit = metrics.highGExit;
  base.minExcursionS = metrics.minExcursionS;
  return base;
}

}  // namespace ft
