#include "config/settings_store.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

#include "config/hotkey_names.h"

namespace ft {
namespace {

using nlohmann::json;

// --- lenient readers: assign only when the key exists with a compatible type ---------

void readFloat(const json& obj, const char* key, float& out) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_number()) return;
  // Read as double first: converting an out-of-range double straight to float is UB.
  // The real per-field ranges are applied afterwards by Settings::clamp().
  const double v = it->get<double>();
  if (std::isfinite(v)) out = static_cast<float>(std::clamp(v, -1.0e6, 1.0e6));
}

void readBool(const json& obj, const char* key, bool& out) {
  const auto it = obj.find(key);
  if (it != obj.end() && it->is_boolean()) out = it->get<bool>();
}

const json* section(const json& root, const char* key) {
  const auto it = root.find(key);
  return (it != root.end() && it->is_object()) ? &*it : nullptr;
}

std::string colorToHex(const Color& c) {
  auto byte = [](float v) {
    return static_cast<unsigned>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
  };
  char buf[8];
  std::snprintf(buf, sizeof buf, "#%02X%02X%02X", byte(c.r), byte(c.g), byte(c.b));
  return buf;
}

// Accepts "#RRGGBB" or "RRGGBB"; leaves `out` unchanged on malformed input.
void readColor(const json& obj, const char* key, Color& out) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_string()) return;
  std::string s = it->get<std::string>();
  if (!s.empty() && s[0] == '#') s.erase(0, 1);
  if (s.size() != 6) return;
  std::uint32_t rgb = 0;
  for (char ch : s) {
    int digit = 0;
    if (ch >= '0' && ch <= '9') digit = ch - '0';
    else if (ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10;
    else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
    else return;
    rgb = (rgb << 4) | static_cast<std::uint32_t>(digit);
  }
  out = Color::fromHex(rgb);
}

void readHotkey(const json& obj, const char* key, Hotkey& out) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_string()) return;
  if (auto hk = parseHotkey(it->get<std::string>())) out = *hk;
}

const char* gModeKey(GMode m) { return m == GMode::Total ? "total" : "nz"; }

}  // namespace

std::string pathToUtf8(const std::filesystem::path& path) {
  // path::string() converts to the ANSI code page on Windows and throws for characters
  // outside it; u8string() is lossless. C++20 returns std::u8string, so copy the bytes.
  // Called from per-frame UI code, so it must not throw (u8string() can, for ill-formed
  // UTF-16 such as an unpaired surrogate in a Windows file name).
  try {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
  } catch (const std::exception&) {
    return "(path not representable as UTF-8)";
  }
}

std::string settingsToJson(const Settings& s) {
  json visible = json::object();
  for (int i = 0; i < kMetricCount; ++i)
    visible[metricKey(static_cast<MetricId>(i))] = s.overlay.visible[static_cast<std::size_t>(i)];

  const OverlaySettings& o = s.overlay;
  json root = {
      {"version", Settings::kVersion},
      {"overlay",
       {{"pos_x", o.posX},
        {"pos_y", o.posY},
        {"locked", o.locked},
        {"font_size_1080p", o.fontSizeAt1080p},
        {"line_spacing", o.lineSpacing},
        {"text_color", colorToHex(o.textColor)},
        {"opacity", o.opacity},
        {"shadow", o.shadow},
        {"shadow_alpha", o.shadowAlpha},
        {"background", o.background},
        {"background_alpha", o.backgroundAlpha},
        {"visible", visible}}},
      {"metrics",
       {{"g_mode", gModeKey(s.metrics.gMode)},
        {"smoothing_tau_s", s.metrics.smoothingTauS},
        {"high_g_enter", s.metrics.highGEnter},
        {"high_g_exit", s.metrics.highGExit}}},
      {"hotkeys",
       {{"toggle_menu", s.hotkeys.toggleMenu.toString()},
        {"toggle_edit_mode", s.hotkeys.toggleEditMode.toString()}}},
      {"summary", {{"auto_show", s.summary.autoShow}}},
  };
  // error_handler_t::replace: never throws on invalid UTF-8 (this runs from the UI frame).
  return root.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
}

