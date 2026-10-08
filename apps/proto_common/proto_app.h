#pragma once
// Everything the prototype does per frame, independent of the window system and graphics API
// (the hosts apps/macos_proto and apps/win_proto own those):
// telemetry source -> pipeline -> LatestValue snapshot -> fake game scene + TelemetryUi +
// debug panel.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "config/settings_store.h"
#include "core/latest_value.h"
#include "core/pipeline.h"
#include "game_scene.h"
#include "sources/csv_recorder.h"
#include "sources/replay_source.h"
#include "sources/simulated_source.h"
#include "storage/mission_archive.h"
#include "storage/mission_recorder.h"
#include "ui/telemetry_ui.h"

namespace ft {

struct ProtoConfig {
  std::filesystem::path settingsPath;
  std::filesystem::path missionsDir;             // mission history (MissionArchive)
  std::filesystem::path assetsDir;               // contains fonts/
  std::vector<std::filesystem::path> replayDirs;  // searched for *.csv
  std::filesystem::path recordDir;               // where "Record to CSV" writes
  // Scripted options (command line), mainly for automated screenshots.
  bool openMenu = false;
  bool openHistory = false;
  bool editMode = false;
  bool debugPanel = true;
  std::optional<Scenario> scenario;
  double endMissionAtS = -1.0;  // simulated seconds; < 0 = never
  double velocityNoiseMS = 0.0;  // simulator: gaussian velocity noise per axis (dirty data)
};

class ProtoApp {
 public:
  explicit ProtoApp(ProtoConfig config);
  ~ProtoApp();

  // After ImGui::CreateContext(), before the first frame: fonts and style.
  void initImGui();
  // Game-thread side: poll the source, run the pipeline, publish the snapshot.
  void update(double realDtS);
  // Render-thread side, between ImGui::NewFrame() and ImGui::Render().
  void draw(float screenW, float screenH, double now);
  void shutdown();  // flushes settings, closes the recorder

  // Pending "virtual resolution" window resize from the debug panel (window size units of the
  // host: points on macOS, pixels on Windows).
  std::optional<std::pair<int, int>> takeResizeRequest();

 private:
  enum class SourceKind { Simulator, Replay };

  void applyManualFlying(double dt);
  void drawDebugPanel(float screenW, float screenH);
  void simulatorControls();
  void replayControls();
  void readouts();
  void restartSource();
  void setRecording(bool on);
  void rescanReplays();
  ITelemetrySource& source();

  std::string missionLabel() const;  // "Simulator: <scenario>" / "Replay: <file>"

  ProtoConfig config_;
  SettingsStore store_;
  MissionArchive archive_;  // declared before ui_ and missionRecorder_, which keep pointers to it
  // Saves every finished mission from the pipeline (game thread), independent of UI frames.
  // Declared before pipeline_, which calls it until destroyed.
  MissionArchiveRecorder missionRecorder_;
  TelemetryUi ui_;
  TelemetryPipeline pipeline_;
  LatestValue<MetricsSnapshot> latest_;  // pipeline -> UI hand-off (game -> render thread in-game)
  // UI -> pipeline hand-off (render -> game thread in-game). The UI never touches pipeline_.
  LatestValue<PipelineConfig> pendingConfig_;
  std::uint64_t appliedConfigVersion_ = 0;
  const PipelineConfig baseConfig_{};  // non-user-tunable pipeline parameters

  SourceKind sourceKind_ = SourceKind::Simulator;
  SimulatedSourceOptions simOptions_;
  SimulatedSource sim_;
  ReplaySource replay_;
  std::vector<RawFlightSample> samples_;
  std::string appliedLabel_;
  double simTimeS_ = 0.0;  // game time of the latest sample (for --end-mission-at)
  bool endMissionDone_ = false;

  ControlInput manual_;  // keyboard flying (Manual scenario)

  CsvRecorder recorder_;
  std::string recordStatus_;

  std::vector<std::filesystem::path> replayFiles_;
  int replayIndex_ = -1;
  std::string replayStatus_;

  GameScene scene_;
  bool debugOpen_ = true;
  std::optional<std::pair<int, int>> resizeRequest_;
};

}  // namespace ft
