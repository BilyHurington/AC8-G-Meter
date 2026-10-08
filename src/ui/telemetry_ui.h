#pragma once
// Single entry point the host calls every frame, between ImGui::NewFrame() and
// ImGui::Render(). Hosts: apps/macos_proto (SDL3 + Metal) now, the UE4SS mod's
// DX11/DX12 Present hook later. Contains no platform or graphics-API code.
#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "config/settings_store.h"
#include "core/metrics.h"

struct ImFont;

namespace ft {

class IOverlayLayer;
class MissionArchive;
class MissionArchiveRecorder;

class TelemetryUi {
 public:
  explicit TelemetryUi(SettingsStore& store);
  ~TelemetryUi();

  // Call once after ImGui::CreateContext(), before the first frame. Loads the HUD font
  // (UTF-8 path; falls back to ImGui's default font if the file is missing). Returns false
  // on fallback.
  bool loadFonts(const std::string& hudFontPath);

  // Handles hotkeys and draws all layers. `now` = monotonic seconds (for auto-save debounce).
  void frame(const MetricsSnapshot& snapshot, float screenW, float screenH, double now);

  // True when the UI needs the mouse/keyboard: the menu or the mission history is open, edit
  // mode is on, or the mouse is over / interacting with the mission summary. Hosts forward input to the game
  // only when this is false. Normal overlay mode never captures input, and the auto-shown
  // summary does not block the game (e.g. the results screen) unless the mouse is on it.
  // Thread-safe: the value is computed at the end of frame() (render thread) and cached,
  // so an input hook (WndProc) on another thread can query it without touching ImGui.
  bool wantsInput() const { return wantsInput_.load(std::memory_order_relaxed); }

  bool menuOpen() const { return menuOpen_; }
  void setMenuOpen(bool open) { menuOpen_ = open; refreshWantsInput(); }
  bool editMode() const { return editMode_; }
  void setEditMode(bool on) { editMode_ = on; refreshWantsInput(); }
  bool summaryOpen() const { return summaryOpen_; }
  void showSummary(bool show) { summaryOpen_ = show; refreshWantsInput(); }
  // Opened deliberately (menu / summary button), so it captures input like the menu.
  bool historyOpen() const { return historyOpen_; }
  void showHistory(bool show) { historyOpen_ = show; refreshWantsInput(); }

  // Optional, not owned (must outlive this object or be reset to null first). With an archive
  // the mission history window is available and the summary shows the save status. Saving
  // itself is done by `recorder`, which the host registers with the pipeline (it must not
  // depend on the UI drawing frames); every frame the UI copies settings.history into it
  // (saveMissions -> recorder->setEnabled) and into the archive (maxSavedMissions ->
  // setMaxSaved), both atomics. Call on the render thread, between frames.
  void setMissionArchive(MissionArchive* archive, MissionArchiveRecorder* recorder = nullptr) {
    archive_ = archive;
    recorder_ = recorder;
  }

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
  bool historyOpen_ = false;
  MissionArchive* archive_ = nullptr;
  MissionArchiveRecorder* recorder_ = nullptr;
  bool imguiWantsMouse_ = false;  // io.WantCaptureMouse at the end of the last frame()
  std::atomic<bool> wantsInput_{false};

  void refreshWantsInput() {
    wantsInput_.store(menuOpen_ || editMode_ || historyOpen_ || (summaryOpen_ && imguiWantsMouse_),
                      std::memory_order_relaxed);
  }
};

}  // namespace ft
