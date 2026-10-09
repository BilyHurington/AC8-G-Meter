#include "install.h"

#include <windows.h>

#include <bcrypt.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <regex>

#include <nlohmann/json.hpp>

#include "core/path_utf8.h"

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

namespace ft::installer {

namespace {

constexpr std::string_view kUe4ssPrefix = "ue4ss/";
constexpr std::string_view kModPrefix = "mod/";
constexpr const char* kManifest = "install.json";
constexpr const char* kUserConfig = "Scripts/config.lua";  // shipped, but the player may edit it
constexpr const char* kKeptConfig = "config.lua.keep";     // the edited one while it is replaced
// SHA-256 of the bundled UE4SS.dll (third_party/ue4ss).
constexpr const char* kBundledUe4ssSha256 = "680a026890abb4d0df2211251f8defc1681a584275f1521dcc0fe30af480006f";
// DLL names a UE4SS proxy loader can have in Win64 (dwmapi.dll for the AC8 builds; others for
// older UE4SS layouts).
constexpr std::array<const wchar_t*, 9> kProxyNames = {L"dwmapi.dll",   L"xinput1_3.dll", L"xinput1_4.dll",
                                                       L"dxgi.dll",     L"d3d11.dll",     L"d3d12.dll",
                                                       L"version.dll",  L"winmm.dll",     L"dinput8.dll"};

// The mod folder's program files are the top-level entries of the payload's "mod/" part (never
// taken from install.json, which a player could edit); everything else there is the player's
// data (settings.json, missions\, recordings\, logs, probe files). These are always included.
constexpr std::array<const char*, 7> kDefaultProgramEntries = {
    "Scripts", "assets", "enabled.txt", "README.txt", "THIRD_PARTY_NOTICES.txt", "UE4SS_LICENSE.txt",
    "UE4SS_THIRD_PARTY_LICENSES.txt"};

std::string readText(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string_view bytes(const PayloadFile& f) {
  return {reinterpret_cast<const char*>(f.data.data()), f.data.size()};
}

std::string withoutBom(std::string s) {
  if (s.starts_with("\xEF\xBB\xBF")) s.erase(0, 3);
  return s;
}

// `base` + a relative path from the payload or install.json. Only plain relative paths are
// accepted (no root, no "..", nothing empty): those files are trusted to stay inside `base`.
std::optional<fs::path> under(const fs::path& base, std::string_view rel) {
  const fs::path p = pathFromUtf8(rel);
  if (rel.empty() || p.has_root_name() || p.has_root_directory()) return std::nullopt;
  for (const fs::path& part : p)
    if (part.empty() || part == "." || part == "..") return std::nullopt;
  return (base / p).make_preferred();
}

// Writes next to the target, then replaces it: never a half-written file.
bool writeFile(const fs::path& p, std::string_view data, std::string* error) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  fs::path tmp = p;
  tmp += L".ac8tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out) {
      *error = "cannot write " + pathToUtf8(p);
      return false;
    }
  }
  if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    fs::remove(tmp, ec);
    *error = "cannot replace " + pathToUtf8(p) + " (in use, or no permission?)";
    return false;
  }
  return true;
}

std::string sha256Hex(std::string_view data) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  std::array<unsigned char, 32> digest{};
  std::string hex;
  if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
    if (BCRYPT_SUCCESS(BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                                  static_cast<ULONG>(data.size()), digest.data(), static_cast<ULONG>(digest.size()))))
      for (const unsigned char b : digest) {
        char two[3];
        std::snprintf(two, sizeof two, "%02x", b);
        hex += two;
      }
    BCryptCloseAlgorithmProvider(alg, 0);
  }
  return hex;
}

// A child of `dir` by name, case-insensitively, with its real spelling ("UE4SS" or "ue4ss").
fs::path findChild(const fs::path& dir, const std::wstring& name) {
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (_wcsicmp(e.path().filename().c_str(), name.c_str()) == 0) return e.path();
  return {};
}

