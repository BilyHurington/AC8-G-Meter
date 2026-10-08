#include "proto_app.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

#include <imgui.h>

#include "ui/hotkey_input.h"
#include "ui/style.h"

namespace ft {

ProtoApp::ProtoApp(ProtoConfig config)
    : config_(std::move(config)),
      store_(config_.settingsPath),
      archive_(config_.missionsDir),
      missionRecorder_(archive_),
      ui_(store_) {
  store_.load();
  // From here on the UI keeps both in step with the settings (atomics, render thread).
  archive_.setMaxSaved(store_.settings().history.maxSavedMissions);
  missionRecorder_.setEnabled(store_.settings().history.saveMissions);
  ui_.setMissionArchive(&archive_, &missionRecorder_);
  pipeline_.addConsumer(&missionRecorder_);
  if (!store_.lastError().empty())
    std::fprintf(stderr, "settings: %s (using defaults)\n", store_.lastError().c_str());
  pipeline_.setConfig(store_.settings().toPipelineConfig(baseConfig_));
  // Mirrors the in-game threading: the UI (render thread) only publishes the new config;
  // update() (game thread) applies it before pushing the next samples.
  ui_.onSettingsChanged = [this](const Settings& s) {
    pendingConfig_.publish(s.toPipelineConfig(baseConfig_));
  };

  if (config_.scenario) simOptions_.scenario = *config_.scenario;
  if (config_.velocityNoiseMS > 0.0) simOptions_.dirty.velocityNoiseMS = config_.velocityNoiseMS;
  sim_.setOptions(simOptions_);
  sim_.restart();
  ui_.setMenuOpen(config_.openMenu);
  ui_.showHistory(config_.openHistory);
  ui_.setEditMode(config_.editMode);
  debugOpen_ = config_.debugPanel;
  rescanReplays();
}

ProtoApp::~ProtoApp() { shutdown(); }

void ProtoApp::initImGui() {
  applyTelemetryStyle(ImGui::GetStyle());
  const auto fonts = config_.assetsDir / "fonts";
  if (!ui_.loadFonts(pathToUtf8(fonts / "B612Mono-Regular.ttf")))
    std::fprintf(stderr, "HUD font not found in %s, using ImGui's default font\n", pathToUtf8(fonts).c_str());
  // The mock game HUD uses the bold cut so it is distinguishable from the telemetry overlay.
  ImFontConfig cfg;
  cfg.Flags |= ImFontFlags_NoLoadError;
  scene_.setFont(ImGui::GetIO().Fonts->AddFontFromFileTTF(pathToUtf8(fonts / "B612Mono-Bold.ttf").c_str(), 0.0f, &cfg));
}

ITelemetrySource& ProtoApp::source() {
  if (sourceKind_ == SourceKind::Replay) return replay_;
  return sim_;
}

void ProtoApp::update(double realDtS) {
  samples_.clear();
  const bool isSim = sourceKind_ == SourceKind::Simulator;
  if (isSim && sim_.options().scenario == Scenario::Manual) sim_.setManualInput(manual_);
  source().poll(realDtS, samples_);

  // The mission label is game-side knowledge (the mod will use the map name).
  if (std::string label = missionLabel(); label != appliedLabel_) {
    pipeline_.setMissionLabel(label);
    appliedLabel_ = std::move(label);
  }

  // Apply settings published by the UI since the last update (UI -> game-thread hand-off).
  if (PipelineConfig config; pendingConfig_.readIfNewer(appliedConfigVersion_, config))
    pipeline_.setConfig(config);

  for (std::size_t i = 0; i < samples_.size(); ++i) {
    const RawFlightSample& s = samples_[i];
    pipeline_.push(s);
    if (recorder_.isOpen()) {
      CsvRow row{s, std::nullopt, std::nullopt};
      // Exact truth is only known for the newest simulator state, i.e. the last sample.
      if (isSim && i + 1 == samples_.size()) {
        row.trueNz = sim_.truth().trueNz;
        row.trueAoaDeg = sim_.truth().trueAoaDeg;
      }
      recorder_.write(row);
    }
  }
  if (!samples_.empty()) simTimeS_ = samples_.back().t;
  if (isSim && config_.endMissionAtS >= 0.0 && !endMissionDone_ && simTimeS_ >= config_.endMissionAtS) {
    sim_.endMission();
    endMissionDone_ = true;
  }
  latest_.publish(pipeline_.snapshot());
}

void ProtoApp::applyManualFlying(double dt) {
  // Keyboard flying for the Manual scenario: hold to pull/push, release to return to 1 G.
  auto down = [](ImGuiKey a, ImGuiKey b) { return ImGui::IsKeyDown(a) || ImGui::IsKeyDown(b); };
  auto approach = [dt](double v, double target, double rate) {
    const double step = rate * dt;
    return v < target ? std::min(v + step, target) : std::max(v - step, target);
  };
  const bool pull = down(ImGuiKey_W, ImGuiKey_UpArrow);
  const bool push = down(ImGuiKey_S, ImGuiKey_DownArrow);
  if (pull && !push) manual_.nzCmd = approach(manual_.nzCmd, 9.0, 4.0);
  else if (push && !pull) manual_.nzCmd = approach(manual_.nzCmd, -3.0, 4.0);
  else manual_.nzCmd = approach(manual_.nzCmd, 1.0, 6.0);

  const bool left = down(ImGuiKey_A, ImGuiKey_LeftArrow), right = down(ImGuiKey_D, ImGuiKey_RightArrow);
  manual_.rollRateDegS = left == right ? 0.0 : (right ? 150.0 : -150.0);

  if (down(ImGuiKey_LeftShift, ImGuiKey_RightShift)) manual_.throttle = approach(manual_.throttle, 1.0, 0.5);
  if (ImGui::IsKeyDown(ImGuiKey_Z)) manual_.throttle = approach(manual_.throttle, 0.0, 0.5);
}

void ProtoApp::draw(float screenW, float screenH, double now) {
  const ImGuiIO& io = ImGui::GetIO();
  if (isHotkeyPressed(Hotkey{"F1", true, false, false})) debugOpen_ = !debugOpen_;

  const MetricsSnapshot snap = latest_.read();

  // Fake game scene: attitude from the source's ground truth (simulator) or the replayed row.
  SceneState st;
  st.phase = snap.phase;
  st.missionTimeS = snap.mission.durationS;
  st.speedKmh = snap.current.speed * 3.6;
  st.altitudeM = snap.current.altitude;
  if (sourceKind_ == SourceKind::Simulator) {
    const FlightState& t = sim_.truth();
    attitudeAngles(t.attitude, st.pitchDeg, st.rollDeg);
    st.aoaDeg = t.trueAoaDeg;
    st.sideslipDeg = t.trueSideslipDeg;
    st.speedKmh = length(t.velocity) * 3.6;
    st.altitudeM = t.position.z;
  } else if (const CsvRow* row = replay_.lastEmitted()) {
    attitudeAngles(row->sample.attitude, st.pitchDeg, st.rollDeg);
    st.aoaDeg = snap.current.aoaValid ? snap.current.aoaDeg : 0.0;
    st.sideslipDeg = snap.current.aoaValid ? snap.current.sideslipDeg : 0.0;
  }
  scene_.draw(ImGui::GetBackgroundDrawList(), screenW, screenH, st, now);

  // Input the overlay does not capture belongs to the "game". wantsInput() is what the mod
  // will use to decide whether to block game input; ImGui's WantCapture flags additionally
  // cover the prototype's debug panel. Values are from the previous UI frame, as in-game.
  const bool uiWantsInput = ui_.wantsInput();
  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.WantCaptureMouse && !uiWantsInput)
    scene_.registerClick(io.MousePos.x, io.MousePos.y, now);
  if (!io.WantCaptureKeyboard && !uiWantsInput) applyManualFlying(static_cast<double>(io.DeltaTime));

