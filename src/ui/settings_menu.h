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

// What the menu needs to know about the host's MissionArchive.
struct MissionArchiveInfo {
  bool available = false;  // the host provides one (mission history available)
  int savedMissions = 0;   // missions in its index
  bool counting = false;   // the folder was never listed yet: savedMissions is not known
};

// "Keep missions" changing from `currentLimit` to `newLimit` (0 = unlimited) with
// `savedMissions` on disk: the number of saved missions the new limit deletes at the next
// save beyond what the current limit already would, i.e. the number to confirm before
// applying. 0 = apply without asking (raising the limit, unlimited, or nothing over it).
int missionsDeletedByNewLimit(int currentLimit, int newLimit, int savedMissions);

class SettingsMenu {
 public:
  // Draws the window while `open` is true (the close button clears it). `filter` is shown
  // read-only (the live adaptive filter windows).
  SettingsMenuResult draw(SettingsStore& store, bool& open, bool& editMode, bool hasLastReport,
                          const MissionArchiveInfo& archive, const FilterState& filter, double now);
  // True while waiting for a key press to rebind a hotkey; global hotkeys are suspended then.
  bool capturingHotkey() const { return capture_ != Capture::None; }

 private:
  enum class Capture { None, Menu, EditMode };
  void overlaySection(Settings& s, bool& editMode, SettingsMenuResult& r);
  void metricsSection(Settings& s, const FilterState& filter, SettingsMenuResult& r);
  void hotkeySection(Settings& s, SettingsMenuResult& r);
  void missionSection(Settings& s, bool hasLastReport, const MissionArchiveInfo& archive, SettingsMenuResult& r);
  void keepMissionsInput(Settings& s, const MissionArchiveInfo& archive, SettingsMenuResult& r);
  void hotkeyRow(const char* label, Hotkey& hotkey, Capture which, SettingsMenuResult& r);

  Capture capture_ = Capture::None;
  // "Keep missions" value waiting for the user to confirm the deletion (-1 = none).
  int pendingKeep_ = -1;
  int pendingDeletes_ = 0;
};

}  // namespace ft
