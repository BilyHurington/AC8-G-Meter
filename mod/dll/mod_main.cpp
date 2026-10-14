// ac8gmeter.dll: the functions the Lua sampler (mod/lua/Scripts/main.lua) loads with
// package.loadlib. Both are lua_CFunctions:
//   ac8gmeter_start(protocolVersion, recordCsv[, debugRecording]) -> 1 started, 0 refused
//                                                                    (UE4SS mod thread)
//   ac8gmeter_frame(<LuaField::Count numbers>[, <LuaDebugField::Count numbers>])
//                                               -> 1 accepted, 2 accepted and the debug block is
//                                                  wanted from the next frame on, 0 rejected (game thread)
// Numbers are read through UE4SS's exported Lua wrapper (lua_view.h). UE4SS.dll is
// delay-loaded, and its exports are checked before the first call into it, so a missing or
// incompatible UE4SS leaves the mod inert instead of crashing the game.
#include <windows.h>

#include <bcrypt.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "lua_view.h"
#include "overlay/overlay.h"
#include "flight/lua_frame.h"
#include "runtime/mod_runtime.h"
#include "support/log.h"

namespace fs = std::filesystem;

namespace {

using ft::mod::logf;
using ft::mod::logLine;

constexpr int kStartProtocol = 1;

struct KnownBuild {
  const char* sha256;
  const char* name;
};
// UE4SS builds the wrapper's ABI was checked against (sha256 of UE4SS.dll).
constexpr KnownBuild kKnownUe4ss[] = {
    {"680a026890abb4d0df2211251f8defc1681a584275f1521dcc0fe30af480006f", "v3.0.1-1152-ge3ba1016"},
};

// Set on UE4SS's mod thread (start), read on the game thread (frames).
std::atomic<bool> g_started{false};
DWORD g_gameThread = 0;
ft::mod::ModRuntime* g_runtime = nullptr;  // never destroyed: the module stays loaded with the game

fs::path thisModuleDir() {
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&thisModuleDir), &self);
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(self, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return {};
    if (n < buf.size()) {
      buf.resize(n);
      return fs::path(buf).parent_path();
    }
    buf.resize(buf.size() * 2);
  }
}

void pinThisModule() {
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                     reinterpret_cast<LPCWSTR>(&pinThisModule), &self);
}

// The delay-loaded imports resolve against the UE4SS.dll already in the process. Checked up
// front: a failed delay-load raises an SEH exception in the game thread.
bool ue4ssWrapperAvailable() {
  const HMODULE ue4ss = GetModuleHandleW(L"UE4SS.dll");
  if (!ue4ss) {
    logLine("UE4SS.dll is not loaded in this process");
    return false;
  }
  static constexpr const char* kExports[] = {
      "??0Lua@LuaMadeSimple@RC@@QEAA@PEAUlua_State@@@Z", "??1Lua@LuaMadeSimple@RC@@QEAA@XZ",
      "?get_stack_size@Lua@LuaMadeSimple@RC@@QEBAHXZ",   "?is_number@Lua@LuaMadeSimple@RC@@QEBA_NH@Z",
      "?get_number@Lua@LuaMadeSimple@RC@@QEBANH@Z",      "?set_number@Lua@LuaMadeSimple@RC@@QEBAXN@Z"};
  for (const char* name : kExports)
    if (!GetProcAddress(ue4ss, name)) {
      logf("UE4SS.dll does not export {}: unsupported UE4SS build", name);
      return false;
    }
  return true;
}

std::string sha256Hex(const fs::path& file) {
  std::FILE* f = nullptr;
  if (_wfopen_s(&f, file.c_str(), L"rb") != 0 || !f) return {};
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::string hex;
  if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
      BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
    std::vector<unsigned char> buf(1 << 16);
    bool ok = true;
    for (std::size_t n; ok && (n = std::fread(buf.data(), 1, buf.size(), f)) > 0;)
      ok = BCRYPT_SUCCESS(BCryptHashData(hash, buf.data(), static_cast<ULONG>(n), 0));
    std::array<unsigned char, 32> digest{};
    if (ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0)))
      for (const unsigned char b : digest) {
        char two[3];
        std::snprintf(two, sizeof two, "%02x", b);
        hex += two;
      }
  }
  if (hash) BCryptDestroyHash(hash);
  if (alg) BCryptCloseAlgorithmProvider(alg, 0);
  std::fclose(f);
  return hex;
}

void logUe4ssBuild() {
  const HMODULE ue4ss = GetModuleHandleW(L"UE4SS.dll");
  std::wstring path(MAX_PATH, L'\0');
  const DWORD n = GetModuleFileNameW(ue4ss, path.data(), static_cast<DWORD>(path.size()));
  path.resize(n);
  const std::string hash = sha256Hex(path);
  for (const KnownBuild& b : kKnownUe4ss)
    if (hash == b.sha256) {
      logf("UE4SS {} ({})", b.name, hash);
      return;
    }
  logf("UE4SS build not in the checked list (sha256 {}); the Lua bridge only needs 6 stable exports", hash);
}

// Whether the Easy Anti-Cheat service is running. It runs for the whole session when the game
// is started protected (Steam's normal launch through start_protected_game.exe), and not at
// all when the game executable is started directly.
bool antiCheatServiceRunning() {
  const SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!manager) return true;  // cannot tell: assume protected
  bool running = false;
  if (const SC_HANDLE service = OpenServiceW(manager, L"EasyAntiCheat_EOS", SERVICE_QUERY_STATUS)) {
    SERVICE_STATUS status{};
    running = !QueryServiceStatus(service, &status) || status.dwCurrentState != SERVICE_STOPPED;
    CloseServiceHandle(service);
  }  // not installed: not running
  CloseServiceHandle(manager);
  return running;
}

