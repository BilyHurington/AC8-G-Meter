#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "config/hotkey_names.h"
#include "config/settings_store.h"

using namespace ft;
namespace fs = std::filesystem;

namespace {

fs::path tmpFile(const char* name) {
  const fs::path p = fs::path(FT_TEST_TMP_DIR) / "ui" / name;
  std::error_code ec;
  fs::create_directories(fs::path(FT_TEST_TMP_DIR) / "ui", ec);
  fs::remove(p, ec);
  return p;
}

void writeFile(const fs::path& p, const std::string& text) {
  std::ofstream(p, std::ios::binary) << text;
}

}  // namespace

TEST_CASE("settings defaults match the spec") {
  const Settings s;
  CHECK(s.version == Settings::kVersion);
  CHECK(s.overlay.posX == doctest::Approx(0.045));
  CHECK(s.overlay.posY == doctest::Approx(0.16));
  CHECK(s.overlay.fontSizeAt1080p == 16.0f);
  CHECK(s.overlay.textColor == Color::fromHex(0x93F6A8));
  CHECK(s.overlay.opacity == 1.0f);
  CHECK(s.overlay.shadow);
  CHECK_FALSE(s.overlay.background);
  for (int i = 0; i < kMetricCount; ++i) {
    const auto id = static_cast<MetricId>(i);
    const bool expected = id == MetricId::CurrentG || id == MetricId::MaxG || id == MetricId::MinG;
    CHECK_MESSAGE(s.overlay.isVisible(id) == expected, metricKey(id));
  }
  CHECK(s.hotkeys.toggleMenu.toString() == "Ctrl+F8");
  CHECK(s.hotkeys.toggleEditMode.toString() == "Ctrl+F9");
  CHECK(s.summary.autoShow);
  CHECK(s.metrics.gMode == GMode::Nz);
}

TEST_CASE("metric keys are unique and stable") {
  CHECK(std::string(metricKey(MetricId::CurrentG)) == "current_g");
  CHECK(std::string(metricKey(MetricId::Speed)) == "speed");
  for (int a = 0; a < kMetricCount; ++a)
    for (int b = a + 1; b < kMetricCount; ++b)
      CHECK(std::string(metricKey(static_cast<MetricId>(a))) != metricKey(static_cast<MetricId>(b)));
}

TEST_CASE("JSON round trip preserves every field") {
  Settings s;
  s.overlay.posX = 0.3f;
  s.overlay.posY = 0.7f;
  s.overlay.locked = true;
  s.overlay.fontSizeAt1080p = 22.0f;
  s.overlay.lineSpacing = 1.5f;
  s.overlay.textColor = Color::fromHex(0x12AB7F);
  s.overlay.opacity = 0.8f;
  s.overlay.shadow = false;
  s.overlay.shadowAlpha = 0.25f;
  s.overlay.background = true;
  s.overlay.backgroundAlpha = 0.6f;
  s.overlay.visible = {false, true, false, true, true, false, true, true};
  s.metrics.gMode = GMode::Total;
  s.metrics.smoothingTauS = 0.3f;
  s.metrics.highGEnter = 7.5f;
  s.metrics.highGExit = 6.0f;
  s.hotkeys.toggleMenu = Hotkey{"Insert", false, true, false};
  s.hotkeys.toggleEditMode = Hotkey{"K", true, true, true};
  s.summary.autoShow = false;

  const std::string json = settingsToJson(s);
  CHECK(json.find("\"#12AB7F\"") != std::string::npos);
  CHECK(json.find("\"Ctrl+Shift+Alt+K\"") != std::string::npos);
  Settings back;
  REQUIRE(settingsFromJson(json, back));
  CHECK(back == s);
}

TEST_CASE("corrupt JSON yields defaults and false") {
  Settings s;
  s.overlay.posX = 0.9f;
  std::string err;
  CHECK_FALSE(settingsFromJson("{ \"overlay\": { \"pos_x\": 0.5, ", s, &err));
  CHECK(s == Settings{});
  CHECK_FALSE(err.empty());
  CHECK_FALSE(settingsFromJson("[1, 2, 3]", s));
  CHECK(s == Settings{});
}

TEST_CASE("partial and mistyped JSON keeps the other defaults") {
  Settings s;
  REQUIRE(settingsFromJson(R"({
    "overlay": { "pos_x": 0.5, "font_size_1080p": "big", "visible": { "aoa": true, "bogus": true } },
    "metrics": { "g_mode": "total", "high_g_enter": null },
    "unknown_section": { "x": 1 }
  })", s));
  Settings expected;
  expected.overlay.posX = 0.5f;
  expected.overlay.visible[static_cast<int>(MetricId::Aoa)] = true;
  expected.metrics.gMode = GMode::Total;
  CHECK(s == expected);
}

