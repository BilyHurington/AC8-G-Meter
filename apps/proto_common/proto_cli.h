#pragma once
// Command line and file locations shared by the prototype hosts (macOS, Windows).
#include <filesystem>
#include <string>
#include <vector>

#include "proto_app.h"

namespace ft {

struct ProtoCommandLine {
  std::filesystem::path settingsPath;  // empty = platform default
  std::filesystem::path missionsDir;   // empty = next to the settings file
  std::filesystem::path screenshotPath;  // non-empty = render `frames` frames, capture, exit
  int frames = 120;
  int width = 1600, height = 900;  // window size (points on macOS, pixels on Windows)
  ProtoConfig proto;
};

// Parses the arguments after the program name, given as UTF-8. False on an unknown option,
// a missing value or a malformed one; `out` is then incomplete.
bool parseProtoCommandLine(const std::vector<std::string>& args, ProtoCommandLine& out);
const char* protoUsage();  // option summary, without the program name

// Fills the ProtoConfig paths: settings (default location: migrates the pre-rename app
// directory first), mission history, assets and replays (next to the executable, else the
// source tree), and the CSV recording directory (`desktopDir` if it exists, else the temp
// directory). Messages for the user (migration, errors) go to stderr.
void resolveProtoPaths(ProtoCommandLine& cl, const std::filesystem::path& exeDir,
                       const std::filesystem::path& sourceDir, const std::filesystem::path& desktopDir);

}  // namespace ft
