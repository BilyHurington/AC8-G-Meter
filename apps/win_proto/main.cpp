// Windows standalone prototype host: Win32 window + Direct3D 12 + Dear ImGui.
// Stands in for the game: the UE4SS mod will host the same ft_ui code from AC8's DX12
// Present hook with the same backends (imgui_impl_win32 + imgui_impl_dx12).
//
// Usage: ac8_gmeter_proto [--settings <file.json>] [--scenario <name>] [--size WxH]
//                       [--menu] [--edit] [--history] [--no-debug] [--end-mission-at <s>] [--velocity-noise <m/s>]
//                       [--missions <dir>]
//                       [--screenshot <file.png> [--frames N]]
// --size is in pixels (the process is per-monitor DPI aware).
#include <windows.h>

#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include "d3d12_host.h"
#include "proto_app.h"
#include "proto_cli.h"
#include "wic_png.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace fs = std::filesystem;

namespace {

ft::D3D12Host* g_host = nullptr;  // for WM_SIZE

std::string toUtf8(const wchar_t* s) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string out(static_cast<std::size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
  return out;
}

fs::path executableDir() {
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return fs::current_path();
    if (n < buf.size()) {
      buf.resize(n);
      return fs::path(buf).parent_path();
    }
    buf.resize(buf.size() * 2);  // truncated: long path
  }
}

// Resizes the window so its client area is width x height pixels.
void setClientSize(HWND hwnd, int width, int height) {
  RECT r{0, 0, width, height};
  AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
                           static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), GetDpiForWindow(hwnd));
  SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT WINAPI wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  // The prototype passes all input to ImGui; ProtoApp decides what reaches the fake game.
  if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return 1;
  switch (msg) {
    case WM_SIZE:
      if (g_host && wParam != SIZE_MINIMIZED) g_host->resize(LOWORD(lParam), HIWORD(lParam));
      return 0;
    case WM_SYSCOMMAND:
      if ((wParam & 0xfff0) == SC_KEYMENU) return 0;  // no Alt application menu
      break;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

double secondsNow() {
  static const double frequency = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return static_cast<double>(c.QuadPart) / frequency;
}

// The user's Desktop folder (also when OneDrive or a policy moved it); empty if unknown.
fs::path desktopDir() {
  PWSTR path = nullptr;
  fs::path out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &path))) out = path;
  CoTaskMemFree(path);
  return out;
}

// Runs frames until the window closes or the screenshot is written; returns the exit code.
int runFrames(HWND hwnd, ft::D3D12Host& host, ft::ProtoApp& app, const ft::ProtoCommandLine& args) {
  const ImGuiIO& io = ImGui::GetIO();
  const bool screenshotMode = !args.screenshotPath.empty();
  int frame = 0;
  double prev = secondsNow();
  for (;;) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) return screenshotMode ? 1 : 0;  // closed before the capture
    }
    // Presenting to an occluded window is harmless; screenshot runs must not wait for it
    // (locked screen, remote session).
    if (!screenshotMode && (IsIconic(hwnd) || host.occluded())) {
      Sleep(10);
      continue;
    }
    if (auto size = app.takeResizeRequest()) setClientSize(hwnd, size->first, size->second);

    // Screenshot mode runs on a fixed 60 Hz clock so captures are reproducible.
    const double t = secondsNow();
    const double realDt = screenshotMode ? 1.0 / 60.0 : std::min(t - prev, 0.25);
    const double now = screenshotMode ? frame / 60.0 : t;
    prev = t;

    // "Game thread": source -> pipeline -> LatestValue.
    app.update(realDt);

    // "Render thread": fake game scene, telemetry overlay/menus, debug panel.
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    app.draw(io.DisplaySize.x, io.DisplaySize.y, now);
    ImGui::Render();

    const bool capture = screenshotMode && frame + 1 >= args.frames;
    ft::CapturedImage image;
    host.renderAndPresent(ImGui::GetDrawData(), !screenshotMode, capture ? &image : nullptr);
    if (capture) {
      const std::string path = ft::pathToUtf8(args.screenshotPath);
      std::string error = "frame read-back failed";
      if (image.bgra.empty() ||
          !ft::writePngBgra(args.screenshotPath, image.width, image.height, image.bgra.data(), &error)) {
        std::fprintf(stderr, "FAILED to write %s: %s\n", path.c_str(), error.c_str());
        return 1;
      }
      std::printf("wrote %s (%ux%u px)\n", path.c_str(), image.width, image.height);
      return 0;
    }
    ++frame;
  }
}