// Offline play only. Accepted: the community launch option and AC8 Tweaks' launcher (they set
// one of these variables), or any launch without the anti-cheat service (the game executable
// started directly). Logs which one applied.
bool offlineLaunch() {
  for (const wchar_t* name : {L"EOS_USE_ANTICHEATCLIENTNULL", L"AC8TWEAKS_OFFLINE"}) {
    wchar_t* value = nullptr;
    std::size_t len = 0;
    const bool set = _wdupenv_s(&value, &len, name) == 0 && value && std::wstring(value) == L"1";
    std::free(value);
    if (set) {
      logf("offline launch: {}=1", ft::pathToUtf8(name));
      return true;
    }
  }
  if (!antiCheatServiceRunning()) {
    logLine("offline launch: the Easy Anti-Cheat service is not running");
    return true;
  }
  return false;
}

// Pops all numbers off the Lua stack in call order. False if any argument is not a number.
bool readNumbers(const RC::LuaMadeSimple::Lua& lua, std::vector<double>& out) {
  const int n = lua.get_stack_size();
  out.assign(static_cast<std::size_t>(n > 0 ? n : 0), 0.0);
  for (int i = n - 1; i >= 0; --i) {  // get_number removes the value it reads: take the top
    if (!lua.is_number(-1)) return false;
    out[static_cast<std::size_t>(i)] = lua.get_number(-1);
  }
  return true;
}

int returnNumber(lua_State* state, double value) {
  RC::LuaMadeSimple::Lua lua(state);
  lua.set_number(value);
  return 1;
}

// Runs once. The log is already open (ac8gmeter_start).
bool start(const std::vector<double>& args, const fs::path& modDir) {
  if ((args.size() != 2 && args.size() != 3) || args[0] != kStartProtocol) {
    logLine("ac8gmeter_start: unexpected arguments (main.lua and the DLL are from different versions?)");
    return false;
  }
  if (!offlineLaunch()) {
    logLine("not started: Easy Anti-Cheat is running (online-capable launch); see README for offline launch");
    return false;
  }
  logUe4ssBuild();
  ft::mod::ModOptions options;
  options.recordCsv = args[1] != 0.0;
  options.debugRecording = args.size() == 3 && args[2] != 0.0;
  g_runtime = new ft::mod::ModRuntime(modDir, options);
  if (!ft::mod::startOverlay(*g_runtime)) logLine("overlay could not be started; telemetry still runs");
  logf("started (CSV recording {}, debug recording {})", options.recordCsv ? "on" : "off",
       options.debugRecording ? "on" : "off");
  return true;
}

}  // namespace

// Called once while main.lua loads, on UE4SS's mod thread (not the game thread).
extern "C" __declspec(dllexport) int ac8gmeter_start(lua_State* state) {
  static bool attempted = false;
  try {
    if (!attempted) {
      attempted = true;
      // Pinned first: the log's writer thread, the overlay's hooks and the Lua functions must
      // never point at unloaded code, even if UE4SS closes the Lua state (mod reload).
      pinThisModule();
      const fs::path modDir = thisModuleDir().parent_path();  // <mod>/Scripts/ac8gmeter.dll
      ft::mod::openLog(modDir / "ac8gmeter.log");
      logf("AC8 G-Meter starting, mod directory {}", ft::pathToUtf8(modDir));
      if (!ue4ssWrapperAvailable()) return 0;  // nothing can be returned without the wrapper
      std::vector<double> args;
      {
        RC::LuaMadeSimple::Lua lua(state);
        if (!readNumbers(lua, args)) args.clear();
      }
      g_started.store(start(args, modDir), std::memory_order_release);  // publishes g_runtime
    } else if (!ue4ssWrapperAvailable()) {
      return 0;
    }
    return returnNumber(state, g_started.load(std::memory_order_acquire) ? 1.0 : 0.0);
  } catch (const std::exception& e) {
    logf("ac8gmeter_start: {}", e.what());
  } catch (...) {
    logLine("ac8gmeter_start: unknown exception");
  }
  return 0;
}

// Called every tick from LoopInGameThreadAfterFrames, i.e. on the game thread. The first
// call's thread is taken as the game thread; the pipeline is single-threaded.
extern "C" __declspec(dllexport) int ac8gmeter_frame(lua_State* state) {
  try {
    if (!g_started.load(std::memory_order_acquire)) return 0;
    if (g_gameThread == 0) g_gameThread = GetCurrentThreadId();
    if (GetCurrentThreadId() != g_gameThread) {
      static bool reported = false;
      if (!reported) {
        reported = true;
        logf("frame from thread {} ignored (frames come from thread {})", GetCurrentThreadId(), g_gameThread);
      }
      return 0;
    }
    static std::vector<double> values;  // game thread only; reused to avoid an allocation per frame
    {
      RC::LuaMadeSimple::Lua lua(state);
      if (!readNumbers(lua, values)) values.clear();
    }
    std::string error;
    const std::optional<ft::mod::LuaFrame> frame = ft::mod::parseLuaFrame(values, &error);
    if (!frame) {
      static int reported = 0;
      if (reported < 10) {
        ++reported;
        logf("frame rejected: {}", error);
      }
      return returnNumber(state, 0.0);
    }
    g_runtime->onLuaFrame(*frame);
    return returnNumber(state, g_runtime->debugRecordingWanted() ? 2.0 : 1.0);
  } catch (const std::exception& e) {
    logf("ac8gmeter_frame: {}", e.what());
  } catch (...) {
    logLine("ac8gmeter_frame: unknown exception");
  }
  return 0;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }