// Prototype-only debug panel (Ctrl+F1): source control, glitch injection, recording,
// replay, and live accuracy readouts. Not part of ft_ui; never shipped in the mod.
#include <cfloat>
#include <cmath>
#include <iterator>

#include <imgui.h>

#include "proto_app.h"
#include "ui/layout.h"

namespace ft {
namespace {

constexpr ImVec4 kAccent{0.576f, 0.965f, 0.659f, 1.0f};
constexpr ImVec4 kWarn{1.0f, 0.62f, 0.35f, 1.0f};

void readoutRow(const char* name, double truth, bool hasTruth, double estimate, bool estValid,
                const char* fmt) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::TextUnformatted(name);
  ImGui::TableNextColumn();
  if (hasTruth) ImGui::Text(fmt, truth);
  else ImGui::TextDisabled("n/a");
  ImGui::TableNextColumn();
  if (estValid) ImGui::Text(fmt, estimate);
  else ImGui::TextDisabled("--");
  ImGui::TableNextColumn();
  if (hasTruth && estValid) {
    const double err = estimate - truth;
    ImGui::TextColored(std::fabs(err) > 0.3 ? kWarn : kAccent, "%+.2f", err);
  } else {
    ImGui::TextDisabled("--");
  }
}

}  // namespace

void ProtoApp::drawDebugPanel(float screenW, float screenH) {
  const float fs = ImGui::GetFontSize();
  ImGui::SetNextWindowPos(ImVec2(screenW - 10.0f, 10.0f), ImGuiCond_FirstUseEver, ImVec2(1.0f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(fs * 25.0f, screenH - 20.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Prototype debug  (Ctrl+F1)###ft_proto_debug", &debugOpen_, ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::End();
    return;
  }
  ImGui::PushItemWidth(-fs * 8.5f);  // leave room for the labels on the right
  ImGui::TextDisabled("%.0f FPS   |   overlay wants input: %s", static_cast<double>(ImGui::GetIO().Framerate),
                      ui_.wantsInput() ? "yes" : "no");

  // --- Source -----------------------------------------------------------------------
  ImGui::SeparatorText("Source");
  int kind = sourceKind_ == SourceKind::Replay ? 1 : 0;
  bool switched = ImGui::RadioButton("Simulator", &kind, 0);
  ImGui::SameLine();
  ImGui::BeginDisabled(replay_.rows().empty());
  switched |= ImGui::RadioButton("Replay", &kind, 1);
  ImGui::EndDisabled();
  if (switched && kind != (sourceKind_ == SourceKind::Replay ? 1 : 0)) {
    sourceKind_ = kind == 1 ? SourceKind::Replay : SourceKind::Simulator;
    restartSource();
  }
  ImGui::SameLine();
  if (ImGui::Button("Restart")) restartSource();

  const bool isSim = sourceKind_ == SourceKind::Simulator;
  float timeScale = static_cast<float>(isSim ? sim_.timeScale() : replay_.timeScale());
  if (ImGui::SliderFloat("Time scale", &timeScale, 0.1f, 8.0f, "%.2fx", ImGuiSliderFlags_Logarithmic)) {
    sim_.setTimeScale(timeScale);
    replay_.setTimeScale(timeScale);
  }
  bool paused = isSim ? sim_.paused() : replay_.paused();
  if (ImGui::Checkbox("Pause", &paused)) {
    if (isSim) sim_.setPaused(paused);
    else replay_.setPaused(paused);
  }

  if (isSim) simulatorControls();
  else replayControls();

  // --- Replay files (load from either source mode) -------------------------------------
  if (ImGui::CollapsingHeader("Replay files")) {
    if (replayFiles_.empty()) ImGui::TextDisabled("No .csv files in the replay folders.");
    if (ImGui::BeginListBox("##replays", ImVec2(-FLT_MIN, fs * 6.0f))) {
      for (int i = 0; i < static_cast<int>(replayFiles_.size()); ++i) {
        const std::filesystem::path& file = replayFiles_[static_cast<std::size_t>(i)];
        if (ImGui::Selectable(pathToUtf8(file.filename()).c_str(), replayIndex_ == i)) replayIndex_ = i;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", pathToUtf8(file).c_str());
      }
      ImGui::EndListBox();
    }
    ImGui::BeginDisabled(replayIndex_ < 0);
    if (ImGui::Button("Load and play")) {
      std::string error;
      const std::string path = replayFiles_[static_cast<std::size_t>(replayIndex_)].string();
      if (replay_.load(path, &error)) {
        replayStatus_ = "Loaded " + std::to_string(replay_.rows().size()) + " rows";
        sourceKind_ = SourceKind::Replay;
        restartSource();
      } else {
        replayStatus_ = "Load failed: " + error;
      }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) rescanReplays();
    if (!replayStatus_.empty()) ImGui::TextWrapped("%s", replayStatus_.c_str());
  }

  // --- Recording --------------------------------------------------------------------
  ImGui::SeparatorText("Recording");
  bool recording = recorder_.isOpen();
  if (ImGui::Checkbox("Record to CSV", &recording)) setRecording(recording);
  if (recorder_.isOpen()) {
    ImGui::SameLine();
    ImGui::TextColored(kWarn, "REC  %zu rows", recorder_.rowsWritten());
    ImGui::TextWrapped("%s", recorder_.path().c_str());
  } else if (!recordStatus_.empty()) {
    ImGui::TextWrapped("%s", recordStatus_.c_str());
  } else {
    ImGui::TextDisabled("Writes to %s", pathToUtf8(config_.recordDir).c_str());
  }

  readouts();

  // --- Overlay geometry -----------------------------------------------------------------
  ImGui::SeparatorText("Screen");
  static const int kRes[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}};
  char current[32];
  std::snprintf(current, sizeof current, "%.0f x %.0f", static_cast<double>(screenW), static_cast<double>(screenH));
  if (ImGui::BeginCombo("Virtual resolution", current)) {
    for (const auto& r : kRes) {
      char label[32];
      std::snprintf(label, sizeof label, "%d x %d", r[0], r[1]);
      if (ImGui::Selectable(label, static_cast<int>(screenW) == r[0] && static_cast<int>(screenH) == r[1]))
        resizeRequest_ = std::make_pair(r[0], r[1]);
    }
    ImGui::EndCombo();
  }
  const OverlayLayout l = computeOverlayLayout(store_.settings().overlay, screenW, screenH);
  const ImVec2 fb = ImGui::GetIO().DisplayFramebufferScale;
  ImGui::Text("Overlay at (%.1f, %.1f)", static_cast<double>(l.x), static_cast<double>(l.y));
  ImGui::Text("Font %.1f px, line %.1f px", static_cast<double>(l.fontPx), static_cast<double>(l.lineHeightPx));
  ImGui::TextDisabled("Framebuffer %.0f x %.0f px (scale %.1f)", static_cast<double>(screenW * fb.x),
                      static_cast<double>(screenH * fb.y), static_cast<double>(fb.x));
  const HotkeySettings& hk = store_.settings().hotkeys;
  ImGui::TextDisabled("%s settings   %s edit layout", hk.toggleMenu.toString().c_str(),
                      hk.toggleEditMode.toString().c_str());

  ImGui::PopItemWidth();
  ImGui::End();
}

void ProtoApp::simulatorControls() {
  ImGui::SeparatorText("Simulator");
  SimulatedSourceOptions opts = sim_.options();
  bool changed = false;

  int scenario = static_cast<int>(opts.scenario);
  if (ImGui::Combo("Scenario", &scenario,
                   [](void*, int i) { return scenarioName(static_cast<Scenario>(i)); }, nullptr,
                   static_cast<int>(Scenario::Count))) {
    opts.scenario = static_cast<Scenario>(scenario);
    changed = true;
  }
  if (opts.scenario == Scenario::Manual) {
    ImGui::TextColored(kAccent, "W/Up pull (to +9 G)  S/Down push (to -3 G)");
    ImGui::TextColored(kAccent, "A/D or Left/Right roll   Shift/Z throttle");
    ImGui::TextDisabled("nz cmd %+.1f G   roll %+.0f \xC2\xB0/s   throttle %.0f%%", manual_.nzCmd,
                        manual_.rollRateDegS, manual_.throttle * 100.0);
  }

  ImGui::Text("Mission phase: %s", toString(sim_.missionPhase()));
  if (ImGui::Button("Start mission")) sim_.startMission();
  ImGui::SameLine();
  if (ImGui::Button("End mission")) sim_.endMission();
  ImGui::SameLine();
  if (ImGui::Button("Quit to menu")) sim_.quitToMenu();

  ImGui::TextUnformatted("Glitches:");
  ImGui::SameLine();
  if (ImGui::SmallButton("Spike")) sim_.injectSpike();
  ImGui::SameLine();
  if (ImGui::SmallButton("Hitch 0.5 s")) sim_.injectHitch(0.5);
  ImGui::SameLine();
  if (ImGui::SmallButton("Teleport")) sim_.injectTeleport();

  // Dirty-data toggles with representative magnitudes for a real game hook.
  DirtyDataOptions& d = opts.dirty;
  bool noise = d.positionNoiseM > 0.0 || d.velocityNoiseMS > 0.0;
  bool drop = d.dropProbability > 0.0, dup = d.duplicateProbability > 0.0, jitter = d.frameJitter > 0.0;
  if (ImGui::Checkbox("Noise", &noise)) {
    d.positionNoiseM = noise ? 0.25 : 0.0;
    d.velocityNoiseMS = noise ? 0.25 : 0.0;
    changed = true;
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("Drops", &drop)) { d.dropProbability = drop ? 0.05 : 0.0; changed = true; }
  ImGui::SameLine();
  if (ImGui::Checkbox("Duplicates", &dup)) { d.duplicateProbability = dup ? 0.03 : 0.0; changed = true; }
  ImGui::SameLine();
  if (ImGui::Checkbox("Jitter", &jitter)) { d.frameJitter = jitter ? 0.15 : 0.0; changed = true; }
  changed |= ImGui::Checkbox("Provide velocity", &opts.provideVelocity);

  static const double kRates[] = {30.0, 60.0, 120.0, 144.0};
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
  char rateLabel[16];
  std::snprintf(rateLabel, sizeof rateLabel, "%.0f Hz", opts.frameRateHz);
  if (ImGui::BeginCombo("Game FPS", rateLabel)) {
    for (double r : kRates) {
      char label[16];
      std::snprintf(label, sizeof label, "%.0f Hz", r);
      if (ImGui::Selectable(label, opts.frameRateHz == r)) {
        opts.frameRateHz = r;
        changed = true;
      }
    }
    ImGui::EndCombo();
  }

  if (changed) {
    const bool restarts = opts.scenario != sim_.options().scenario || opts.seed != sim_.options().seed;
    sim_.setOptions(opts);
    simOptions_ = opts;
    if (restarts) {
      pipeline_.reset();
      manual_ = ControlInput{};
    }
  }
}

void ProtoApp::replayControls() {
  ImGui::SeparatorText("Replay");
  ImGui::TextWrapped("%s", replay_.path().empty() ? "(in-memory log)" : replay_.path().c_str());
  char overlay[48];
  std::snprintf(overlay, sizeof overlay, "%.1f / %.1f s", replay_.progress() * replay_.durationS(),
                replay_.durationS());
  ImGui::ProgressBar(static_cast<float>(replay_.progress()), ImVec2(-FLT_MIN, 0.0f), overlay);
  bool loop = replay_.loop();
  if (ImGui::Checkbox("Loop", &loop)) replay_.setLoop(loop);
  if (replay_.finished()) {
    ImGui::SameLine();
    ImGui::TextDisabled("finished");
  }
}

void ProtoApp::readouts() {
  const MetricsSnapshot snap = latest_.read();
  const FlightMetrics& m = snap.current;
  const bool isSim = sourceKind_ == SourceKind::Simulator;
  const bool totalMode = pipeline_.config().gMode == GMode::Total;

  bool hasNz = false, hasAoa = false;
  double trueG = 0.0, trueAoa = 0.0;
  if (isSim) {
    hasNz = hasAoa = true;
    trueG = totalMode ? sim_.truth().trueGTotal : sim_.truth().trueNz;
    trueAoa = sim_.truth().trueAoaDeg;
  } else if (const CsvRow* row = replay_.lastEmitted()) {
    hasNz = row->trueNz.has_value() && !totalMode;
    hasAoa = row->trueAoaDeg.has_value();
    trueG = row->trueNz.value_or(0.0);
    trueAoa = row->trueAoaDeg.value_or(0.0);
  }

  ImGui::SeparatorText("Truth vs estimate");
  if (ImGui::BeginTable("##readouts", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("");
    ImGui::TableSetupColumn("truth");
    ImGui::TableSetupColumn("estimate");
    ImGui::TableSetupColumn("error");
    ImGui::TableHeadersRow();
    readoutRow(totalMode ? "G total" : "Nz", trueG, hasNz, m.g, m.valid, "%+.2f");
    readoutRow("AoA \xC2\xB0", trueAoa, hasAoa, m.aoaDeg, m.valid && m.aoaValid, "%+.1f");
    ImGui::EndTable();
  }
  ImGui::Text("raw %+.2f  onset %+.1f G/s  speed %.0f km/h", m.gRaw, m.onsetRate, m.speed * 3.6);
  ImGui::Text("phase %s  high-G %s %.1f s", toString(snap.phase), m.inHighG ? "ON" : "off", m.highGDurationS);

  const DataQuality& q = snap.quality;
  ImGui::SeparatorText("Data quality");
  ImGui::Text("accepted %llu", static_cast<unsigned long long>(q.accepted));
  ImGui::Text("rejected: non-finite %llu  time %llu", static_cast<unsigned long long>(q.rejectedNonFinite),
              static_cast<unsigned long long>(q.rejectedTime));
  ImGui::Text("          spike %llu  (extreme G %llu)", static_cast<unsigned long long>(q.rejectedSpike),
              static_cast<unsigned long long>(q.extremeG));
  ImGui::Text("resets: gap %llu  teleport %llu", static_cast<unsigned long long>(q.resetsGap),
              static_cast<unsigned long long>(q.resetsTeleport));
  const FilterState& f = snap.filter;
  ImGui::Text("input %.0f Hz  velocity noise %.3f m/s", f.sampleRateHz, f.velocityNoiseMS);
  ImGui::Text("filter %s: window %.1f ms  tau %.1f ms  onset %.0f ms", f.adaptive ? "adaptive" : "fixed",
              f.accelWindowS * 1000.0, f.smoothingTauS * 1000.0, f.onsetWindowS * 1000.0);
  if (snap.lastReport) ImGui::TextDisabled("last mission report: #%u", snap.lastReport->id);
}

}  // namespace ft