// Device, ImGui and the prototype for an existing window; returns the exit code.
int runPrototype(HWND hwnd, const ft::ProtoCommandLine& args) {
  ft::D3D12Host host;
  if (!host.create(hwnd)) {
    std::fprintf(stderr, "Direct3D 12 initialisation failed\n");
    return 1;
  }
  g_host = &host;
  struct ClearHost {
    ~ClearHost() { g_host = nullptr; }  // before `host` is destroyed
  } clearHost;
  // SW_SHOWDEFAULT follows the caller's STARTUPINFO, which may say minimised: a screenshot
  // run needs a real client area.
  ShowWindow(hwnd, args.screenshotPath.empty() ? SW_SHOWDEFAULT : SW_SHOWNOACTIVATE);
  UpdateWindow(hwnd);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // the overlay must never write imgui.ini next to the game
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  int exitCode = 1;
  {
    ft::ProtoApp app(args.proto);
    app.initImGui();
    const bool win32Backend = ImGui_ImplWin32_Init(hwnd);
    if (win32Backend && host.initImGuiBackend())
      exitCode = runFrames(hwnd, host, app, args);
    else
      std::fprintf(stderr, "Dear ImGui backend initialisation failed\n");
    app.shutdown();
    host.shutdownImGuiBackend();
    if (win32Backend) ImGui_ImplWin32_Shutdown();
  }
  ImGui::DestroyContext();
  return exitCode;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  // Paths in messages are UTF-8; restore the console's code page for the user's shell.
  struct Utf8Console {
    UINT saved = GetConsoleOutputCP();
    Utf8Console() { SetConsoleOutputCP(CP_UTF8); }
    ~Utf8Console() { SetConsoleOutputCP(saved); }
  } utf8Console;

  std::vector<std::string> argList;
  for (int i = 1; i < argc; ++i) argList.push_back(toUtf8(argv[i]));
  ft::ProtoCommandLine args;
  if (!ft::parseProtoCommandLine(argList, args)) {
    std::fprintf(stderr, "usage: ac8_gmeter_proto %s", ft::protoUsage());
    return 2;
  }

  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {  // WIC (screenshots), known folders
    std::fprintf(stderr, "CoInitializeEx failed\n");
    return 1;
  }
  // FT_SOURCE_DIR is UTF-8 (/utf-8); path(const char*) would read it in the ANSI code page.
  ft::resolveProtoPaths(args, executableDir(), ft::pathFromUtf8(FT_SOURCE_DIR), desktopDir());
  ImGui_ImplWin32_EnableDpiAwareness();

  const HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof wc;
  wc.style = CS_CLASSDC;
  wc.lpfnWndProc = wndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = L"AC8GMeterProto";
  RegisterClassExW(&wc);
  int exitCode = 1;
  if (HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"AC8 G-Meter — Prototype", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, args.width, args.height, nullptr, nullptr, instance,
                                  nullptr)) {
    setClientSize(hwnd, args.width, args.height);
    exitCode = runPrototype(hwnd, args);
    DestroyWindow(hwnd);
  } else {
    std::fprintf(stderr, "CreateWindowExW failed\n");
  }
  UnregisterClassW(wc.lpszClassName, instance);
  CoUninitialize();
  return exitCode;
}
