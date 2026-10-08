// Headless ImGui run of the whole UI: no window, no GPU. Mimics imgui_impl_null so ImGui 1.92's
// dynamic font atlas works (textures are "created" by flipping their status).
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

#include <imgui.h>

#include "config/hotkey_names.h"
#include "core/pipeline.h"
#include "storage/mission_archive.h"
#include "storage/mission_recorder.h"
#include "ui/g_chart.h"
#include "ui/mission_history.h"
#include "ui/mission_summary.h"
#include "ui/hotkey_input.h"
#include "ui/layout.h"
#include "ui/overlay_layer.h"
#include "ui/settings_menu.h"
#include "ui/style.h"
#include "ui/telemetry_ui.h"

using namespace ft;

namespace {

// RAII ImGui context configured like a renderer backend that supports dynamic textures.
struct HeadlessImGui {
  explicit HeadlessImGui(float w = 1920.0f, float h = 1080.0f) {
    ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(w, h);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    applyTelemetryStyle(ImGui::GetStyle());
  }
  ~HeadlessImGui() { ImGui::DestroyContext(ctx); }

  void frame(TelemetryUi& ui, const MetricsSnapshot& snap, double now) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ui.frame(snap, io.DisplaySize.x, io.DisplaySize.y, now);
    ImGui::Render();
    if (ImDrawData* dd = ImGui::GetDrawData(); dd && dd->Textures)
      for (ImTextureData* tex : *dd->Textures) {
        if (tex->Status == ImTextureStatus_WantDestroy) {
          tex->SetTexID(ImTextureID_Invalid);
          tex->SetStatus(ImTextureStatus_Destroyed);
        } else if (tex->Status != ImTextureStatus_OK) {
          tex->SetStatus(ImTextureStatus_OK);
        }
      }
  }

  ImGuiContext* ctx = nullptr;
};

std::filesystem::path tmpSettings(const char* name) {
  const auto p = std::filesystem::path(FT_TEST_TMP_DIR) / "ui" / name;
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  std::filesystem::remove(p, ec);
  return p;
}

MetricsSnapshot fakeSnapshot(std::uint32_t reportId) {
  MetricsSnapshot s;
  s.phase = MissionPhase::Active;
  s.current.valid = true;
  s.current.g = 6.3;
  s.current.aoaValid = true;
  s.current.aoaDeg = 12.4;
  s.mission.hasData = true;
  s.mission.maxG = 7.9;
  s.mission.minG = -1.2;
  if (reportId > 0) {
    auto r = std::make_shared<MissionReport>();
    r->id = reportId;
    r->stats = s.mission;
    r->stats.durationS = 125.0;
    r->stats.timeInGBinS = {1, 2, 30, 50, 20, 10, 5, 3, 2, 1};
    r->stats.extremeGSamples = 42;
    for (int i = 0; i < 2500; ++i) {
      const float g = static_cast<float>(1.0 + 5.0 * ((i % 100) / 100.0));
      r->trace.push({g, g - 0.2f, g + 0.4f, g - 0.1f, 2.0f, 8.0f, 250.0f, 3000.0f, g > 5.5f});
    }
    s.lastReport = r;
  }
  return s;
}

struct CountingLayer final : IOverlayLayer {
  int* draws;
  explicit CountingLayer(int* d) : draws(d) {}
  const char* name() const override { return "counting"; }
  bool draw(OverlayFrame&) override {
    ++*draws;
    return false;
  }
};

}  // namespace

TEST_CASE("every known hotkey name maps to an ImGui key") {
  HeadlessImGui im;
  for (std::string_view name : knownHotkeyNames())
    CHECK_MESSAGE(imguiKeyFromName(name) != ImGuiKey_None, std::string(name));
  CHECK(imguiKeyFromName("F8") == ImGuiKey_F8);
  CHECK(imguiKeyFromName("Nope") == ImGuiKey_None);
}

