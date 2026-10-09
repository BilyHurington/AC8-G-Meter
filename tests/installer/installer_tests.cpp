// Installer logic against fake ACE COMBAT 8 folders, plus the real AC8GMeter-Installer.exe in
// silent mode (embedded payload, exit codes).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <windows.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/path_utf8.h"
#include "install.h"
#include "payload.h"

namespace fs = std::filesystem;
using namespace ft::installer;

namespace {

std::string readText(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void writeText(const fs::path& p, const std::string& s) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << s;
}

PayloadFile file(std::string path, const std::string& content) {
  return {std::move(path), std::vector<std::uint8_t>(content.begin(), content.end())};
}

// The real UE4SS proxy (its version resource says "UE4SS Injection Proxy").
std::string realProxy() { return readText(ft::pathFromUtf8(FT_UE4SS_PAYLOAD_DIR) / "dwmapi.dll"); }

// A small stand-in for the real payload, same layout.
std::vector<PayloadFile> testPayload(const std::string& config = "return { RecordCsv = true }") {
  return {file("ue4ss/dwmapi.dll", realProxy()),
          file("ue4ss/override.txt", "UE4SS\r\n"),
          file("ue4ss/UE4SS/UE4SS.dll", "bundled ue4ss"),
          file("ue4ss/UE4SS/UE4SS-settings.ini", "[Hooks]\nHookEngineTick = 1\n"),
          file("ue4ss/UE4SS/THIRD_PARTY_LICENSES.txt", "licenses"),
          file("mod/enabled.txt", ""),
          file("mod/Scripts/main.lua", "-- main"),
          file("mod/Scripts/config.lua", config),
          file("mod/Scripts/ac8gmeter.dll", "dll"),
          file("mod/assets/fonts/B612Mono-Regular.ttf", "font"),
          file("mod/README.txt", "readme"),
          file("mod/UE4SS_LICENSE.txt", "mit")};
}

// A fresh fake game folder under the test temp dir.
fs::path makeGame(const std::string& name) {
  const fs::path root = ft::pathFromUtf8(FT_TEST_TMP_DIR) / "installer" / name / "ACE COMBAT 8";
  std::error_code ec;
  fs::remove_all(root.parent_path(), ec);
  writeText(root / "Game/Binaries/Win64/AceCombat8.exe", "exe");
  writeText(root / "EasyAntiCheat/Settings.json",
            "\xEF\xBB\xBF{\n\t\"title\": \"ACE COMBAT 8\",\n\t\"deploymentid\": \"one\"\n}\n");
  return root;
}

// An existing third-party UE4SS: lowercase folder, real proxy, another mod.
void addForeignUe4ss(const fs::path& root, const std::string& settings = "[Hooks]\nHookEngineTick = 1\n") {
  const fs::path win64 = root / "Game/Binaries/Win64";
  writeText(win64 / "dwmapi.dll", realProxy());
  writeText(win64 / "ue4ss/UE4SS.dll", "their ue4ss");
  writeText(win64 / "ue4ss/UE4SS-settings.ini", settings);
  writeText(win64 / "ue4ss/Mods/mods.txt", "OtherMod : 1\r\nKeybinds : 1\r\n");
  writeText(win64 / "ue4ss/Mods/OtherMod/Scripts/main.lua", "-- other");
}

}  // namespace