TEST_CASE("out-of-range values are clamped") {
  Settings s;
  REQUIRE(settingsFromJson(R"({
    "overlay": { "pos_x": -3, "pos_y": 7, "font_size_1080p": 500, "line_spacing": 0.2,
                 "opacity": 2, "shadow_alpha": -1, "background_alpha": 9, "text_color": "#zzzzzz" },
    "metrics": { "smoothing_tau_s": 5, "high_g_enter": 20, "high_g_exit": 30 }
  })", s));
  CHECK(s.overlay.posX == 0.0f);
  CHECK(s.overlay.posY == 1.0f);
  CHECK(s.overlay.fontSizeAt1080p == 64.0f);
  CHECK(s.overlay.lineSpacing == 1.0f);
  CHECK(s.overlay.opacity == 1.0f);
  CHECK(s.overlay.shadowAlpha == 0.0f);
  CHECK(s.overlay.backgroundAlpha == 1.0f);
  CHECK(s.overlay.textColor == Color::fromHex(0x93F6A8));  // malformed color ignored
  CHECK(s.metrics.smoothingTauS == 1.0f);
  CHECK(s.metrics.highGEnter == 12.0f);
  CHECK(s.metrics.highGExit == doctest::Approx(11.9f));

  Settings low;
  low.metrics.highGEnter = 1.0f;  // below range
  low.metrics.highGExit = 0.0f;
  low.clamp();
  CHECK(low.metrics.highGEnter == 2.0f);
  CHECK(low.metrics.highGExit == 1.0f);
}

TEST_CASE("unknown hotkey falls back to the default binding") {
  Settings s;
  REQUIRE(settingsFromJson(R"({ "hotkeys": { "toggle_menu": "Ctrl+Banana", "toggle_edit_mode": "shift+f10" } })", s));
  CHECK(s.hotkeys.toggleMenu.toString() == "Ctrl+F8");
  CHECK(s.hotkeys.toggleEditMode.toString() == "Shift+F10");

  Settings direct;
  direct.hotkeys.toggleEditMode.key = "NotAKey";
  direct.clamp();
  CHECK(direct.hotkeys.toggleEditMode.toString() == "Ctrl+F9");
}

TEST_CASE("hotkey parsing") {
  CHECK(parseHotkey("Ctrl+F8")->toString() == "Ctrl+F8");
  CHECK(parseHotkey(" alt + shift + keypad5 ")->toString() == "Shift+Alt+Keypad5");
  CHECK(parseHotkey("F12")->toString() == "F12");
  CHECK_FALSE(parseHotkey("").has_value());
  CHECK_FALSE(parseHotkey("Ctrl+").has_value());
  CHECK_FALSE(parseHotkey("Meta+F1").has_value());
  CHECK(isKnownHotkeyName("PageUp"));
  CHECK_FALSE(isKnownHotkeyName("LeftCtrl"));
}

TEST_CASE("save and load through a file") {
  std::error_code ec;
  fs::remove_all(fs::path(FT_TEST_TMP_DIR) / "ui" / "nested", ec);
  const fs::path p = tmpFile("nested/dir/settings.json");  // parent dirs created by save
  Settings s;
  s.overlay.posX = 0.25f;
  s.overlay.visible[static_cast<int>(MetricId::Speed)] = true;
  std::string err;
  REQUIRE_MESSAGE(saveSettings(p, s, &err), err);
  CHECK(fs::exists(p));
  CHECK_FALSE(fs::exists(fs::path(p.string() + ".tmp")));

  Settings back;
  REQUIRE(loadSettings(p, back));
  CHECK(back == s);

  Settings missing;
  missing.overlay.posX = 0.9f;
  CHECK_FALSE(loadSettings(tmpFile("does_not_exist.json"), missing, &err));
  CHECK(missing == Settings{});
}

TEST_CASE("SettingsStore debounces saves and restores defaults") {
  const fs::path p = tmpFile("store.json");
  SettingsStore store(p, 0.5);
  store.load();  // missing file: defaults, no error
  CHECK(store.settings() == Settings{});
  CHECK(store.lastError().empty());

  store.settings().overlay.posX = 0.4f;
  store.markDirty(10.0);
  store.update(10.3);
  CHECK(store.dirty());
  CHECK_FALSE(fs::exists(p));
  store.settings().overlay.posY = 0.4f;
  store.markDirty(10.3);  // another change pushes the save out
  store.update(10.6);
  CHECK(store.dirty());
  store.update(10.8);
  CHECK_FALSE(store.dirty());
  REQUIRE(fs::exists(p));

  SettingsStore reloaded(p);
  reloaded.load();
  CHECK(reloaded.settings().overlay.posX == 0.4f);
  CHECK(reloaded.settings().overlay.posY == 0.4f);

  reloaded.restoreDefaults(20.0);
  CHECK(reloaded.settings() == Settings{});
  CHECK(reloaded.dirty());
  reloaded.flush();
  CHECK_FALSE(reloaded.dirty());
  Settings onDisk;
  REQUIRE(loadSettings(p, onDisk));
  CHECK(onDisk == Settings{});

  writeFile(p, "not json at all");
  SettingsStore corrupt(p);
  corrupt.load();
  CHECK(corrupt.settings() == Settings{});
  CHECK_FALSE(corrupt.lastError().empty());
}

TEST_CASE("toPipelineConfig applies the user-tunable fields only") {
  Settings s;
  s.metrics.gMode = GMode::Total;
  s.metrics.smoothingTauS = 0.25f;
  s.metrics.highGEnter = 8.0f;
  s.metrics.highGExit = 7.0f;
  PipelineConfig base;
  base.maxGapS = 0.5;
  const PipelineConfig c = s.toPipelineConfig(base);
  CHECK(c.gMode == GMode::Total);
  CHECK(c.smoothingTauS == doctest::Approx(0.25));
  CHECK(c.highGEnter == doctest::Approx(8.0));
  CHECK(c.highGExit == doctest::Approx(7.0));
  CHECK(c.maxGapS == doctest::Approx(0.5));
}

TEST_CASE("default settings path ends in the app folder") {
  const fs::path p = defaultSettingsPath();
  CHECK(p.filename() == "settings.json");
  CHECK(p.parent_path().filename() == "AC8FlightTelemetry");
}