TEST_CASE("font loading falls back when the file is missing") {
  {
    HeadlessImGui im;
    SettingsStore store(tmpSettings("font_ok.json"));
    TelemetryUi ui(store);
    CHECK(ui.loadFonts(FT_SOURCE_DIR "/assets/fonts/B612Mono-Regular.ttf"));
  }
  {
    HeadlessImGui im;
    SettingsStore store(tmpSettings("font_missing.json"));
    TelemetryUi ui(store);
    CHECK_FALSE(ui.loadFonts("/definitely/not/here.ttf"));
    for (int i = 0; i < 3; ++i) im.frame(ui, fakeSnapshot(0), i * 0.1);  // default-font fallback draws
  }
}

TEST_CASE("headless frames: overlay, menu, summary, edit-mode drag, extra layers") {
  HeadlessImGui im;
  SettingsStore store(tmpSettings("smoke.json"));
  store.load();
  TelemetryUi ui(store);
  REQUIRE(ui.loadFonts(FT_SOURCE_DIR "/assets/fonts/B612Mono-Regular.ttf"));
  int extraDraws = 0;
  ui.addLayer(std::make_unique<CountingLayer>(&extraDraws));
  int pipelineChanges = 0;
  ui.onSettingsChanged = [&](const Settings&) { ++pipelineChanges; };

  double now = 0.0;
  ImGuiIO& io = ImGui::GetIO();
  const MetricsSnapshot noReport = fakeSnapshot(0);
  for (int i = 0; i < 5; ++i) im.frame(ui, noReport, now += 1.0 / 60.0);
  CHECK(extraDraws == 5);
  CHECK_FALSE(ui.wantsInput());  // normal overlay mode never captures input

  // Normal mode, mouse over the HUD text and clicking: the game keeps the mouse.
  const OverlayLayout hud = computeOverlayLayout(store.settings().overlay, 1920.0f, 1080.0f);
  io.AddMousePosEvent(hud.x + 10.0f, hud.y + 5.0f);
  im.frame(ui, noReport, now += 1.0 / 60.0);
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
  im.frame(ui, noReport, now += 1.0 / 60.0);
  CHECK_FALSE(io.WantCaptureMouse);
  CHECK_FALSE(ui.wantsInput());
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
  im.frame(ui, noReport, now += 1.0 / 60.0);

  // A finished mission pops the summary (autoShow defaults to on). It does not capture
  // input while the mouse is elsewhere (the game's results screen stays usable)...
  const MetricsSnapshot withReport = fakeSnapshot(1);
  io.AddMousePosEvent(10.0f, 1070.0f);
  for (int i = 0; i < 3; ++i) im.frame(ui, withReport, now += 1.0 / 60.0);
  CHECK(ui.summaryOpen());
  CHECK_FALSE(ui.wantsInput());
  // ...only while the mouse is over it (it opens centered on the screen).
  io.AddMousePosEvent(960.0f, 540.0f);
  im.frame(ui, withReport, now += 1.0 / 60.0);
  CHECK(ui.wantsInput());
  io.AddMousePosEvent(10.0f, 1070.0f);
  im.frame(ui, withReport, now += 1.0 / 60.0);
  CHECK_FALSE(ui.wantsInput());

  ui.showSummary(false);
  im.frame(ui, withReport, now += 1.0 / 60.0);
  CHECK_FALSE(ui.summaryOpen());  // same report id does not reopen it

  // Ctrl+F8 toggles the settings menu.
  io.AddKeyEvent(ImGuiMod_Ctrl, true);
  io.AddKeyEvent(ImGuiKey_F8, true);
  im.frame(ui, withReport, now += 1.0 / 60.0);
  io.AddKeyEvent(ImGuiKey_F8, false);
  io.AddKeyEvent(ImGuiMod_Ctrl, false);
  im.frame(ui, withReport, now += 1.0 / 60.0);
  CHECK(ui.menuOpen());
  for (int i = 0; i < 3; ++i) im.frame(ui, withReport, now += 1.0 / 60.0);

  // Ctrl+F9 enters edit mode; drag the overlay by (+200, +100) px.
  io.AddKeyEvent(ImGuiMod_Ctrl, true);
  io.AddKeyEvent(ImGuiKey_F9, true);
  im.frame(ui, withReport, now += 1.0 / 60.0);
  io.AddKeyEvent(ImGuiKey_F9, false);
  io.AddKeyEvent(ImGuiMod_Ctrl, false);
  im.frame(ui, withReport, now += 1.0 / 60.0);
  CHECK(ui.editMode());
  ui.setMenuOpen(false);  // keep the menu window out of the drag path

  const OverlayLayout before = computeOverlayLayout(store.settings().overlay, 1920.0f, 1080.0f);
  const ImVec2 grab(before.x + 10.0f, before.y + 5.0f);
  io.AddMousePosEvent(grab.x, grab.y);
  im.frame(ui, withReport, now += 1.0 / 60.0);
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
  im.frame(ui, withReport, now += 1.0 / 60.0);
  io.AddMousePosEvent(grab.x + 200.0f, grab.y + 100.0f);
  im.frame(ui, withReport, now += 1.0 / 60.0);
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
  im.frame(ui, withReport, now += 1.0 / 60.0);
  im.frame(ui, withReport, now += 1.0 / 60.0);

  const OverlayLayout after = computeOverlayLayout(store.settings().overlay, 1920.0f, 1080.0f);
  CHECK(after.x == doctest::Approx(before.x + 200.0f).epsilon(0.01));
  CHECK(after.y == doctest::Approx(before.y + 100.0f).epsilon(0.01));
  CHECK(store.dirty());
  im.frame(ui, withReport, now += 1.0);  // debounce elapsed -> saved
  CHECK_FALSE(store.dirty());
  CHECK(std::filesystem::exists(store.path()));

  // Locked overlays do not move.
  store.settings().overlay.locked = true;
  const OverlaySettings lockedPos = store.settings().overlay;
  io.AddMousePosEvent(after.x + 10.0f, after.y + 5.0f);
  im.frame(ui, withReport, now += 1.0 / 60.0);
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
  im.frame(ui, withReport, now += 1.0 / 60.0);
  io.AddMousePosEvent(after.x + 300.0f, after.y + 300.0f);
  im.frame(ui, withReport, now += 1.0 / 60.0);
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
  im.frame(ui, withReport, now += 1.0 / 60.0);
  CHECK(store.settings().overlay.posX == lockedPos.posX);

  // A second report, all metrics on, other resolutions, menu open again.
  ui.setEditMode(false);
  ui.setMenuOpen(true);
  store.settings().overlay.visible.fill(true);
  store.settings().overlay.background = true;
  const MetricsSnapshot second = fakeSnapshot(2);
  io.DisplaySize = ImVec2(3840.0f, 2160.0f);
  for (int i = 0; i < 3; ++i) im.frame(ui, second, now += 1.0 / 60.0);
  io.DisplaySize = ImVec2(1280.0f, 720.0f);
  for (int i = 0; i < 3; ++i) im.frame(ui, second, now += 1.0 / 60.0);
  CHECK(ui.wantsInput());
  CHECK(pipelineChanges == 0);  // nothing pipeline-related was edited
}