TEST_CASE("payload: round trip, empty files, damaged data") {
  const std::vector<PayloadFile> files = testPayload();
  const fs::path out = ft::pathFromUtf8(FT_TEST_TMP_DIR) / "installer_payload.bin";
  std::string error;
  REQUIRE_MESSAGE(writePayload(out, files, &error), error);
  std::string bytes = readText(out);
  auto read = [&](const std::string& b, std::vector<PayloadFile>& back) {
    return readPayload({reinterpret_cast<const std::uint8_t*>(b.data()), b.size()}, back, &error);
  };
  std::vector<PayloadFile> back;
  REQUIRE_MESSAGE(read(bytes, back), error);
  REQUIRE(back.size() == files.size());
  for (std::size_t i = 0; i < files.size(); ++i) {
    CHECK(back[i].path == files[i].path);
    CHECK(back[i].data == files[i].data);
  }
  CHECK_FALSE(read(bytes.substr(0, bytes.size() - 3), back));  // truncated
  // A huge path length or file size fails cleanly (no allocation attempted).
  std::string damaged = bytes;
  const std::uint32_t hugePath = 0x7fffffff;
  std::memcpy(damaged.data() + 12, &hugePath, sizeof hugePath);
  CHECK_FALSE(read(damaged, back));
  damaged = bytes;
  const std::uint64_t hugeSize = ~0ull;
  const std::size_t firstSize = 12 + 4 + files[0].path.size();
  std::memcpy(damaged.data() + firstSize, &hugeSize, sizeof hugeSize);
  CHECK_FALSE(read(damaged, back));
  // Unsafe paths are refused.
  for (const char* bad : {"../evil.dll", "mod/../../x", "C:/x", "/abs", "mod//x", "mod\\x"}) {
    REQUIRE(writePayload(out, {file(bad, "x")}, &error));
    CHECK_FALSE(read(readText(out), back));
  }
}

TEST_CASE("installer: fresh install, update, uninstall keeping data") {
  const fs::path root = makeGame("fresh");
  const fs::path win64 = root / "Game/Binaries/Win64";
  GameState g = inspectGame(root);
  REQUIRE(g.valid);
  CHECK_FALSE(g.ue4ss.present);
  CHECK_FALSE(g.modInstalled);
  CHECK(installBlocker(g) == Blocker::None);

  Result r = install(g, testPayload(), "1.0.0");
  REQUIRE_MESSAGE(r.ok, r.error);
  CHECK(r.ue4ssInstalled);
  CHECK(readText(win64 / "UE4SS/UE4SS.dll") == "bundled ue4ss");
  CHECK(fs::exists(win64 / "UE4SS/THIRD_PARTY_LICENSES.txt"));
  const fs::path mod = win64 / "UE4SS/Mods/AC8GMeter";
  CHECK(fs::exists(mod / "Scripts/main.lua"));
  CHECK(fs::exists(mod / "enabled.txt"));
  CHECK_FALSE(fs::exists(win64 / "UE4SS/Mods/mods.txt"));  // enabled.txt starts the mod
  const std::string offline = readText(root / "EasyAntiCheat" / kOfflineSettings);
  CHECK(offline.find("\"allow_null_client\": \"true\"") != std::string::npos);
  CHECK(offline.find("\"deploymentid\": \"one\"") != std::string::npos);

  g = inspectGame(root);
  CHECK(g.ue4ss.present);
  CHECK(g.ue4ss.loaderFound);
  CHECK(g.modInstalled);
  CHECK(g.installedVersion == "1.0.0");

  // Update: data and the edited config survive, program files are replaced, and the offline
  // settings follow a game patch.
  writeText(mod / "settings.json", "{}");
  writeText(mod / "missions/m1.json", "{}");
  writeText(mod / "Scripts/config.lua", "return { RecordCsv = false }");
  writeText(mod / "Scripts/old_file.lua", "stale");
  writeText(root / "EasyAntiCheat/Settings.json", "{\"title\": \"ACE COMBAT 8\", \"deploymentid\": \"two\"}");
  r = install(g, testPayload(), "1.1.0");
  REQUIRE_MESSAGE(r.ok, r.error);
  CHECK_FALSE(r.ue4ssInstalled);
  CHECK_FALSE(r.ue4ssRepaired);
  CHECK(readText(mod / "Scripts/config.lua") == "return { RecordCsv = false }");
  CHECK(fs::exists(mod / "missions/m1.json"));
  CHECK(fs::exists(mod / "settings.json"));
  CHECK_FALSE(fs::exists(mod / "Scripts/old_file.lua"));
  CHECK_FALSE(fs::exists(mod / "config.lua.keep"));
  CHECK(readText(root / "EasyAntiCheat" / kOfflineSettings).find("\"two\"") != std::string::npos);
  CHECK(inspectGame(root).installedVersion == "1.1.0");

  // Uninstall, keeping data: UE4SS (ours, no other mods) and the offline settings go.
  r = uninstall(inspectGame(root), testPayload(), false);
  REQUIRE_MESSAGE(r.ok, r.error);
  CHECK(r.ue4ssRemoved);
  CHECK_FALSE(fs::exists(win64 / "dwmapi.dll"));
  CHECK_FALSE(fs::exists(win64 / "override.txt"));
  CHECK_FALSE(fs::exists(win64 / "UE4SS/UE4SS.dll"));
  CHECK_FALSE(fs::exists(mod / "Scripts"));
  CHECK_FALSE(fs::exists(mod / "UE4SS_LICENSE.txt"));  // program files recorded at install
  CHECK_FALSE(fs::exists(mod / "install.json"));
  CHECK(fs::exists(mod / "missions/m1.json"));
  CHECK_FALSE(fs::exists(root / "EasyAntiCheat" / kOfflineSettings));
  CHECK(fs::exists(root / "EasyAntiCheat/Settings.json"));
  CHECK(fs::exists(win64 / "AceCombat8.exe"));

  // Reinstall over the kept data, then uninstall everything.
  REQUIRE(install(inspectGame(root), testPayload(), "1.1.0").ok);
  CHECK(fs::exists(mod / "missions/m1.json"));
  r = uninstall(inspectGame(root), testPayload(), true);
  REQUIRE(r.ok);
  CHECK_FALSE(fs::exists(win64 / "UE4SS"));
  CHECK_FALSE(fs::exists(win64 / "dwmapi.dll"));
  r = uninstall(inspectGame(root), testPayload(), true);
  CHECK(r.ok);
  CHECK(r.nothingInstalled);
}

