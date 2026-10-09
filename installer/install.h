#pragma once
// Installs / updates / uninstalls AC8 G-Meter in an ACE COMBAT 8 folder (no UI here).
//
// Payload paths: "ue4ss/<path under Game\Binaries\Win64>" (the bundled UE4SS and its license
// list) and "mod/<path under the mod folder>". The bundled UE4SS is installed only when the
// game has none; an existing UE4SS (from another mod) is reused untouched, so mods keep sharing
// one. UE4SS starts the mod because its folder contains enabled.txt: the shared mods.txt is
// never edited. What was installed is recorded in <mod folder>\install.json, so uninstalling
// removes only that: UE4SS only if this installer put it there and no other mod uses it.
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "payload.h"

namespace ft::installer {

inline constexpr const char* kModFolder = "AC8GMeter";
inline constexpr const char* kOfflineSettings = "AC8_Offline.json";  // community standard name
inline constexpr const wchar_t* kLaunchOption =
    L"cmd /d /c \"set EOS_USE_ANTICHEATCLIENTNULL=1&& %command% -anticheat_settings=AC8_Offline.json\"";

struct Ue4ssState {
  bool present = false;           // UE4SS.dll found
  std::filesystem::path dir;      // the folder with UE4SS.dll (its Mods folder is inside)
  bool loaderFound = false;       // a UE4SS proxy DLL (dwmapi.dll, ...) loads it; else it is off
  std::wstring loaderName;        // that DLL's file name, e.g. dwmapi.dll
  bool loaderSwitchedOff = false;  // a renamed loader (dwmapi.dll.off / .disabled): off for online play
  bool knownBuild = false;        // the bundled build (the one AC8 mods are made for)
  bool engineTickOff = false;     // HookEngineTick = 0: Lua mods cannot tick on the game thread
  bool customModsFolder = false;  // [Overrides] ModsFolderPath / ControllingModsTxt in its settings
  bool extraModsFolders = false;  // ModsFolderPaths: more mods folders, possibly with other mods
  bool foreignProxy = false;      // Win64\dwmapi.dll exists and is not a UE4SS loader
};

struct GameState {
  std::filesystem::path root;   // the folder containing Game\ and EasyAntiCheat\ (Steam's game folder)
  std::filesystem::path win64;  // Game\Binaries\Win64
  bool valid = false;           // Game\Binaries\Win64\AceCombat8.exe exists
  Ue4ssState ue4ss;
  bool modInstalled = false;
  std::string installedVersion;  // from install.json; empty for an install without one
  std::filesystem::path modDir() const;  // where the mod is or will be
};

GameState inspectGame(const std::filesystem::path& root);
bool isGameRunning();

// Why install() refuses, before changing anything.
enum class Blocker { None, NotGame, ForeignLoader, CustomModsFolder };
Blocker installBlocker(const GameState& game);

struct Result {
  bool ok = false;
  std::string error;               // technical detail when !ok
  std::vector<std::string> notes;  // what was done, and warnings (English, for details / log)
  bool ue4ssInstalled = false;     // the bundled UE4SS was installed now
  bool ue4ssOurs = false;          // install: the UE4SS in the game is managed by this installer
  bool ue4ssRepaired = false;      // missing files of our UE4SS were restored
  bool ue4ssDisabled = false;      // UE4SS has no loader (switched off): the mod will not start
  bool engineTickOff = false;      // the existing UE4SS cannot run Lua mods' game-thread loops
  bool ue4ssRemoved = false;
  bool ue4ssKeptForOtherMods = false;
  bool ue4ssRemains = false;       // uninstall: UE4SS is still in the game (others' or shared)
  bool offlineSettingsCreated = false;
  bool nothingInstalled = false;   // uninstall: no AC8 G-Meter found
};

Result install(const GameState& game, const std::vector<PayloadFile>& payload, std::string_view version);
// `deleteData`: also mission history, settings, recordings and logs (else they stay in the
// mod folder for a later reinstall).
// `payload` defines which files are ours (an edited install.json cannot widen that).
Result uninstall(const GameState& game, const std::vector<PayloadFile>& payload, bool deleteData);

}  // namespace ft::installer
