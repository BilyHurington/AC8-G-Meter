#pragma once
// The in-game settings window (toggled with Ctrl+F8 by default).
#include "config/settings_store.h"

namespace ft {

struct SettingsMenuResult {
  bool changed = false;          // any setting changed -> schedule auto-save
  bool pipelineChanged = false;  // a setting consumed by TelemetryPipeline changed
  bool showLastSummary = false;  // "Show last mission" clicked
};

class SettingsMenu {
 public:
  // Draws the window while `open` is true (the close button clears it).
  SettingsMenuResult draw(SettingsStore& store, bool& open, bool& editMode, bool hasLastReport,
                          double now);
  // True while waiting for a key press to rebind a hotkey; global hotkeys are suspended then.
  bool capturingHotkey() const { return capture_ != Capture::None; }

 private:
  enum class Capture { None, Menu, EditMode };
  void overlaySection(Settings& s, bool& editMode, SettingsMenuResult& r);
  void metricsSection(Settings& s, SettingsMenuResult& r);
  void hotkeySection(Settings& s, SettingsMenuResult& r);
  void hotkeyRow(const char* label, Hotkey& hotkey, Capture which, SettingsMenuResult& r);

  Capture capture_ = Capture::None;
};

}  // namespace ft