TEST_CASE("installer: an unedited config.lua takes the new defaults") {
  const fs::path root = makeGame("config");
  REQUIRE(install(inspectGame(root), testPayload("v1 defaults"), "1.0.0").ok);
  REQUIRE(install(inspectGame(root), testPayload("v2 defaults"), "1.1.0").ok);
  CHECK(readText(inspectGame(root).modDir() / "Scripts/config.lua") == "v2 defaults");
}

TEST_CASE("installer: a failed update keeps the record and the edited config") {
  const fs::path root = makeGame("locked");
  REQUIRE(install(inspectGame(root), testPayload(), "1.0.0").ok);
  const fs::path mod = inspectGame(root).modDir();
  writeText(mod / "Scripts/config.lua", "edited");
  // A file the update must replace is in use.
  const HANDLE lock = CreateFileW((mod / "README.txt").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  REQUIRE(lock != INVALID_HANDLE_VALUE);
  const Result failed = install(inspectGame(root), testPayload(), "1.1.0");
  CloseHandle(lock);
  CHECK_FALSE(failed.ok);
  CHECK(fs::exists(mod / "install.json"));  // UE4SS stays recorded as ours
  CHECK(readText(mod / "config.lua.keep") == "edited");
  // The retry completes with the edited config, and uninstall still removes our UE4SS.
  REQUIRE(install(inspectGame(root), testPayload(), "1.1.0").ok);
  CHECK(readText(mod / "Scripts/config.lua") == "edited");
  const Result r = uninstall(inspectGame(root), testPayload(), true);
  CHECK(r.ue4ssRemoved);
  CHECK_FALSE(fs::exists(root / "Game/Binaries/Win64/dwmapi.dll"));
}

TEST_CASE("installer: repair restores missing files of our UE4SS, nothing else") {
  const fs::path root = makeGame("repair");
  const fs::path win64 = root / "Game/Binaries/Win64";
  REQUIRE(install(inspectGame(root), testPayload(), "1.0.0").ok);
  fs::remove(win64 / "dwmapi.dll");
  fs::remove(win64 / "UE4SS/THIRD_PARTY_LICENSES.txt");
  writeText(win64 / "UE4SS/UE4SS-settings.ini", "[Debug]\nGuiConsoleEnabled = 1\n");  // the player's edit
  const GameState g = inspectGame(root);
  CHECK_FALSE(g.ue4ss.loaderFound);
  const Result r = install(g, testPayload(), "1.0.0");
  REQUIRE_MESSAGE(r.ok, r.error);
  CHECK(r.ue4ssRepaired);
  CHECK_FALSE(r.ue4ssDisabled);
  CHECK(fs::exists(win64 / "dwmapi.dll"));
  CHECK(fs::exists(win64 / "UE4SS/THIRD_PARTY_LICENSES.txt"));
  CHECK(readText(win64 / "UE4SS/UE4SS-settings.ini") == "[Debug]\nGuiConsoleEnabled = 1\n");
}

TEST_CASE("installer: a UE4SS replaced by another build is no longer ours") {
  const fs::path root = makeGame("replaced");
  const fs::path win64 = root / "Game/Binaries/Win64";
  REQUIRE(install(inspectGame(root), testPayload(), "1.0.0").ok);
  writeText(win64 / "UE4SS/UE4SS.dll", "a newer build another mod needs");
  Result r = install(inspectGame(root), testPayload(), "1.1.0");
  REQUIRE_MESSAGE(r.ok, r.error);
  CHECK_FALSE(r.ue4ssOurs);
  CHECK(readText(win64 / "UE4SS/UE4SS.dll") == "a newer build another mod needs");
  r = uninstall(inspectGame(root), testPayload(), true);
  REQUIRE(r.ok);
  CHECK_FALSE(r.ue4ssRemoved);
  CHECK(r.ue4ssRemains);
  CHECK(fs::exists(win64 / "UE4SS/UE4SS.dll"));
  CHECK(fs::exists(win64 / "dwmapi.dll"));
}

TEST_CASE("installer: a loader switched off for online play stays off") {
  const fs::path root = makeGame("switchedoff");
  const fs::path win64 = root / "Game/Binaries/Win64";
  REQUIRE(install(inspectGame(root), testPayload(), "1.0.0").ok);
  fs::rename(win64 / "dwmapi.dll", win64 / "dwmapi.dll.off");
  const GameState g = inspectGame(root);
  CHECK(g.ue4ss.loaderSwitchedOff);
  const Result r = install(g, testPayload(), "1.1.0");
  REQUIRE(r.ok);
  CHECK(r.ue4ssDisabled);
  CHECK_FALSE(fs::exists(win64 / "dwmapi.dll"));
}

TEST_CASE("installer: switched off stays reported even when other files are repaired") {
  const fs::path root = makeGame("switchedoff2");
  const fs::path win64 = root / "Game/Binaries/Win64";
  REQUIRE(install(inspectGame(root), testPayload(), "1.0.0").ok);
  fs::rename(win64 / "dwmapi.dll", win64 / "dwmapi.dll.off");
  fs::remove(win64 / "UE4SS/THIRD_PARTY_LICENSES.txt");
  const Result r = install(inspectGame(root), testPayload(), "1.0.0");
  REQUIRE(r.ok);
  CHECK(r.ue4ssRepaired);
  CHECK(r.ue4ssDisabled);
}

TEST_CASE("installer: uninstall fails loudly when UE4SS cannot be removed") {
  const fs::path root = makeGame("lockeduninstall");
  const fs::path win64 = root / "Game/Binaries/Win64";
  REQUIRE(install(inspectGame(root), testPayload(), "1.0.0").ok);
  const HANDLE lock = CreateFileW((win64 / "dwmapi.dll").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  REQUIRE(lock != INVALID_HANDLE_VALUE);
  const Result r = uninstall(inspectGame(root), testPayload(), true);
  CloseHandle(lock);
  CHECK_FALSE(r.ok);
  CHECK_FALSE(r.ue4ssRemoved);
  CHECK(r.ue4ssRemains);
  CHECK(fs::exists(win64 / "dwmapi.dll"));
}

TEST_CASE("installer: install.json with wrong value types is tolerated") {
  const fs::path root = makeGame("wrongtypes");
  REQUIRE(install(inspectGame(root), testPayload(), "1.0.0").ok);
  const fs::path mod = inspectGame(root).modDir();
  writeText(mod / "install.json", R"({"version": 5, "ue4ss_installed": "yes", "ue4ss_files": 3, "config_sha256": []})");
  const GameState g = inspectGame(root);
  CHECK(g.installedVersion.empty());
  CHECK(install(g, testPayload(), "1.1.0").ok);
  CHECK(uninstall(inspectGame(root), testPayload(), true).ok);
}

TEST_CASE("installer: a leftover loader that was not written is not recorded as ours") {
  const fs::path root = makeGame("leftover");
  const fs::path win64 = root / "Game/Binaries/Win64";
  writeText(win64 / "dwmapi.dll", realProxy());  // UE4SS folder deleted by hand, loader left behind
  const GameState g = inspectGame(root);
  REQUIRE_FALSE(g.ue4ss.present);
  REQUIRE(install(g, testPayload(), "1.0.0").ok);
  REQUIRE(uninstall(inspectGame(root), testPayload(), true).ok);
  CHECK(fs::exists(win64 / "dwmapi.dll"));
  CHECK_FALSE(fs::exists(win64 / "UE4SS/UE4SS.dll"));
}
TEST_CASE("installer: uninstall keeping data keeps an edited config for a reinstall") {
  const fs::path root = makeGame("keepconfig");
  REQUIRE(install(inspectGame(root), testPayload(), "1.0.0").ok);
  const fs::path mod = inspectGame(root).modDir();
  writeText(mod / "Scripts/config.lua", "edited");
  REQUIRE(uninstall(inspectGame(root), testPayload(), false).ok);
  CHECK(readText(mod / "config.lua.keep") == "edited");
  REQUIRE(install(inspectGame(root), testPayload(), "1.0.0").ok);
  CHECK(readText(mod / "Scripts/config.lua") == "edited");
}

TEST_CASE("installer: extra mods folders keep our UE4SS on uninstall") {
  const fs::path root = makeGame("extramods");
  const fs::path win64 = root / "Game/Binaries/Win64";
  REQUIRE(install(inspectGame(root), testPayload(), "1.0.0").ok);
  writeText(win64 / "UE4SS/UE4SS-settings.ini", "[Overrides]\n+ModsFolderPaths = D:\\MoreMods\n");
  const Result r = uninstall(inspectGame(root), testPayload(), true);
  REQUIRE(r.ok);
  CHECK(r.ue4ssKeptForOtherMods);
  CHECK(fs::exists(win64 / "UE4SS/UE4SS.dll"));
}
TEST_CASE("installer: an existing UE4SS is reused and never touched") {
  const fs::path root = makeGame("existing");
  const fs::path win64 = root / "Game/Binaries/Win64";
  addForeignUe4ss(root, "[Hooks]\nHookEngineTick = 0\n");
  writeText(root / "EasyAntiCheat" / kOfflineSettings, "theirs");

  GameState g = inspectGame(root);
  REQUIRE(g.ue4ss.present);
  CHECK(g.ue4ss.loaderFound);
  CHECK_FALSE(g.ue4ss.knownBuild);
  CHECK(g.ue4ss.engineTickOff);
  Result r = install(g, testPayload(), "1.0.0");
  REQUIRE_MESSAGE(r.ok, r.error);
  CHECK_FALSE(r.ue4ssInstalled);
  CHECK(r.engineTickOff);
  CHECK_FALSE(r.offlineSettingsCreated);
  CHECK(readText(win64 / "ue4ss/UE4SS.dll") == "their ue4ss");
  CHECK(readText(win64 / "ue4ss/Mods/mods.txt") == "OtherMod : 1\r\nKeybinds : 1\r\n");
  CHECK(fs::exists(win64 / "ue4ss/Mods/AC8GMeter/Scripts/main.lua"));
  CHECK(readText(root / "EasyAntiCheat" / kOfflineSettings) == "theirs");

  r = uninstall(inspectGame(root), testPayload(), true);
  REQUIRE(r.ok);
  CHECK_FALSE(r.ue4ssRemoved);
  CHECK(readText(win64 / "ue4ss/UE4SS.dll") == "their ue4ss");
  CHECK(fs::exists(win64 / "dwmapi.dll"));
  CHECK(fs::exists(win64 / "ue4ss/Mods/OtherMod/Scripts/main.lua"));
  CHECK(readText(root / "EasyAntiCheat" / kOfflineSettings) == "theirs");
}

TEST_CASE("installer: UE4SS layouts and settings that need care") {
  SUBCASE("override.txt with a BOM and a nested folder") {
    const fs::path root = makeGame("override");
    const fs::path win64 = root / "Game/Binaries/Win64";
    writeText(win64 / "dwmapi.dll", realProxy());
    writeText(win64 / "override.txt", "\xEF\xBB\xBFmods/UE4SS \r\n");
    writeText(win64 / "mods/UE4SS/UE4SS.dll", "nested");
    const GameState g = inspectGame(root);
    REQUIRE(g.ue4ss.present);
    CHECK_FALSE(g.ue4ss.foreignProxy);
    CHECK(fs::equivalent(g.ue4ss.dir, win64 / "mods/UE4SS"));
    REQUIRE(install(g, testPayload(), "1.0.0").ok);
    CHECK(fs::exists(win64 / "mods/UE4SS/Mods/AC8GMeter/enabled.txt"));
  }
  SUBCASE("disabled UE4SS (loader renamed)") {
    const fs::path root = makeGame("disabled");
    addForeignUe4ss(root);
    fs::rename(root / "Game/Binaries/Win64/dwmapi.dll", root / "Game/Binaries/Win64/dwmapi.dll.off");
    const GameState g = inspectGame(root);
    CHECK(g.ue4ss.present);
    CHECK_FALSE(g.ue4ss.loaderFound);
    const Result r = install(g, testPayload(), "1.0.0");
    REQUIRE(r.ok);
    CHECK(r.ue4ssDisabled);
    CHECK_FALSE(fs::exists(root / "Game/Binaries/Win64/dwmapi.dll"));  // not re-enabled behind its back
  }
  SUBCASE("custom mods folder in UE4SS-settings.ini") {
    const fs::path root = makeGame("custommods");
    addForeignUe4ss(root, "[Overrides]\nModsFolderPath = D:\\MyMods\nControllingModsTxt =\n");
    const GameState g = inspectGame(root);
    CHECK(installBlocker(g) == Blocker::CustomModsFolder);
    CHECK_FALSE(install(g, testPayload(), "1.0.0").ok);
    CHECK_FALSE(fs::exists(root / "Game/Binaries/Win64/ue4ss/Mods/AC8GMeter"));
  }
  SUBCASE("empty overrides are fine") {
    const fs::path root = makeGame("emptyoverrides");
    addForeignUe4ss(root, "[Overrides]\nModsFolderPath =\nControllingModsTxt = \n");
    CHECK(installBlocker(inspectGame(root)) == Blocker::None);
  }
  SUBCASE("UE4SS.dll directly in Win64 (older layout)") {
    const fs::path root = makeGame("oldlayout");
    const fs::path win64 = root / "Game/Binaries/Win64";
    writeText(win64 / "xinput1_3.dll", realProxy());
    writeText(win64 / "UE4SS.dll", "old");
    const GameState g = inspectGame(root);
    REQUIRE(g.ue4ss.present);
    CHECK(g.ue4ss.loaderFound);
    REQUIRE(install(g, testPayload(), "1.0.0").ok);
    CHECK(fs::exists(win64 / "Mods/AC8GMeter/enabled.txt"));
    CHECK_FALSE(fs::exists(win64 / "dwmapi.dll"));
  }
  SUBCASE("another loader's dwmapi.dll and no UE4SS") {
    const fs::path root = makeGame("foreign");
    writeText(root / "Game/Binaries/Win64/dwmapi.dll", "some other loader");
    const GameState g = inspectGame(root);
    CHECK(g.ue4ss.foreignProxy);
    CHECK(installBlocker(g) == Blocker::ForeignLoader);
    CHECK_FALSE(install(g, testPayload(), "1.0.0").ok);
    CHECK(readText(root / "Game/Binaries/Win64/dwmapi.dll") == "some other loader");
  }
  SUBCASE("not a game folder") {
    const fs::path root = makeGame("notgame");
    CHECK(installBlocker(inspectGame(root.parent_path())) == Blocker::NotGame);
  }
  SUBCASE("invalid EasyAntiCheat\\Settings.json is only a warning") {
    const fs::path root = makeGame("badsettings");
    writeText(root / "EasyAntiCheat/Settings.json", "{ not json");
    const Result r = install(inspectGame(root), testPayload(), "1.0.0");
    CHECK(r.ok);
    CHECK_FALSE(r.offlineSettingsCreated);
    CHECK_FALSE(fs::exists(root / "EasyAntiCheat" / kOfflineSettings));
  }
}

TEST_CASE("installer: a tampered install.json cannot reach outside the mod / Win64") {
  const fs::path root = makeGame("tampered");
  const fs::path win64 = root / "Game/Binaries/Win64";
  REQUIRE(install(inspectGame(root), testPayload(), "1.0.0").ok);
  const fs::path mod = inspectGame(root).modDir();
  writeText(root / "Sentinel/keep.txt", "outside");
  writeText(mod / "settings.json", "{}");
  writeText(mod / "install.json",
            R"({"ue4ss_installed": true, "ue4ss_files": ["../../../Sentinel/keep.txt", "", "C:/Windows/x.dll",
                "AceCombat8.exe", "dwmapi.dll"], "mod_entries": ["../../../../../../Sentinel", "", "missions"]})");
  writeText(mod / "missions/m1.json", "{}");
  const Result r = uninstall(inspectGame(root), testPayload(), false);
  REQUIRE(r.ok);
  CHECK(readText(root / "Sentinel/keep.txt") == "outside");
  CHECK(fs::exists(win64 / "AceCombat8.exe"));  // not a file the payload installs
  CHECK(fs::exists(mod / "missions/m1.json"));    // not a program entry of the payload
  CHECK(fs::exists(mod / "settings.json"));
  CHECK_FALSE(fs::exists(win64 / "dwmapi.dll"));  // the valid entry was still honoured
}

