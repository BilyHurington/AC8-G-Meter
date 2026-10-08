#include "proto_cli.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "config/settings_store.h"

namespace fs = std::filesystem;

namespace ft {

namespace {

std::string squash(std::string_view s) {  // "Pull-up / push-over" -> "pulluppushover"
  std::string out;
  for (const char c : s)
    if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

fs::path firstExisting(std::initializer_list<fs::path> candidates) {
  std::error_code ec;
  for (const auto& p : candidates)
    if (fs::exists(p, ec)) return p;
  return *candidates.begin();
}

}  // namespace

bool parseProtoCommandLine(const std::vector<std::string>& args, ProtoCommandLine& out) {
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    const bool hasValue = i + 1 < args.size();
    if (arg == "--settings" && hasValue) out.settingsPath = pathFromUtf8(args[++i]);
    else if (arg == "--screenshot" && hasValue) out.screenshotPath = pathFromUtf8(args[++i]);
    else if (arg == "--frames" && hasValue) out.frames = std::max(1, std::atoi(args[++i].c_str()));
    else if (arg == "--missions" && hasValue) out.missionsDir = pathFromUtf8(args[++i]);
    else if (arg == "--menu") out.proto.openMenu = true;
    else if (arg == "--history") out.proto.openHistory = true;
    else if (arg == "--edit") out.proto.editMode = true;
    else if (arg == "--no-debug") out.proto.debugPanel = false;
    else if (arg == "--end-mission-at" && hasValue) out.proto.endMissionAtS = std::atof(args[++i].c_str());
    else if (arg == "--velocity-noise" && hasValue) out.proto.velocityNoiseMS = std::atof(args[++i].c_str());
    else if (arg == "--size" && hasValue) {
      const std::string& v = args[++i];
      const std::size_t x = v.find('x');
      if (x == std::string::npos) return false;
      out.width = std::atoi(v.substr(0, x).c_str());
      out.height = std::atoi(v.substr(x + 1).c_str());
      if (out.width <= 0 || out.height <= 0) return false;
    } else if (arg == "--scenario" && hasValue) {
      const std::string want = squash(args[++i]);
      for (int s = 0; s < static_cast<int>(Scenario::Count); ++s)
        if (squash(scenarioName(static_cast<Scenario>(s))) == want) out.proto.scenario = static_cast<Scenario>(s);
      if (!out.proto.scenario) return false;
    } else {
      return false;
    }
  }
  return true;
}

const char* protoUsage() {
  return "[--settings file.json] [--missions dir] [--scenario name] [--size WxH]\n"
         "          [--menu] [--edit] [--history] [--no-debug] [--end-mission-at seconds] [--velocity-noise m/s]\n"
         "          [--screenshot out.png [--frames N]]\n";
}

void resolveProtoPaths(ProtoCommandLine& cl, const fs::path& exeDir, const fs::path& sourceDir,
                       const fs::path& desktopDir) {
  const bool defaultSettings = cl.settingsPath.empty();
  if (defaultSettings) {
    // Settings and mission history saved before the rename to AC8 G-Meter move along.
    std::string migrateError;
    if (migrateAppDir(legacySettingsPath().parent_path(), defaultSettingsPath().parent_path(), &migrateError))
      std::fprintf(stderr, "moved settings and missions to %s\n", pathToUtf8(defaultSettingsPath().parent_path()).c_str());
    else if (!migrateError.empty())
      std::fprintf(stderr, "%s\n", migrateError.c_str());
  }
  ProtoConfig& p = cl.proto;
  p.settingsPath = defaultSettings ? defaultSettingsPath() : cl.settingsPath;
  // Mission history next to the settings file (a custom --settings keeps its missions with it).
  p.missionsDir = !cl.missionsDir.empty() ? cl.missionsDir
                  : defaultSettings      ? defaultMissionsDir()
                                         : p.settingsPath.parent_path() / "missions";
  // Assets and replays are copied next to the executable post-build; else use the source tree.
  p.assetsDir = firstExisting({exeDir / "assets", sourceDir / "assets"});
  p.replayDirs = {exeDir / "replays", sourceDir / "data" / "replays"};
  std::error_code ec;
  p.recordDir = (!desktopDir.empty() && fs::is_directory(desktopDir, ec)) ? desktopDir : fs::temp_directory_path(ec);
}

}  // namespace ft