TEST_CASE("aborted missions do not pop the summary") {
  HeadlessImGui im;
  SettingsStore store(tmpSettings("aborted.json"));
  TelemetryUi ui(store);
  REQUIRE(ui.loadFonts(FT_SOURCE_DIR "/assets/fonts/B612Mono-Regular.ttf"));
  double now = 0.0;

  MetricsSnapshot aborted = fakeSnapshot(1);
  auto report = std::make_shared<MissionReport>(*aborted.lastReport);
  report->aborted = true;
  aborted.lastReport = report;
  for (int i = 0; i < 3; ++i) im.frame(ui, aborted, now += 1.0 / 60.0);
  CHECK_FALSE(ui.summaryOpen());
  CHECK_FALSE(ui.wantsInput());

  // Still reachable on demand ("Show last mission" does the same).
  ui.showSummary(true);
  im.frame(ui, aborted, now += 1.0 / 60.0);
  CHECK(ui.summaryOpen());
  ui.showSummary(false);

  // The next completed mission pops as usual.
  for (int i = 0; i < 3; ++i) im.frame(ui, fakeSnapshot(2), now += 1.0 / 60.0);
  CHECK(ui.summaryOpen());
}

TEST_CASE("leaving edit mode mid-drag keeps the dragged position") {
  HeadlessImGui im;
  SettingsStore store(tmpSettings("middrag.json"));
  TelemetryUi ui(store);
  REQUIRE(ui.loadFonts(FT_SOURCE_DIR "/assets/fonts/B612Mono-Regular.ttf"));
  ImGuiIO& io = ImGui::GetIO();
  const MetricsSnapshot snap = fakeSnapshot(0);
  double now = 0.0;

  ui.setEditMode(true);
  const OverlayLayout before = computeOverlayLayout(store.settings().overlay, 1920.0f, 1080.0f);
  io.AddMousePosEvent(before.x + 10.0f, before.y + 5.0f);
  im.frame(ui, snap, now += 1.0 / 60.0);
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
  im.frame(ui, snap, now += 1.0 / 60.0);
  io.AddMousePosEvent(before.x + 160.0f, before.y + 85.0f);
  im.frame(ui, snap, now += 1.0 / 60.0);

  // Ctrl+F9 while the button is still held.
  io.AddKeyEvent(ImGuiMod_Ctrl, true);
  io.AddKeyEvent(ImGuiKey_F9, true);
  im.frame(ui, snap, now += 1.0 / 60.0);
  io.AddKeyEvent(ImGuiKey_F9, false);
  io.AddKeyEvent(ImGuiMod_Ctrl, false);
  im.frame(ui, snap, now += 1.0 / 60.0);
  CHECK_FALSE(ui.editMode());
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
  im.frame(ui, snap, now += 1.0 / 60.0);

  const OverlayLayout after = computeOverlayLayout(store.settings().overlay, 1920.0f, 1080.0f);
  CHECK(after.x == doctest::Approx(before.x + 150.0f).epsilon(0.01));
  CHECK(after.y == doctest::Approx(before.y + 80.0f).epsilon(0.01));
  CHECK(store.dirty());
}

