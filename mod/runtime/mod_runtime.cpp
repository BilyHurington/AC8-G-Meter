#include "runtime/mod_runtime.h"

#include <chrono>
#include <ctime>

#include <imgui.h>

#include "support/log.h"
#include "ui/style.h"

namespace ft::mod {

namespace {

constexpr const char* kMissionLabel = "AC8 sortie";  // until the mod passes the map name
constexpr double kRecordFlushIntervalS = 2.0;

std::string localStamp() {
  const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm local{};
  localtime_s(&local, &t);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", &local);
  return buf;
}

}  // namespace

ModRuntime::ModRuntime(std::filesystem::path modDir, ModOptions options)
    : modDir_(std::move(modDir)),
      options_(options),
      store_(modDir_ / "settings.json"),
      archive_(modDir_ / "missions"),
      missionRecorder_(archive_),
      ui_(store_) {
  store_.load();
  if (!store_.lastError().empty()) logf("settings: {} (using defaults)", store_.lastError());
  // From here on the UI keeps both in step with the settings (atomics, render thread).
  archive_.setMaxSaved(store_.settings().history.maxSavedMissions);
  missionRecorder_.setEnabled(store_.settings().history.saveMissions);
  ui_.setMissionArchive(&archive_, &missionRecorder_);
  pipeline_.addConsumer(&missionRecorder_);
  pipeline_.setConfig(store_.settings().toPipelineConfig(baseConfig_));
  pipeline_.setMissionLabel(kMissionLabel);
  // The UI (render thread) only publishes the new config; onLuaFrame (game thread) applies it.
  ui_.onSettingsChanged = [this](const Settings& s) { pendingConfig_.publish(s.toPipelineConfig(baseConfig_)); };
  latest_.publish(pipeline_.snapshot());
  // Debug recording is per session (a forgotten switch must not record every future game):
  // off unless config.lua asks for it from the start.
  store_.settings().diagnostics.debugRecording = options_.debugRecording;
  debugWanted_ = options_.debugRecording;
  debugStatus_.publish("Off");
}

ModRuntime::~ModRuntime() {
  stopRecording();
  debug_.close();
  store_.flush();
  archive_.flush();
}

void ModRuntime::onLuaFrame(const LuaFrame& frame) {
  if (PipelineConfig config; pendingConfig_.readIfNewer(appliedConfigVersion_, config)) pipeline_.setConfig(config);

  samples_.clear();
  const bool wasInMission = sampler_.inMission();
  sampler_.onFrame(frame, samples_);
  if (!wasInMission && sampler_.inMission()) {
    logLine("mission started");
    if (options_.recordCsv) startRecording();
  }
  for (const RawFlightSample& s : samples_) push(s);
  if (wasInMission && !sampler_.inMission()) {
    logLine("mission ended");
    stopRecording();
  }
  updateDebugRecording();
  if (!samples_.empty() || debug_.isOpen()) {
    const MetricsSnapshot snapshot = pipeline_.snapshot();
    if (debug_.isOpen()) debug_.write(frame, samples_, snapshot);
    if (!samples_.empty()) latest_.publish(snapshot);
  }
}

void ModRuntime::updateDebugRecording() {
  const bool wanted = debugWanted_.load(std::memory_order_relaxed);
  if (!wanted) debugOpenFailed_ = false;  // switching it off and on again retries
  if (wanted == debug_.isOpen() || (wanted && debugOpenFailed_)) return;
  if (wanted) {
    std::string error;
    if (debug_.open(modDir_ / "debug", &error)) {
      logf("debug recording to {}", pathToUtf8(debug_.path()));
      debugStatus_.publish("Recording to " + pathToUtf8(debug_.path()));
    } else {
      logf("debug recording: {}", error);
      debugStatus_.publish("Cannot record: " + error);
      debugOpenFailed_ = true;
    }
  } else {
    logf("debug recording stopped ({} rows)", debug_.rows());
    debugStatus_.publish("Off. Last file: " + pathToUtf8(debug_.path()));
    debug_.close();
  }
}

void ModRuntime::push(const RawFlightSample& s) {
  pipeline_.push(s);
  if (!recorder_.isOpen()) return;
  // While paused the game repeats the same state every tick: record the pause once.
  const bool paused = s.phase == MissionPhase::Paused;
  if (paused && lastRecordedPaused_) return;
  lastRecordedPaused_ = paused;
  recorder_.write(CsvRow{s, std::nullopt, std::nullopt});
  if (s.t >= recordFlushAtS_ || s.t + kRecordFlushIntervalS < recordFlushAtS_) {  // also on a clock restart
    recorder_.flush();
    recordFlushAtS_ = s.t + kRecordFlushIntervalS;
  }
}

void ModRuntime::startRecording() {
  const std::filesystem::path dir = modDir_ / "recordings";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::filesystem::path path = dir / ("ac8_flight_" + localStamp() + ".csv");
  std::string error;
  if (!recorder_.open(path, &error)) {
    logf("CSV recording: {}", error);
    return;
  }
  recorder_.writeComment("recorded in ACE COMBAT 8 by the AC8 G-Meter mod");
  recordFlushAtS_ = 0.0;
  lastRecordedPaused_ = false;
  logf("recording to {}", pathToUtf8(path));
}

void ModRuntime::stopRecording() {
  if (!recorder_.isOpen()) return;
  logf("recorded {} samples", recorder_.rowsWritten());
  recorder_.close();
}

void ModRuntime::onImGuiInit() {
  applyTelemetryStyle(ImGui::GetStyle());
  const std::filesystem::path font = modDir_ / "assets" / "fonts" / "B612Mono-Regular.ttf";
  if (!ui_.loadFonts(pathToUtf8(font))) logf("HUD font not found at {}, using ImGui's default font", pathToUtf8(font));
}

void ModRuntime::onFrame(float width, float height, double now) {
  debugWanted_.store(store_.settings().diagnostics.debugRecording, std::memory_order_relaxed);
  ui_.setDiagnostics(true, debugStatus_.read());
  ui_.frame(latest_.read(), width, height, now);
}

}  // namespace ft::mod