// UE4SS's proxy describes itself as "UE4SS Injection Proxy" in its version resource.
bool isUe4ssProxy(const fs::path& dll) {
  DWORD ignored = 0;
  const DWORD size = GetFileVersionInfoSizeW(dll.c_str(), &ignored);
  if (size == 0) return false;
  std::vector<unsigned char> info(size);
  if (!GetFileVersionInfoW(dll.c_str(), 0, size, info.data())) return false;
  struct Translation {
    WORD language, codePage;
  };
  Translation* translations = nullptr;
  UINT bytesLen = 0;
  if (!VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &bytesLen))
    return false;
  for (UINT i = 0; i < bytesLen / sizeof(Translation); ++i)
    for (const wchar_t* key : {L"ProductName", L"FileDescription"}) {
      wchar_t query[64];
      swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\%s", translations[i].language, translations[i].codePage, key);
      wchar_t* value = nullptr;
      UINT len = 0;
      if (VerQueryValueW(info.data(), query, reinterpret_cast<void**>(&value), &len) && value &&
          std::wstring_view(value).find(L"UE4SS") != std::wstring_view::npos)
        return true;
    }
  return false;
}

Ue4ssState inspectUe4ss(const fs::path& win64) {
  Ue4ssState s;
  const fs::path dwmapi = findChild(win64, L"dwmapi.dll");
  s.foreignProxy = !dwmapi.empty() && !isUe4ssProxy(dwmapi);
  for (const wchar_t* name : kProxyNames)
    if (const fs::path p = findChild(win64, name); !p.empty() && isUe4ssProxy(p)) {
      s.loaderFound = true;
      s.loaderName = p.filename().wstring();
      break;
    }
  // How players switch UE4SS off for online play: the loader renamed, e.g. dwmapi.dll.off.
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(win64, ec)) {
    const std::wstring name = e.path().filename().wstring();
    if (name.size() > 11 && _wcsnicmp(name.c_str(), L"dwmapi.dll.", 11) == 0 && !name.ends_with(L".ac8tmp"))
      s.loaderSwitchedOff = true;
  }

  // Where the proxy looks: the folder in override.txt (relative to Win64 or absolute), else
  // "ue4ss", else Win64 itself (older layouts).
  std::vector<fs::path> candidates;
  if (const fs::path o = findChild(win64, L"override.txt"); !o.empty()) {
    std::string name = withoutBom(readText(o));
    name.erase(name.find_last_not_of(" \t\r\n") + 1);
    name.erase(0, name.find_first_not_of(" \t\r\n"));
    if (!name.empty()) candidates.push_back((win64 / pathFromUtf8(name)).make_preferred());
  }
  if (const fs::path d = findChild(win64, L"ue4ss"); !d.empty()) candidates.push_back(d);
  candidates.push_back(win64);
  for (const fs::path& dir : candidates) {
    const fs::path dll = findChild(dir, L"UE4SS.dll");
    if (dll.empty()) continue;
    s.present = true;
    s.dir = dir;
    s.knownBuild = sha256Hex(readText(dll)) == kBundledUe4ssSha256;
    if (const fs::path ini = findChild(dir, L"UE4SS-settings.ini"); !ini.empty()) {
      const std::string text = readText(ini);
      static const std::regex kTickOff(R"((^|\n)[ \t]*HookEngineTick[ \t]*=[ \t]*0)");
      static const std::regex kOverride(R"((^|\n)[ \t]*(ModsFolderPath|ControllingModsTxt)[ \t]*=[ \t]*[^ \t\r\n;])");
      static const std::regex kExtra(R"((^|\n)[ \t]*\+?ModsFolderPaths[ \t]*=[ \t]*[^ \t\r\n;])");
      s.engineTickOff = std::regex_search(text, kTickOff);
      s.customModsFolder = std::regex_search(text, kOverride);
      s.extraModsFolders = std::regex_search(text, kExtra);
    }
    break;
  }
  return s;
}

json readManifest(const fs::path& modDir) {
  json m = json::parse(readText(modDir / kManifest), nullptr, false);
  return m.is_object() ? m : json::object();
}

// Type-safe reads: an install.json edited by hand must never stop install or uninstall
// (json::value() throws on a wrong type).
bool boolField(const json& m, const char* key) { return m.contains(key) && m[key].is_boolean() && m[key].get<bool>(); }
std::string stringField(const json& m, const char* key) {
  return m.contains(key) && m[key].is_string() ? m[key].get<std::string>() : std::string();
}