namespace {

std::filesystem::path freshArchiveDir(const char* name) {
  const auto p = std::filesystem::path(FT_TEST_TMP_DIR) / "ui" / name;
  std::error_code ec;
  std::filesystem::remove_all(p, ec);
  return p;
}

}  // namespace

TEST_CASE("live missions are archived and the summary / history windows draw") {
  HeadlessImGui im;
  SettingsStore store(tmpSettings("history.json"));
  TelemetryUi ui(store);
  REQUIRE(ui.loadFonts(FT_SOURCE_DIR "/assets/fonts/B612Mono-Regular.ttf"));
  MissionArchive archive(freshArchiveDir("history_archive"));
  MissionArchiveRecorder recorder(archive);
  ui.setMissionArchive(&archive, &recorder);
  ImGuiIO& io = ImGui::GetIO();
  double now = 0.0;

  // Two finished missions arrive; the recorder (game thread, called by the pipeline) saves
  // each, the UI only shows the summary and the save status.
  MetricsSnapshot first = fakeSnapshot(1);
  std::const_pointer_cast<MissionReport>(first.lastReport)->stats.hasGData = true;
  recorder.onMissionEnded(first.lastReport);
  for (int i = 0; i < 3; ++i) im.frame(ui, first, now += 1.0 / 60.0);
  archive.flush();
  CHECK(archive.lastSave().state == SaveStatus::State::Saved);
  REQUIRE(archive.lastSave().report);
  CHECK(archive.lastSave().report == first.lastReport);  // shared, not copied
  MetricsSnapshot second = fakeSnapshot(2);
  std::const_pointer_cast<MissionReport>(second.lastReport)->stats.hasGData = true;
  recorder.onMissionEnded(second.lastReport);
  for (int i = 0; i < 3; ++i) im.frame(ui, second, now += 1.0 / 60.0);
  archive.flush();
  CHECK(archive.index().entries.size() == 2);
  CHECK(ui.summaryOpen());

  // Mouse wheel / drag / double-click on the chart (it sits in the upper part of the summary).
  io.AddMousePosEvent(960.0f, 420.0f);
  im.frame(ui, second, now += 1.0 / 60.0);
  io.AddMouseWheelEvent(0.0f, 2.0f);
  im.frame(ui, second, now += 1.0 / 60.0);
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
  im.frame(ui, second, now += 1.0 / 60.0);
  io.AddMousePosEvent(900.0f, 420.0f);
  im.frame(ui, second, now += 1.0 / 60.0);
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
  im.frame(ui, second, now += 1.0 / 60.0);
  CHECK(ui.wantsInput());  // the mouse is on the summary

  // The history window captures input like the menu, wherever the mouse is.
  ui.showSummary(false);
  ui.showHistory(true);
  io.AddMousePosEvent(5.0f, 5.0f);
  for (int i = 0; i < 3; ++i) im.frame(ui, second, now += 1.0 / 60.0);
  CHECK(ui.wantsInput());
  archive.flush();
  for (int i = 0; i < 2; ++i) im.frame(ui, second, now += 1.0 / 60.0);
  ui.showHistory(false);
  im.frame(ui, second, now += 1.0 / 60.0);
  CHECK_FALSE(ui.wantsInput());

  // Saving off in the settings: the UI turns the recorder off; nothing new is written.
  CHECK(recorder.enabled());
  store.settings().history.saveMissions = false;
  im.frame(ui, second, now += 1.0 / 60.0);
  CHECK_FALSE(recorder.enabled());
  const MetricsSnapshot third = fakeSnapshot(3);
  std::const_pointer_cast<MissionReport>(third.lastReport)->stats.hasGData = true;
  recorder.onMissionEnded(third.lastReport);
  for (int i = 0; i < 3; ++i) im.frame(ui, third, now += 1.0 / 60.0);
  archive.flush();
  CHECK(archive.index().entries.size() == 2);

  // The retention limit follows the setting as well.
  store.settings().history.saveMissions = true;
  store.settings().history.maxSavedMissions = 1;
  im.frame(ui, third, now += 1.0 / 60.0);
  CHECK(recorder.enabled());
  auto fourth = std::make_shared<MissionReport>(*fakeSnapshot(4).lastReport);
  fourth->stats.hasGData = true;
  recorder.onMissionEnded(fourth);
  archive.flush();
  CHECK(archive.index().entries.size() == 1);
}

