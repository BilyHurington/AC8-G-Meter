#pragma once
// JSON persistence for Settings with debounced auto-save.
#include <filesystem>
#include <string>

#include "config/settings.h"

namespace ft {

// Lenient load: unknown keys are ignored, missing keys keep their defaults, wrong types
// and out-of-range values fall back / clamp. Returns false only if the file is missing or
// not valid JSON (out is then left at defaults). *error receives a readable reason.
bool loadSettings(const std::filesystem::path& path, Settings& out, std::string* error = nullptr);
// Atomic: writes "<path>.tmp" then renames over the target. Creates parent directories.
bool saveSettings(const std::filesystem::path& path, const Settings& settings,
                  std::string* error = nullptr);
std::string settingsToJson(const Settings& settings);
bool settingsFromJson(const std::string& json, Settings& out, std::string* error = nullptr);

// Platform default location:
//   macOS   ~/Library/Application Support/AC8FlightTelemetry/settings.json
//   Windows %APPDATA%\AC8FlightTelemetry\settings.json (the UE4SS mod will pass its own path)
//   other   $XDG_CONFIG_HOME or ~/.config/AC8FlightTelemetry/settings.json
std::filesystem::path defaultSettingsPath();

// UTF-8 text for showing a path in ImGui or an error message. Never use path::string() for
// that: on Windows it throws for characters outside the ANSI code page.
std::string pathToUtf8(const std::filesystem::path& path);

class SettingsStore {
 public:
  explicit SettingsStore(std::filesystem::path path, double debounceS = 0.5);

  void load();                                    // missing/corrupt file -> defaults
  Settings& settings() { return settings_; }      // edit, then call markDirty()
  const Settings& settings() const { return settings_; }
  void markDirty(double now);                     // schedules a save at now + debounce
  void update(double now);                        // saves when the debounce elapses
  void flush();                                   // save immediately if dirty (on exit)
  void restoreDefaults(double now);               // reset to defaults and schedule save

  bool dirty() const { return dirty_; }
  const std::filesystem::path& path() const { return path_; }
  const std::string& lastError() const { return lastError_; }

 private:
  std::filesystem::path path_;
  double debounceS_;
  Settings settings_;
  bool dirty_ = false;
  double saveAt_ = 0.0;
  std::string lastError_;
};

}  // namespace ft