TEST_CASE("installer: our UE4SS stays while another mod uses it") {
  const fs::path root = makeGame("shared");
  const fs::path win64 = root / "Game/Binaries/Win64";
  REQUIRE(install(inspectGame(root), testPayload(), "1.0.0").ok);
  writeText(win64 / "UE4SS/Mods/OtherMod/Scripts/main.lua", "-- installed later");
  const Result r = uninstall(inspectGame(root), testPayload(), true);
  REQUIRE(r.ok);
  CHECK(r.ue4ssKeptForOtherMods);
  CHECK(fs::exists(win64 / "UE4SS/UE4SS.dll"));
  CHECK(fs::exists(win64 / "UE4SS/THIRD_PARTY_LICENSES.txt"));  // its notices stay with it
  CHECK(fs::exists(win64 / "dwmapi.dll"));
  CHECK_FALSE(fs::exists(win64 / "UE4SS/Mods/AC8GMeter"));
}

TEST_CASE("installer exe: silent install and uninstall with the embedded payload") {
  if (isGameRunning()) {
    MESSAGE("ACE COMBAT 8 is running: skipped");
    return;
  }
  const fs::path root = makeGame("exe");
  const fs::path win64 = root / "Game/Binaries/Win64";
  auto run = [&](const std::wstring& args) {
    std::wstring cmd = L"\"" + ft::pathFromUtf8(FT_INSTALLER_EXE).wstring() + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    REQUIRE(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi));
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 99;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
  };
  const std::wstring game = L" --game-dir \"" + root.wstring() + L"\"";
  CHECK(run(L"--install" + game) == 0);
  // The real bundled files: UE4SS e3ba1016, its notices and the built mod.
  CHECK(fs::file_size(win64 / "UE4SS/UE4SS.dll") == 20605440);
  CHECK(fs::exists(win64 / "UE4SS/THIRD_PARTY_LICENSES.txt"));
  CHECK(fs::exists(win64 / "UE4SS/LICENSE"));
  CHECK(fs::exists(win64 / "UE4SS/Mods/AC8GMeter/Scripts/ac8gmeter.dll"));
  CHECK(fs::exists(win64 / "UE4SS/Mods/AC8GMeter/THIRD_PARTY_NOTICES.txt"));
  const GameState installed = inspectGame(root);
  CHECK(installed.ue4ss.knownBuild);
  CHECK(installed.ue4ss.loaderFound);
  CHECK(run(L"--uninstall --delete-data" + game) == 0);
  CHECK_FALSE(fs::exists(win64 / "UE4SS"));
  CHECK_FALSE(fs::exists(win64 / "dwmapi.dll"));
  CHECK(run(L"--install --game-dir \"" + root.parent_path().wstring() + L"\"") == 2);  // not a game folder
  CHECK(run(L"--install --uninstall" + game) == 4);
  CHECK(run(L"--install --game-dir") == 4);
  CHECK(run(L"--bogus" + game) == 4);
}