TEST_CASE("the summary pops for a new report even when a recreated pipeline reuses its id") {
  HeadlessImGui im;
  SettingsStore store(tmpSettings("idrestart.json"));
  TelemetryUi ui(store);
  REQUIRE(ui.loadFonts(FT_SOURCE_DIR "/assets/fonts/B612Mono-Regular.ttf"));
  double now = 0.0;

  const MetricsSnapshot first = fakeSnapshot(1);
  for (int i = 0; i < 3; ++i) im.frame(ui, first, now += 1.0 / 60.0);
  CHECK(ui.summaryOpen());
  ui.showSummary(false);
  for (int i = 0; i < 3; ++i) im.frame(ui, first, now += 1.0 / 60.0);
  CHECK_FALSE(ui.summaryOpen());  // the same report does not pop again

  // The host recreated the pipeline: its first mission is id 1 again, but a new report.
  const MetricsSnapshot restarted = fakeSnapshot(1);
  REQUIRE(restarted.lastReport != first.lastReport);
  im.frame(ui, restarted, now += 1.0 / 60.0);
  CHECK(ui.summaryOpen());
}

TEST_CASE("keep-missions changes that delete saved missions need confirmation") {
  // (current limit, new limit, saved missions) -> missions to confirm (0 = apply directly)
  CHECK(missionsDeletedByNewLimit(50, 10, 30) == 20);
  CHECK(missionsDeletedByNewLimit(0, 10, 30) == 20);    // from unlimited
  CHECK(missionsDeletedByNewLimit(50, 29, 30) == 1);
  CHECK(missionsDeletedByNewLimit(50, 30, 30) == 0);    // all still fit
  CHECK(missionsDeletedByNewLimit(50, 40, 30) == 0);
  CHECK(missionsDeletedByNewLimit(10, 0, 30) == 0);     // to unlimited
  CHECK(missionsDeletedByNewLimit(10, 20, 30) == 0);    // raising the limit
  CHECK(missionsDeletedByNewLimit(20, 10, 30) == 20);   // lower than before: the total is shown
  CHECK(missionsDeletedByNewLimit(10, 10, 30) == 0);    // unchanged
  CHECK(missionsDeletedByNewLimit(50, 10, 0) == 0);     // nothing saved

  // Headless: the menu draws with the history section and a known count.
  HeadlessImGui im;
  SettingsStore store(tmpSettings("keepmissions.json"));
  TelemetryUi ui(store);
  REQUIRE(ui.loadFonts(FT_SOURCE_DIR "/assets/fonts/B612Mono-Regular.ttf"));
  MissionArchive archive(freshArchiveDir("keep_missions"));
  MissionArchiveRecorder recorder(archive);
  ui.setMissionArchive(&archive, &recorder);
  ui.setMenuOpen(true);
  double now = 0.0;
  for (int i = 0; i < 3; ++i) im.frame(ui, fakeSnapshot(0), now += 1.0 / 60.0);
  archive.flush();
  CHECK(archive.indexVersion() >= 1);  // opening the menu lists the folder (for the count)
  CHECK(store.settings().history.maxSavedMissions == 50);
}

TEST_CASE("history window: closing it abandons a pending load; a second click switches rows") {
  HeadlessImGui im;
  MissionArchive archive(freshArchiveDir("history_cancel"));
  for (std::uint32_t id = 1; id <= 3; ++id) archive.saveAsync(std::make_shared<MissionReport>(*fakeSnapshot(id).lastReport));
  archive.flush();
  MissionHistoryWindow window;
  bool open = true;
  auto frame = [&] {
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    MissionHistoryWindow::Result r = window.draw(archive, open);
    ImGui::Render();
    if (ImDrawData* dd = ImGui::GetDrawData(); dd && dd->Textures)
      for (ImTextureData* tex : *dd->Textures)
        if (tex->Status != ImTextureStatus_OK && tex->Status != ImTextureStatus_Destroyed) tex->SetStatus(ImTextureStatus_OK);
    return r;
  };
  frame();
  archive.flush();
  frame();
  frame();
  CHECK_FALSE(window.readingFolder());
  const std::vector<MissionIndexEntry> entries = archive.index().entries;
  REQUIRE(entries.size() == 3);

  // Start a load, close the window before it is polled, let the load finish meanwhile.
  window.requestLoad(archive, entries[0]);
  CHECK(window.loading());
  open = false;
  CHECK_FALSE(frame().report);
  CHECK_FALSE(window.loading());
  archive.flush();
  for (int i = 0; i < 3; ++i) CHECK_FALSE(frame().report);

  // Reopen: the listing is re-read ("Reading folder..." until it is done), and the abandoned
  // load never pops a summary.
  const std::uint64_t versionBefore = archive.indexVersion();
  open = true;
  CHECK_FALSE(frame().report);
  CHECK((window.readingFolder() || archive.indexVersion() > versionBefore));
  archive.flush();
  for (int i = 0; i < 3; ++i) CHECK_FALSE(frame().report);
  CHECK_FALSE(window.readingFolder());

  // Two clicks in a row (second while the first is loading): the second row wins.
  window.requestLoad(archive, entries[0]);
  window.requestLoad(archive, entries[2]);
  archive.flush();
  MissionHistoryWindow::Result r = frame();
  REQUIRE(r.report);
  CHECK(r.entry.path == entries[2].path);
  CHECK(r.report->id == entries[2].id);
  for (int i = 0; i < 3; ++i) CHECK_FALSE(frame().report);  // the first load is never delivered
}

TEST_CASE("history window: clicking a row loads that mission without blocking") {
  HeadlessImGui im;
  MissionArchive archive(freshArchiveDir("history_click"));
  for (std::uint32_t id = 1; id <= 3; ++id) {
    auto r = std::make_shared<MissionReport>(*fakeSnapshot(id).lastReport);
    r->label = "Simulator: DogfightMix";
    archive.saveAsync(r);
  }
  archive.flush();
  MissionHistoryWindow window;
  bool open = true;
  ImGuiIO& io = ImGui::GetIO();
  GChart chart;

  auto frame = [&] {
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    MissionHistoryWindow::Result r = window.draw(archive, open);
    ImGui::Render();
    if (ImDrawData* dd = ImGui::GetDrawData(); dd && dd->Textures)
      for (ImTextureData* tex : *dd->Textures)
        if (tex->Status != ImTextureStatus_OK && tex->Status != ImTextureStatus_Destroyed) tex->SetStatus(ImTextureStatus_OK);
    return r;
  };
  for (int i = 0; i < 3; ++i) frame();
  archive.flush();
  for (int i = 0; i < 2; ++i) frame();

  // Rows sit below the toolbar and the table header; scan down the window's left column.
  const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
  MissionHistoryWindow::Result loaded;
  for (float y = centre.y - 300.0f; y < centre.y && !window.loading() && !loaded.report; y += 3.0f) {
    io.AddMousePosEvent(centre.x - 200.0f, y);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    loaded = frame();
  }
  for (int i = 0; i < 500 && !loaded.report; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    loaded = frame();
  }
  REQUIRE(loaded.report);
  CHECK(loaded.report->label == "Simulator: DogfightMix");
  CHECK(loaded.report->trace.size() == 2500);
  CHECK(loaded.entry.path.extension() == ".json");
  CHECK_FALSE(window.loading());

  // The loaded mission draws in the summary window (same code path as a live one).
  bool summaryOpen = true;
  for (int i = 0; i < 2; ++i) {
    ImGui::NewFrame();
    SummaryContext ctx;
    ctx.subtitle = "from history";
    ctx.status = "From the mission history";
    drawMissionSummary(*loaded.report, ctx, chart, summaryOpen, nullptr);
    ImGui::Render();
  }
  CHECK(summaryOpen);
}

TEST_CASE("results screen: dual G rows, average-G table and its chart highlight") {
  // A real mission through the pipeline: 7 G for 6 s with a short 11 G bump, 70 s in all.
  TelemetryPipeline p;
  double vz = 0.0, z = 3000.0;
  for (int i = 0; i < 70 * 60; ++i) {
    const double t = i / 60.0;
    const double nz = (t >= 20.0 && t < 26.0) ? 7.0 : ((t >= 40.0 && t < 40.1) ? 11.0 : 1.0);
    RawFlightSample s;
    s.t = t;
    s.position = {200.0 * t, 0.0, z};
    s.velocity = Vec3{200.0, 0.0, vz};
    s.phase = MissionPhase::Active;
    p.push(s);
    vz += (nz - 1.0) * kG0 / 60.0;
    z += vz / 60.0;
  }
  RawFlightSample end;
  end.t = 70.0;
  end.position = {14000.0, 0.0, z};
  end.velocity = Vec3{200.0, 0.0, vz};
  end.phase = MissionPhase::Ended;
  p.push(end);
  const auto report = p.snapshot().lastReport;
  REQUIRE(report);
  CHECK(report->stats.hasSustainedData);
  CHECK(report->stats.maxG > report->stats.sustainedMaxG + 2.0);  // the bump: peak only
  REQUIRE(report->windowed.size() == 5);
  CHECK(report->windowed[4].valid);  // 60 s of a 70 s mission
  CHECK(report->windowed[1].maxAvgG == doctest::Approx(7.0).epsilon(0.02));

  HeadlessImGui im;
  ImGui::GetIO().Fonts->AddFontDefault();
  GChart chart;
  bool open = true;
  int highlighted = 0;
  // Sweep the mouse over the whole window: every row and cell gets hovered at some point.
  for (int y = 0; y < 1080; y += 12) {
    for (int x = 400; x < 1520; x += 160) {
      ImGui::GetIO().AddMousePosEvent(static_cast<float>(x), static_cast<float>(y));
      ImGuiIO& io = ImGui::GetIO();
      io.DeltaTime = 1.0f / 60.0f;
      ImGui::NewFrame();
      drawMissionSummary(*report, SummaryContext{}, chart, open, nullptr);
      ImGui::Render();
      if (ImDrawData* dd = ImGui::GetDrawData(); dd && dd->Textures)
        for (ImTextureData* tex : *dd->Textures)
          if (tex->Status != ImTextureStatus_OK && tex->Status != ImTextureStatus_Destroyed) tex->SetStatus(ImTextureStatus_OK);
      if (chart.highlightPending()) ++highlighted;
    }
  }
  CHECK(open);
  CHECK(highlighted > 0);
}
