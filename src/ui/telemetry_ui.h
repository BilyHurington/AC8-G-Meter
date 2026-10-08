#pragma once
// Single entry point the host calls every frame, between ImGui::NewFrame() and
// ImGui::Render(). Hosts: apps/macos_proto (SDL3 + Metal) now, the UE4SS mod's
// DX11/DX12 Present hook later. Contains no platform or graphics-API code.
#include <functional>
#include <memory>
#include <string>

#include "config/settings_store.h"
#include "core/metrics.h"

struct ImFont;

namespace ft {

class IOverlayLayer;

class TelemetryUi {
 public:
  explicit TelemetryUi(SettingsStore& store);
  ~TelemetryUi();

  // Call once after ImGui::CreateContext(), before the first frame. Loads the HUD font
  // (falls back to ImGui's default font if the file is missing). Returns false on fallback.
  bool loadFonts(const std::string& hudFontPath);

  // Handles hotkeys and draws all layers. `now` = monotonic seconds (for auto-save debounce).
  void frame(const MetricsSnapshot& snapshot, float screenW, float screenH, double now);

  // True when the UI needs the mouse/keyboard (menu open or edit mode). Hosts forward
  // input to the game only when this is false. Normal overlay mode never captures input.
  bool wantsInput() const;

  bool menuOpen() const { return menuOpen_; }
  void setMenuOpen(bool open) { menuOpen_ = open; }
  bool editMode() const { return editMode_; }
  void setEditMode(bool on) { editMode_ = on; }
  void showSummary(bool show) { summaryOpen_ = show; }

  // Invoked after any setting that affects the pipeline changes (apply via toPipelineConfig).
  std::function<void(const Settings&)> onSettingsChanged;

  // Additional layers drawn after the HUD (e.g. future physiology screen effects).
  void addLayer(std::unique_ptr<IOverlayLayer> layer);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  SettingsStore& store_;
  bool menuOpen_ = false;
  bool editMode_ = false;
  bool summaryOpen_ = false;
};

}  // namespace ft
