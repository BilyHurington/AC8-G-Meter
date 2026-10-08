#pragma once
// The in-game settings window (toggled with Ctrl+F8 by default).
#include "config/settings_store.h"
#include "core/metrics.h"

namespace ft {

struct SettingsMenuResult {
  bool changed = false;          // any setting changed -> schedule auto-save
  bool pipelineChanged = false;  // a setting consumed by TelemetryPipeline changed
  bool showLastSummary = false;  // "Show last mission" clicked
  bool showHistory = false;      // "Mission history..." clicked
};

class SettingsMenu {
 public:
  // Draws the window while `open` is true (the close button clears it). `filter` is shown
  // read-only (the live adaptive filter windows). hasArchive: the host provides a
  // MissionArchive (mission history available).
  SettingsMenuResult draw(SettingsStore& store, bool& open, bool& editMode, bool hasLastReport, bool hasArchive,
                          const FilterState& filter, double now);
  // True while waiting for a key press to rebind a hotkey; global hotkeys are suspended then.
  bool capturingHotkey() const { return capture_ != Capture::None; }

 private:
  enum class Capture { None, Menu, EditMode };
  void overlaySection(Settings& s, bool& editMode, SettingsMenuResult& r);
  void metricsSection(Settings& s, const FilterState& filter, SettingsMenuResult& r);
  void hotkeySection(Settings& s, SettingsMenuResult& r);
  void missionSection(Settings& s, bool hasLastReport, bool hasArchive, SettingsMenuResult& r);
  void hotkeyRow(const char* label, Hotkey& hotkey, Capture which, SettingsMenuResult& r);

  Capture capture_ = Capture::None;
};

}  // namespace ft
