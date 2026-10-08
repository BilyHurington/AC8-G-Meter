// Headless ImGui run of the whole UI: no window, no GPU. Mimics imgui_impl_null so ImGui 1.92's
// dynamic font atlas works (textures are "created" by flipping their status).
#include <doctest/doctest.h>

#include <filesystem>
#include <memory>

#include <imgui.h>

#include "config/hotkey_names.h"
#include "ui/hotkey_input.h"
#include "ui/layout.h"
#include "ui/overlay_layer.h"
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
    r->stats.timeInGBinS = {1, 2, 30, 50, 20, 10, 5, 0};
    for (int i = 0; i < 1250; ++i) r->gTimeline.push_back(static_cast<float>(1.0 + 5.0 * ((i % 100) / 100.0)));
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
