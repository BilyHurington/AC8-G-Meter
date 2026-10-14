#pragma once
// Everything the mod runs in game, wired like the prototype (apps/proto_common/proto_app.*):
//   game thread   : Lua frame -> FlightSampler -> TelemetryPipeline -> LatestValue snapshot;
//                   MissionArchiveRecorder saves finished missions; optional CSV recording
//   render thread : OverlayClient -> TelemetryUi (HUD, menus, debrief, history)
//   window thread : wantsInput() for the overlay's input hook
// Files live in the mod directory: settings.json, missions/, recordings/, assets/fonts/.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "config/settings_store.h"
#include "core/latest_value.h"
#include "core/pipeline.h"
#include "flight/flight_sampler.h"
#include "flight/lua_frame.h"
#include "overlay/overlay.h"
#include "runtime/debug_recorder.h"
#include "sources/csv_recorder.h"
#include "storage/mission_archive.h"
#include "storage/mission_recorder.h"
#include "ui/telemetry_ui.h"

namespace ft::mod {

struct ModOptions {
  bool recordCsv = false;  // write every mission's samples to <mod dir>/recordings/*.csv
  bool debugRecording = false;  // start with debug recording on (else off at every start)
};

class ModRuntime final : public OverlayClient {
 public:
  ModRuntime(std::filesystem::path modDir, ModOptions options);
  ~ModRuntime() override;
  ModRuntime(const ModRuntime&) = delete;
  ModRuntime& operator=(const ModRuntime&) = delete;

  // Game thread, once per tick.
  void onLuaFrame(const LuaFrame& frame);
  // Whether debug recording is on (the Lua sampler then sends the debug block). Any thread.
  bool debugRecordingWanted() const { return debugWanted_.load(std::memory_order_relaxed); }

  // OverlayClient (render / window thread).
  void onImGuiInit() override;
  void onFrame(float width, float height, double now) override;
  bool wantsInput() const override { return ui_.wantsInput(); }

 private:
  void push(const RawFlightSample& sample);
  void startRecording();
  void stopRecording();
  void updateDebugRecording();  // opens / closes the debug file to follow the setting

  std::filesystem::path modDir_;
  ModOptions options_;
  SettingsStore store_;
  MissionArchive archive_;  // before missionRecorder_ and ui_, which keep pointers to it
  MissionArchiveRecorder missionRecorder_;  // before pipeline_, which calls it
  TelemetryUi ui_;
  TelemetryPipeline pipeline_;
  LatestValue<MetricsSnapshot> latest_;        // game -> render thread
  LatestValue<PipelineConfig> pendingConfig_;  // render -> game thread (settings changes)
  std::uint64_t appliedConfigVersion_ = 0;
  const PipelineConfig baseConfig_{};

  FlightSampler sampler_;
  std::vector<RawFlightSample> samples_;
  CsvRecorder recorder_;
  double recordFlushAtS_ = 0.0;  // game time of the next CSV flush
  bool lastRecordedPaused_ = false;

  // Debug recording: the UI's setting (render thread) -> the game thread, which owns the file.
  std::atomic<bool> debugWanted_{false};
  DebugRecorder debug_;
  bool debugOpenFailed_ = false;  // until the setting is switched off
  LatestValue<std::string> debugStatus_;  // game -> render thread, shown in the menu
};

}  // namespace ft::mod
