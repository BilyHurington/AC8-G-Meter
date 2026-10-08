#include "ui/telemetry_ui.h"

#include <optional>
#include <vector>

#include <imgui.h>

#include "storage/mission_archive.h"
#include "ui/format.h"
#include "ui/g_chart.h"
#include "ui/hotkey_input.h"
#include "ui/hud_overlay.h"
#include "ui/mission_history.h"
#include "ui/mission_summary.h"
#include "ui/overlay_layer.h"
#include "ui/settings_menu.h"

namespace ft {

struct TelemetryUi::Impl {
  HudOverlayLayer* hud = nullptr;                      // owned by `layers` (always index 0)
  std::vector<std::unique_ptr<IOverlayLayer>> layers;  // drawn in order, HUD first
  SettingsMenu menu;
  ImFont* hudFont = nullptr;
  std::shared_ptr<const MissionReport> summaryReport;  // report shown in the summary window
  std::optional<MissionIndexEntry> summaryEntry;       // set when it was opened from the history
  std::uint32_t lastSeenReportId = 0;
  GChart chart;
  MissionHistoryWindow history;
};

namespace {

// Heading line and archive status for the summary window.
SummaryContext summaryContext(const MissionReport& report, const std::optional<MissionIndexEntry>& entry,
                              const std::shared_ptr<const MissionReport>& shown, MissionArchive* archive,
                              const Settings& settings) {
  SummaryContext c;
  c.canOpenHistory = archive != nullptr;
  if (entry) {
    c.subtitle = (report.label.empty() ? std::string() : report.label + "   ") + "saved " +
                 formatLocalDateTime(entry->savedAtUnix);
    c.status = "From the mission history: " + pathToUtf8(entry->path.filename());
    return c;
  }
  c.subtitle = report.label;
  if (!archive) return c;
  const SaveStatus st = archive->lastSave();
  if (st.report == shown) {
    switch (st.state) {
      case SaveStatus::State::Pending:
        c.status = "Saving...";
        c.statusTone = SummaryContext::Tone::Busy;
        break;
      case SaveStatus::State::Saved:
        c.status = "Saved to " + pathToUtf8(st.path);
        break;
      case SaveStatus::State::Failed:
        c.status = "Save failed: " + st.error;
        c.statusTone = SummaryContext::Tone::Error;
        break;
      case SaveStatus::State::None:
        break;
    }
  } else if (!settings.history.saveMissions) {
    c.status = "Not saved (saving missions is off in the settings menu).";
  } else if (!report.stats.hasGData) {
    c.status = "Not saved (no G data).";
  }
  return c;
}

}  // namespace

TelemetryUi::TelemetryUi(SettingsStore& store) : impl_(std::make_unique<Impl>()), store_(store) {
  auto hud = std::make_unique<HudOverlayLayer>();
  impl_->hud = hud.get();
  impl_->layers.push_back(std::move(hud));
}

TelemetryUi::~TelemetryUi() = default;

bool TelemetryUi::loadFonts(const std::string& hudFontPath) {
  ImGuiIO& io = ImGui::GetIO();
  // Menus keep ImGui's default font; only the overlay uses the HUD font. Adding the default
  // first also makes it io.FontDefault-equivalent (the first font is the default).
  if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();
  ImFontConfig cfg;
  cfg.Flags |= ImFontFlags_NoLoadError;  // a missing file is handled by the fallback below
  // Size 0: ImGui 1.92 rasterizes glyphs on demand at whatever size the overlay asks for.
  impl_->hudFont = io.Fonts->AddFontFromFileTTF(hudFontPath.c_str(), 0.0f, &cfg);
  impl_->hud->setFont(impl_->hudFont);
  return impl_->hudFont != nullptr;
}

void TelemetryUi::addLayer(std::unique_ptr<IOverlayLayer> layer) {
  if (layer) impl_->layers.push_back(std::move(layer));
}

void TelemetryUi::frame(const MetricsSnapshot& snapshot, float screenW, float screenH, double now) {
  Impl& d = *impl_;
  Settings& settings = store_.settings();

  // Hotkeys (suspended while the menu is capturing a new binding).
  if (!d.menu.capturingHotkey()) {
    if (isHotkeyPressed(settings.hotkeys.toggleMenu)) menuOpen_ = !menuOpen_;
    else if (isHotkeyPressed(settings.hotkeys.toggleEditMode)) editMode_ = !editMode_;
  }

  // A new mission report arrived: archive it, and pop the summary if enabled. Aborted missions
  // (left before the results screen) are saved but not shown automatically; "Show last
  // mission" still opens them.
  if (snapshot.lastReport && snapshot.lastReport->id != d.lastSeenReportId) {
    d.lastSeenReportId = snapshot.lastReport->id;
    if (archive_ && settings.history.saveMissions && snapshot.lastReport->stats.hasGData) {
      archive_->setMaxSaved(settings.history.maxSavedMissions);
      archive_->saveAsync(snapshot.lastReport);
    }
    if (!snapshot.lastReport->aborted) {
      d.summaryReport = snapshot.lastReport;
      d.summaryEntry.reset();
      if (settings.summary.autoShow) summaryOpen_ = true;
    }
  }
  if (summaryOpen_ && !d.summaryReport) {
    d.summaryReport = snapshot.lastReport;
    d.summaryEntry.reset();
  }

  bool changed = false;
  OverlayFrame overlayFrame{snapshot, settings, screenW, screenH, now, editMode_};
  for (auto& layer : d.layers) changed |= layer->draw(overlayFrame);

  bool pipelineChanged = false;
  if (menuOpen_) {
    const SettingsMenuResult r = d.menu.draw(store_, menuOpen_, editMode_, snapshot.lastReport != nullptr,
                                             archive_ != nullptr, snapshot.filter, now);
    changed |= r.changed;
    pipelineChanged = r.pipelineChanged;
    if (r.showLastSummary && snapshot.lastReport) {
      d.summaryReport = snapshot.lastReport;
      d.summaryEntry.reset();
      summaryOpen_ = true;
    }
    if (r.showHistory) historyOpen_ = true;
  }

  if (archive_) {
    MissionHistoryWindow::Result h = d.history.draw(*archive_, historyOpen_);
    if (h.report) {
      d.summaryReport = std::move(h.report);
      d.summaryEntry = std::move(h.entry);
      summaryOpen_ = true;
      ImGui::SetWindowFocus("###ft_summary");  // in front of the history window
    }
  } else {
    historyOpen_ = false;
  }

  if (summaryOpen_ && d.summaryReport) {
    const SummaryContext context =
        summaryContext(*d.summaryReport, d.summaryEntry, d.summaryReport, archive_, settings);
    if (drawMissionSummary(*d.summaryReport, context, d.chart, summaryOpen_, d.hudFont).openHistory)
      historyOpen_ = true;
  } else {
    summaryOpen_ = false;
  }

  if (changed) store_.markDirty(now);
  if (pipelineChanged && onSettingsChanged) onSettingsChanged(store_.settings());
  store_.update(now);

  imguiWantsMouse_ = ImGui::GetIO().WantCaptureMouse;
  refreshWantsInput();
}

}  // namespace ft