  ui_.frame(snap, screenW, screenH, now);
  if (debugOpen_) drawDebugPanel(screenW, screenH);
}

void ProtoApp::shutdown() {
  store_.flush();
  recorder_.close();
  archive_.flush();  // a mission that just ended is written before the process exits
}

std::string ProtoApp::missionLabel() const {
  if (sourceKind_ == SourceKind::Simulator) return std::string("Simulator: ") + scenarioName(sim_.options().scenario);
  return "Replay: " + (replay_.path().empty() ? std::string("(in-memory log)")
                                               : pathToUtf8(std::filesystem::path(replay_.path()).filename()));
}

std::optional<std::pair<int, int>> ProtoApp::takeResizeRequest() {
  auto r = resizeRequest_;
  resizeRequest_.reset();
  return r;
}

void ProtoApp::restartSource() {
  if (sourceKind_ == SourceKind::Simulator) sim_.restart();
  else replay_.restart();
  pipeline_.reset();  // time restarts: estimator history would be discontinuous
  manual_ = ControlInput{};
  endMissionDone_ = false;
}

void ProtoApp::setRecording(bool on) {
  if (!on) {
    if (recorder_.isOpen())
      recordStatus_ = "Saved " + std::to_string(recorder_.rowsWritten()) + " rows to " + recorder_.path();
    recorder_.close();
    return;
  }
  const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  char name[64];
  std::strftime(name, sizeof name, "ac8_flight_%Y%m%d_%H%M%S.csv", std::localtime(&t));
  const std::string path = (config_.recordDir / name).string();
  std::string error;
  if (!recorder_.open(path, &error)) {
    recordStatus_ = "Cannot record: " + error;
    return;
  }
  std::string comment = "recorded by ac8_gmeter_proto, source=";
  comment += sourceKind_ == SourceKind::Simulator ? scenarioName(sim_.options().scenario) : "replay";
  recorder_.writeComment(comment);
  recordStatus_.clear();
}

void ProtoApp::rescanReplays() {
  replayFiles_.clear();
  for (const auto& dir : config_.replayDirs) {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
      if (e.path().extension() == ".csv") replayFiles_.push_back(e.path());
  }
  // Stable sort by file name keeps the first directory's copy first among duplicates.
  std::stable_sort(replayFiles_.begin(), replayFiles_.end(),
                   [](const auto& a, const auto& b) { return a.filename() < b.filename(); });
  // Same file found in two places (build-dir copy + source tree): keep the first.
  replayFiles_.erase(std::unique(replayFiles_.begin(), replayFiles_.end(),
                                 [](const auto& a, const auto& b) { return a.filename() == b.filename(); }),
                     replayFiles_.end());
  if (replayIndex_ >= static_cast<int>(replayFiles_.size())) replayIndex_ = -1;
}

}  // namespace ft