std::vector<std::string> stringList(const json& m, const char* key) {
  std::vector<std::string> out;
  if (m.contains(key) && m[key].is_array())
    for (const auto& v : m[key])
      if (v.is_string()) out.push_back(v.get<std::string>());
  return out;
}

void addUnique(std::vector<std::string>& list, const std::string& value) {
  if (std::find(list.begin(), list.end(), value) == list.end()) list.push_back(value);
}

// Program entries of the mod folder (see kDefaultProgramEntries). Payload paths are validated
// when the payload is read, so these are plain names.
std::vector<std::string> programEntries(const std::vector<PayloadFile>& payload) {
  std::vector<std::string> entries(kDefaultProgramEntries.begin(), kDefaultProgramEntries.end());
  for (const PayloadFile& f : payload)
    if (f.path.starts_with(kModPrefix)) {
      const std::string rel = f.path.substr(kModPrefix.size());
      addUnique(entries, rel.substr(0, rel.find('/')));
    }
  return entries;
}

// The UE4SS files this installer can have put in Win64: install.json's list, limited to what the
// payload contains (so an edited install.json cannot point at the game's own files).
std::vector<fs::path> ourUe4ssFiles(const fs::path& win64, const json& manifest, const std::vector<PayloadFile>& payload) {
  std::vector<fs::path> out;
  for (const std::string& rel : stringList(manifest, "ue4ss_files")) {
    const bool shipped = std::any_of(payload.begin(), payload.end(), [&](const PayloadFile& f) {
      return f.path.starts_with(kUe4ssPrefix) && f.path.substr(kUe4ssPrefix.size()) == rel;
    });
    if (const std::optional<fs::path> p = under(win64, rel); shipped && p) out.push_back(*p);
  }
  return out;
}

// Whether the game's UE4SS.dll is one this installer put there: this payload's, or the one an
// earlier version installed (its SHA-256 is in install.json). Not if someone replaced it with
// another build, which then belongs to whoever put it there.
bool isOurUe4ss(const GameState& game, const std::vector<PayloadFile>& payload, const json& manifest) {
  if (!game.ue4ss.present) return false;
  const std::string current = readText(findChild(game.ue4ss.dir, L"UE4SS.dll"));
  if (sha256Hex(current) == stringField(manifest, "ue4ss_sha256")) return true;
  return std::any_of(payload.begin(), payload.end(), [&](const PayloadFile& f) {
    return f.path.starts_with(kUe4ssPrefix) && pathFromUtf8(f.path).filename() == L"UE4SS.dll" && bytes(f) == current;
  });
}

// Keeps an edited config.lua as config.lua.keep (install() puts it back). With no config.lua
// (removed by an interrupted install) an existing config.lua.keep is the edited one and stays;
// otherwise config.lua is the newest and decides.
bool keepEditedConfig(const fs::path& modDir, const json& manifest, std::string* error) {
  std::error_code ec;
  const fs::path config = modDir / "Scripts" / "config.lua";
  const fs::path kept = modDir / kKeptConfig;
  if (!fs::exists(config, ec)) return true;
  const std::string current = readText(config);
  const std::string shippedHash = stringField(manifest, "config_sha256");
  if (!shippedHash.empty() && sha256Hex(current) == shippedHash) {
    fs::remove(kept, ec);  // the shipped one: nothing to keep
    return true;
  }
  return writeFile(kept, current, error);
}

void removeTree(const fs::path& p) {
  std::error_code ec;
  fs::remove_all(p, ec);
}

// Removes `dir` and empty parents up to (excluding) `stop`.
void removeEmptyDirs(fs::path dir, const fs::path& stop) {
  std::error_code ec;
  while (!dir.empty() && dir != stop && fs::is_directory(dir, ec) && fs::is_empty(dir, ec)) {
    fs::remove(dir, ec);
    dir = dir.parent_path();
  }
}

std::vector<std::string> otherMods(const fs::path& modsDir) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(modsDir, ec)) {
    if (!e.is_directory()) continue;
    const std::wstring name = e.path().filename().wstring();
    if (_wcsicmp(name.c_str(), L"AC8GMeter") == 0 || _wcsicmp(name.c_str(), L"shared") == 0) continue;
    out.push_back(pathToUtf8(e.path().filename()));
  }
  return out;
}