bool settingsFromJson(const std::string& text, Settings& out, std::string* error) {
  out = Settings{};
  const json root = json::parse(text, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
  if (root.is_discarded() || !root.is_object()) {
    if (error) *error = root.is_discarded() ? "invalid JSON" : "top level is not a JSON object";
    return false;
  }

  if (const json* o = section(root, "overlay")) {
    OverlaySettings& ov = out.overlay;
    readFloat(*o, "pos_x", ov.posX);
    readFloat(*o, "pos_y", ov.posY);
    readBool(*o, "locked", ov.locked);
    readFloat(*o, "font_size_1080p", ov.fontSizeAt1080p);
    readFloat(*o, "line_spacing", ov.lineSpacing);
    readColor(*o, "text_color", ov.textColor);
    readFloat(*o, "opacity", ov.opacity);
    readBool(*o, "shadow", ov.shadow);
    readFloat(*o, "shadow_alpha", ov.shadowAlpha);
    readBool(*o, "background", ov.background);
    readFloat(*o, "background_alpha", ov.backgroundAlpha);
    if (const json* vis = section(*o, "visible")) {
      for (int i = 0; i < kMetricCount; ++i)
        readBool(*vis, metricKey(static_cast<MetricId>(i)), ov.visible[static_cast<std::size_t>(i)]);
    }
  }
  if (const json* m = section(root, "metrics")) {
    const auto it = m->find("g_mode");
    if (it != m->end() && it->is_string()) {
      const std::string mode = it->get<std::string>();
      if (mode == "total") out.metrics.gMode = GMode::Total;
      else if (mode == "nz") out.metrics.gMode = GMode::Nz;
    }
    readFloat(*m, "smoothing_tau_s", out.metrics.smoothingTauS);
    readFloat(*m, "high_g_enter", out.metrics.highGEnter);
    readFloat(*m, "high_g_exit", out.metrics.highGExit);
  }
  if (const json* h = section(root, "hotkeys")) {
    readHotkey(*h, "toggle_menu", out.hotkeys.toggleMenu);
    readHotkey(*h, "toggle_edit_mode", out.hotkeys.toggleEditMode);
  }
  if (const json* sm = section(root, "summary")) readBool(*sm, "auto_show", out.summary.autoShow);

  out.clamp();
  return true;
}

bool loadSettings(const std::filesystem::path& path, Settings& out, std::string* error) {
  out = Settings{};
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = "cannot open " + pathToUtf8(path);
    return false;
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  return settingsFromJson(buf.str(), out, error);
}

bool saveSettings(const std::filesystem::path& path, const Settings& settings,
                  std::string* error) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (path.has_parent_path()) {
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
      if (error) *error = "cannot create " + pathToUtf8(path.parent_path()) + ": " + ec.message();
      return false;
    }
  }
  fs::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream outFile(tmp, std::ios::binary | std::ios::trunc);
    if (!outFile) {
      if (error) *error = "cannot write " + pathToUtf8(tmp);
      return false;
    }
    outFile << settingsToJson(settings);
    outFile.flush();
    if (!outFile) {
      if (error) *error = "write failed: " + pathToUtf8(tmp);
      return false;
    }
  }
  // rename() replaces the target atomically on POSIX and via MoveFileEx on Windows, so a
  // crash mid-save never leaves a truncated settings file behind. There is no fsync: after a
  // power loss the file may revert to the previous version, which is fine for settings.
  fs::rename(tmp, path, ec);
  if (ec) {
    if (error) *error = "cannot replace " + pathToUtf8(path) + ": " + ec.message();
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

std::filesystem::path defaultSettingsPath() {
  namespace fs = std::filesystem;
  const fs::path leaf = fs::path("AC8FlightTelemetry") / "settings.json";
#if defined(_WIN32)
  // Wide API: %APPDATA% contains the user name, which may not fit the ANSI code page.
  // _wdupenv_s also avoids the C4996 deprecation of getenv/_wgetenv.
  fs::path result = leaf;
  wchar_t* appData = nullptr;
  std::size_t len = 0;
  if (_wdupenv_s(&appData, &len, L"APPDATA") == 0 && appData) {
    if (*appData) result = fs::path(appData) / leaf;
    std::free(appData);
  }
  return result;
#elif defined(__APPLE__)
  if (const char* home = std::getenv("HOME"); home && *home)
    return fs::path(home) / "Library" / "Application Support" / leaf;
  return leaf;
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) return fs::path(xdg) / leaf;
  if (const char* home = std::getenv("HOME"); home && *home) return fs::path(home) / ".config" / leaf;
  return leaf;
#endif
}

// --- SettingsStore ---------------------------------------------------------------------

SettingsStore::SettingsStore(std::filesystem::path path, double debounceS)
    : path_(std::move(path)), debounceS_(debounceS) {}

void SettingsStore::load() {
  std::string error;
  dirty_ = false;
  if (!loadSettings(path_, settings_, &error)) {
    // A missing file is the normal first-run case, not an error worth surfacing.
    std::error_code ec;
    lastError_ = std::filesystem::exists(path_, ec) ? error : std::string{};
    settings_ = Settings{};
    return;
  }
  lastError_.clear();
}

void SettingsStore::markDirty(double now) {
  // Trailing debounce: every change pushes the save out, so dragging a slider writes once.
  dirty_ = true;
  saveAt_ = now + debounceS_;
}

void SettingsStore::update(double now) {
  if (!dirty_ || now < saveAt_) return;
  if (saveSettings(path_, settings_, &lastError_)) {
    dirty_ = false;
    lastError_.clear();
  } else {
    saveAt_ = now + 5.0;  // retry later instead of hammering the disk every frame
  }
}

void SettingsStore::flush() {
  if (!dirty_) return;
  if (saveSettings(path_, settings_, &lastError_)) {
    dirty_ = false;
    lastError_.clear();
  }
}

void SettingsStore::restoreDefaults(double now) {
  settings_ = Settings{};
  markDirty(now);
}

}  // namespace ft
