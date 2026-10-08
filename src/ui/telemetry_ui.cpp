#include "ui/telemetry_ui.h"

#include <vector>

#include <imgui.h>

#include "ui/hotkey_input.h"
#include "ui/hud_overlay.h"
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
  std::uint32_t lastSeenReportId = 0;
};

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

  // A new mission report arrived: pop the summary if enabled. Aborted missions (left before
  // the results screen) are not shown automatically; "Show last mission" still opens them.
  if (snapshot.lastReport && snapshot.lastReport->id != d.lastSeenReportId) {
    d.lastSeenReportId = snapshot.lastReport->id;
    if (!snapshot.lastReport->aborted) {
      d.summaryReport = snapshot.lastReport;
      if (settings.summary.autoShow) summaryOpen_ = true;
    }
  }
  if (summaryOpen_ && !d.summaryReport) d.summaryReport = snapshot.lastReport;

  bool changed = false;
  OverlayFrame overlayFrame{snapshot, settings, screenW, screenH, now, editMode_};
  for (auto& layer : d.layers) changed |= layer->draw(overlayFrame);

  bool pipelineChanged = false;
  if (menuOpen_) {
    const SettingsMenuResult r =
        d.menu.draw(store_, menuOpen_, editMode_, snapshot.lastReport != nullptr, snapshot.filter, now);
    changed |= r.changed;
    pipelineChanged = r.pipelineChanged;
    if (r.showLastSummary && snapshot.lastReport) {
      d.summaryReport = snapshot.lastReport;
      summaryOpen_ = true;
    }
  }

  if (summaryOpen_ && d.summaryReport)
    drawMissionSummary(*d.summaryReport, settings, summaryOpen_, d.hudFont);
  else
    summaryOpen_ = false;

  if (changed) store_.markDirty(now);
  if (pipelineChanged && onSettingsChanged) onSettingsChanged(store_.settings());
  store_.update(now);

  imguiWantsMouse_ = ImGui::GetIO().WantCaptureMouse;
  refreshWantsInput();
}

}  // namespace ft