// EasyAntiCheat\AC8_Offline.json: the game's Settings.json allowing the null client (offline
// play with UE4SS), selected by the Steam launch option. Created if missing; rewritten when
// `refresh` (ours, and the game's Settings.json may have changed with a patch). Problems are
// notes, not failures: the mod itself is installed either way.
bool writeOfflineSettings(const fs::path& root, bool refresh, Result& r) {
  const fs::path eac = findChild(root, L"EasyAntiCheat");
  const fs::path settings = eac.empty() ? fs::path() : findChild(eac, L"Settings.json");
  if (settings.empty()) {
    r.notes.push_back("warning: EasyAntiCheat\\Settings.json not found; offline settings not written");
    return false;
  }
  const bool exists = !findChild(eac, pathFromUtf8(kOfflineSettings).wstring()).empty();
  if (exists && !refresh) return false;
  json doc = json::parse(withoutBom(readText(settings)), nullptr, false);
  if (!doc.is_object()) {
    r.notes.push_back("warning: EasyAntiCheat\\Settings.json is not valid JSON; offline settings not written");
    return false;
  }
  doc["allow_null_client"] = "true";
  std::string error;
  if (!writeFile(eac / kOfflineSettings, doc.dump(1, '\t') + "\n", &error)) {
    r.notes.push_back("warning: " + error);
    return false;
  }
  r.notes.push_back(std::string(exists ? "updated" : "created") + " EasyAntiCheat\\" + kOfflineSettings);
  return true;
}

}  // namespace

fs::path GameState::modDir() const {
  const fs::path ue4ssDir = ue4ss.present ? ue4ss.dir : win64 / "UE4SS";  // the bundled layout
  const fs::path mods = findChild(ue4ssDir, L"Mods");
  return (mods.empty() ? ue4ssDir / "Mods" : mods) / kModFolder;
}

GameState inspectGame(const fs::path& root) {
  GameState g;
  g.root = root;
  g.win64 = root / "Game" / "Binaries" / "Win64";
  std::error_code ec;
  g.valid = fs::exists(g.win64 / "AceCombat8.exe", ec);
  if (!g.valid) return g;
  g.ue4ss = inspectUe4ss(g.win64);
  const fs::path modDir = g.modDir();
  g.modInstalled = fs::exists(modDir / "Scripts" / "main.lua", ec) || fs::exists(modDir / kManifest, ec);
  if (g.modInstalled) g.installedVersion = stringField(readManifest(modDir), "version");
  return g;
}

bool isGameRunning() {
  const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W e{};
  e.dwSize = sizeof e;
  bool running = false;
  for (BOOL ok = Process32FirstW(snap, &e); ok && !running; ok = Process32NextW(snap, &e))
    running = _wcsicmp(e.szExeFile, L"AceCombat8.exe") == 0 || _wcsicmp(e.szExeFile, L"start_protected_game.exe") == 0;
  CloseHandle(snap);
  return running;
}

Blocker installBlocker(const GameState& game) {
  if (!game.valid) return Blocker::NotGame;
  if (!game.ue4ss.present && game.ue4ss.foreignProxy) return Blocker::ForeignLoader;
  if (game.ue4ss.present && game.ue4ss.customModsFolder) return Blocker::CustomModsFolder;
  return Blocker::None;
}

