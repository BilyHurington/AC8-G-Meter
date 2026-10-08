#pragma once
// User settings (persisted as JSON). No ImGui or platform dependencies.
// Sections are independent structs so new features (e.g. a future `physiology`
// section for blackout / redout effects) can be added without touching the others.
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>

#include "core/pipeline.h"

namespace ft {

// Overlay rows in display order. Append new metrics at the end (before Count);
// the JSON stores them by key (see metricKey), so reordering is safe too.
enum class MetricId : std::uint8_t {
  CurrentG,
  MaxG,
  MinG,
  OnsetRate,
  Aoa,
  SustainedHighG,
  Sideslip,
  Speed,
  Count
};
inline constexpr int kMetricCount = static_cast<int>(MetricId::Count);
const char* metricKey(MetricId id);    // stable JSON key, e.g. "current_g"
const char* metricLabel(MetricId id);  // human readable, e.g. "Current G"

struct Color {
  float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
  static Color fromHex(std::uint32_t rgb, float alpha = 1.0f);
  bool operator==(const Color&) const = default;
};

// Metrics shown on a fresh install: current, max and min G. Indexed by MetricId, so it stays
// correct when metrics are added.
constexpr std::array<bool, kMetricCount> defaultVisibleMetrics() {
  std::array<bool, kMetricCount> v{};
  for (MetricId id : {MetricId::CurrentG, MetricId::MaxG, MetricId::MinG})
    v[static_cast<std::size_t>(id)] = true;
  return v;
}

struct OverlaySettings {
  float posX = 0.045f;            // top-left corner, fraction of screen width
  float posY = 0.16f;             // top-left corner, fraction of screen height
  bool locked = false;            // when locked, edit mode cannot drag the overlay
  float fontSizeAt1080p = 16.0f;  // px at 1080p; scaled by screenHeight / 1080
  float lineSpacing = 1.3125f;    // line height = font size * lineSpacing (21 px at 16 px)
  Color textColor = Color::fromHex(0x93F6A8);
  float opacity = 1.0f;           // multiplies text, shadow and background alpha
  bool shadow = true;
  float shadowAlpha = 0.55f;
  bool background = false;
  float backgroundAlpha = 0.35f;
  std::array<bool, kMetricCount> visible = defaultVisibleMetrics();

  bool isVisible(MetricId id) const { return visible[static_cast<int>(id)]; }
  bool operator==(const OverlaySettings&) const = default;
};

struct MetricsSettings {
  GMode gMode = GMode::Nz;
  float smoothingTauS = 0.05f;  // keep equal to PipelineConfig::smoothingTauS
  float highGEnter = 6.0f;
  float highGExit = 5.5f;
  bool operator==(const MetricsSettings&) const = default;
};

struct Hotkey {
  std::string key = "F8";  // ImGuiKey name without prefix: "F1".."F12", "A".."Z", "Insert", ...
  bool ctrl = true;
  bool shift = false;
  bool alt = false;
  std::string toString() const;  // "Ctrl+F8"
  bool operator==(const Hotkey&) const = default;
};

struct HotkeySettings {
  Hotkey toggleMenu{"F8", true, false, false};
  Hotkey toggleEditMode{"F9", true, false, false};
  bool operator==(const HotkeySettings&) const = default;
};

struct SummarySettings {
  bool autoShow = true;  // pop up the mission summary when a mission ends
  bool operator==(const SummarySettings&) const = default;
};

struct Settings {
  static constexpr int kVersion = 1;
  int version = kVersion;
  OverlaySettings overlay;
  MetricsSettings metrics;
  HotkeySettings hotkeys;
  SummarySettings summary;

  // Forces every field into its valid range (ranges documented in settings.cpp).
  void clamp();
  // Applies the user-tunable parts onto a base pipeline configuration.
  PipelineConfig toPipelineConfig(PipelineConfig base = {}) const;
  bool operator==(const Settings&) const = default;
};

}  // namespace ft