Result install(const GameState& game, const std::vector<PayloadFile>& payload, std::string_view version) {
  Result r;
  switch (installBlocker(game)) {
    case Blocker::None: break;
    case Blocker::NotGame: r.error = "not an ACE COMBAT 8 folder"; return r;
    case Blocker::ForeignLoader: r.error = "Game\\Binaries\\Win64\\dwmapi.dll belongs to another mod loader"; return r;
    case Blocker::CustomModsFolder: r.error = "UE4SS-settings.ini sets a custom mods folder"; return r;
  }
  const fs::path modDir = game.modDir();
  json manifest = readManifest(modDir);  // an update keeps what earlier installs recorded
  auto saveManifest = [&] { return writeFile(modDir / kManifest, manifest.dump(2) + "\n", &r.error); };

  // 1. UE4SS: the bundled one if the game has none; ours is repaired; anyone else's is kept.
  //    Someone who replaced our UE4SS.dll with another build owns that UE4SS from then on.
  bool ours = boolField(manifest, "ue4ss_installed");
  if (ours && game.ue4ss.present && !isOurUe4ss(game, payload, manifest)) {
    ours = false;
    manifest["ue4ss_installed"] = false;
    r.notes.push_back("UE4SS was replaced by another build; AC8 G-Meter no longer manages it");
  }
  if (!game.ue4ss.present || ours) {
    std::vector<std::string> files = stringList(manifest, "ue4ss_files");
    int written = 0;
    for (const PayloadFile& f : payload) {
      if (!f.path.starts_with(kUe4ssPrefix)) continue;
      const std::string rel = f.path.substr(kUe4ssPrefix.size());
      const std::optional<fs::path> target = under(game.win64, rel);
      if (!target) continue;
      std::error_code ec;
      if (fs::exists(*target, ec)) continue;  // repair restores missing files only; leftovers stay theirs
      // A loader the player switched off (dwmapi.dll.off, for online play) stays off.
      if (game.ue4ss.present && game.ue4ss.loaderSwitchedOff && _wcsicmp(target->filename().c_str(), L"dwmapi.dll") == 0)
        continue;
      if (!writeFile(*target, bytes(f), &r.error)) return r;
      addUnique(files, rel);  // only files this installer wrote are ever removed by it
      ++written;
    }
    manifest["ue4ss_installed"] = true;
    manifest["ue4ss_files"] = files;
    if (const fs::path dll = findChild(game.ue4ss.present ? game.ue4ss.dir : game.win64 / "UE4SS", L"UE4SS.dll"); !dll.empty())
      manifest["ue4ss_sha256"] = sha256Hex(readText(dll));
    r.ue4ssOurs = true;
    if (!game.ue4ss.present) {
      r.ue4ssInstalled = true;
      r.notes.push_back("installed UE4SS v3.0.1-1152-ge3ba1016 (" + std::to_string(written) + " files)");
    } else if (written > 0) {
      r.ue4ssRepaired = true;
      r.notes.push_back("restored " + std::to_string(written) + " UE4SS files");
    }
    if (!saveManifest()) return r;  // recorded before anything else can fail
  } else {
    r.notes.push_back("using the UE4SS already installed in " + pathToUtf8(game.ue4ss.dir) +
                      (game.ue4ss.knownBuild ? " (the build AC8 mods use)" : " (another build)"));
    r.engineTickOff = game.ue4ss.engineTickOff;
    if (r.engineTickOff) r.notes.push_back("warning: UE4SS-settings.ini has HookEngineTick = 0; AC8 G-Meter needs 1");
  }
  r.ue4ssDisabled = game.ue4ss.present && !inspectUe4ss(game.win64).loaderFound;  // after any repair
  if (r.ue4ssDisabled) r.notes.push_back("warning: UE4SS is switched off (no loader such as dwmapi.dll in Win64)");

  // 2. The mod: program files replaced, the player's data kept, and config.lua kept if the
  //    player edited it. The edited one waits on disk (config.lua.keep), so a failure in
  //    between does not lose it.
  if (!keepEditedConfig(modDir, manifest, &r.error)) return r;
  std::error_code ec;
  const fs::path kept = modDir / kKeptConfig;
  const bool keepConfig = fs::exists(kept, ec);
  bool configWritten = false;
  for (const std::string& entry : programEntries(payload)) removeTree(modDir / pathFromUtf8(entry));
  for (const PayloadFile& f : payload) {
    if (!f.path.starts_with(kModPrefix)) continue;
    const std::string rel = f.path.substr(kModPrefix.size());
    const std::optional<fs::path> target = under(modDir, rel);
    if (!target) continue;
    const bool isConfig = rel == kUserConfig;
    if (isConfig) manifest["config_sha256"] = sha256Hex(bytes(f));
    const std::string keptText = isConfig && keepConfig ? readText(kept) : std::string();
    if (!writeFile(*target, isConfig && keepConfig ? std::string_view(keptText) : bytes(f), &r.error)) return r;
    configWritten = configWritten || isConfig;
  }
  if (configWritten) fs::remove(kept, ec);
  r.notes.push_back("installed AC8 G-Meter " + std::string(version) + " in " + pathToUtf8(modDir) +
                    (keepConfig ? " (kept your Scripts\\config.lua)" : ""));

  // 3. Offline launch settings for Easy Anti-Cheat (ours are refreshed on every update).
  const bool oursOffline = boolField(manifest, "offline_settings_created");
  if (writeOfflineSettings(game.root, oursOffline, r) && !oursOffline) {
    manifest["offline_settings_created"] = true;
    r.offlineSettingsCreated = true;
  }

  manifest["version"] = std::string(version);  // only once everything is in place
  if (!saveManifest()) return r;
  r.ok = true;
  return r;
}

Result uninstall(const GameState& game, const std::vector<PayloadFile>& payload, bool deleteData) {
  Result r;
  if (!game.valid) {
    r.error = "not an ACE COMBAT 8 folder";
    return r;
  }
  const fs::path modDir = game.modDir();
  const fs::path modsDir = modDir.parent_path();
  std::error_code ec;
  if (!fs::exists(modDir, ec)) {
    r.nothingInstalled = true;
    r.ue4ssRemains = game.ue4ss.present;
    r.notes.push_back("AC8 G-Meter is not installed in " + pathToUtf8(modsDir));
    r.ok = true;
    return r;
  }
  const json manifest = readManifest(modDir);

  // 1. The mod.
  if (deleteData) {
    removeTree(modDir);
    r.notes.push_back("removed " + pathToUtf8(modDir));
  } else {
    if (!keepEditedConfig(modDir, manifest, &r.error)) return r;  // a reinstall puts it back
    for (const std::string& entry : programEntries(payload)) removeTree(modDir / pathFromUtf8(entry));
    fs::remove(modDir / kManifest, ec);
    removeEmptyDirs(modDir, modsDir);
    r.notes.push_back("removed the mod; kept your settings and mission history in " + pathToUtf8(modDir));
  }

  // 2. UE4SS, if this installer put it there (and it is still that build) and no other mod
  //    needs it.
  if (boolField(manifest, "ue4ss_installed") && isOurUe4ss(game, payload, manifest)) {
    const std::vector<std::string> others = otherMods(modsDir);
    if (!others.empty() || game.ue4ss.extraModsFolders) {
      r.ue4ssKeptForOtherMods = true;
      std::string list;
      for (const std::string& m : others) list += (list.empty() ? "" : ", ") + m;
      r.notes.push_back("kept UE4SS: other mods may use it (" +
                        (list.empty() ? std::string("ModsFolderPaths in UE4SS-settings.ini") : list) + ")");
    } else {
      std::vector<std::string> stuck;  // e.g. in use by a running process
      for (const fs::path& p : ourUe4ssFiles(game.win64, manifest, payload)) {
        // A dwmapi.dll that is (readably) not a UE4SS loader any more was put there by someone else.
        if (_wcsicmp(p.filename().c_str(), L"dwmapi.dll") == 0 && std::ifstream(p, std::ios::binary).is_open() &&
            !isUe4ssProxy(p))
          continue;
        if (!fs::remove(p, ec) && fs::exists(p, ec)) stuck.push_back(pathToUtf8(p));
        removeEmptyDirs(p.parent_path(), game.win64);
      }
      if (!stuck.empty()) {
        // Never report UE4SS gone (safe for online play) while its files are still there.
        r.error = "could not remove " + stuck.front() + (stuck.size() > 1 ? " and other files" : "") +
                  " (in use?); close all programs using the game folder and uninstall again";
        r.ue4ssRemains = true;
        return r;
      }
      if (game.ue4ss.dir != game.win64) {
        fs::remove(game.ue4ss.dir / "UE4SS.log", ec);
        removeEmptyDirs(modsDir, game.win64);
        removeEmptyDirs(game.ue4ss.dir, game.win64);
      }
      r.ue4ssRemoved = true;
      r.notes.push_back("removed UE4SS");
      // The offline settings belong to the UE4SS setup: removed with it, if created here.
      if (boolField(manifest, "offline_settings_created")) {
        if (const fs::path eac = findChild(game.root, L"EasyAntiCheat"); !eac.empty()) fs::remove(eac / kOfflineSettings, ec);
        r.notes.push_back("removed EasyAntiCheat\\" + std::string(kOfflineSettings));
      }
    }
  }
  r.ue4ssRemains = game.ue4ss.present && !r.ue4ssRemoved;
  r.ok = true;
  return r;
}

}  // namespace ft::installer