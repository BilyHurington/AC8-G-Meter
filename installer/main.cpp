// AC8GMeter-Installer.exe: put it in the ACE COMBAT 8 folder (Steam's game folder, the one
// with Game\ and EasyAntiCheat\) and run it. Installs AC8 G-Meter, plus the bundled UE4SS if
// the game has none; run again to update or uninstall. Everything it installs is embedded.
//
// Silent mode (tests, scripts): --install | --uninstall [--delete-data] [--game-dir <dir>]
// [--report <file>]. Exit codes: 0 done, 1 failed, 2 not a game folder / blocked, 3 the game is
// running, 4 bad arguments.
#include <windows.h>

#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

#include "core/path_utf8.h"
#include "install.h"
#include "installer_resources.h"

namespace fs = std::filesystem;
using namespace ft::installer;

namespace {

const std::wstring kVersion = ft::pathFromUtf8(FT_APP_VERSION).wstring();

// --- Text (Chinese for Chinese Windows, else English) ------------------------------------

struct Text {
  const wchar_t* title;
  const wchar_t* notFoundMain;
  const wchar_t* notFoundContent;
  const wchar_t* browse;
  const wchar_t* blockedMain;
  const wchar_t* foreignLoader;
  const wchar_t* customModsFolder;
  const wchar_t* runningMain;
  const wchar_t* runningContent;
  const wchar_t* installMain;            // {} = version
  const wchar_t* installBundled;
  const wchar_t* installExisting;
  const wchar_t* installButton;
  const wchar_t* manageMain;             // {} = installed version
  const wchar_t* updateButton;           // {} = new version
  const wchar_t* uninstallButton;
  const wchar_t* uninstallMain;
  const wchar_t* uninstallContent;
  const wchar_t* deleteData;
  const wchar_t* installedMain;
  const wchar_t* installedContent;       // {} = launch option
  const wchar_t* disabledWarning;
  const wchar_t* tickOffWarning;
  const wchar_t* uninstalledMain;
  const wchar_t* uninstalledContent;
  const wchar_t* ue4ssRemains;           // {} = loader file name
  const wchar_t* ue4ssNotOurs;           // {} = loader file name
  const wchar_t* failedMain;
  const wchar_t* failedContent;          // {} = error
  const wchar_t* payloadBroken;
  const wchar_t* details;
  const wchar_t* licensesLink;
};

const Text kZh = {
    L"AC8 G-Meter 安装程序",
    L"没有找到 ACE COMBAT 8",
    L"请把本程序放到游戏根目录（Steam 的“ACE COMBAT 8”文件夹，里面有 Game 和 EasyAntiCheat 文件夹）后再运行，或者手动选择游戏文件夹。",
    L"选择游戏文件夹…",
    L"无法安装",
    L"Game\\Binaries\\Win64\\dwmapi.dll 属于其他加载器，不是 UE4SS。请先移除它，或者先装好 UE4SS，再运行本程序。",
    L"现有 UE4SS 的 UE4SS-settings.ini 设置了自定义 Mods 文件夹（ModsFolderPath / ControllingModsTxt），本程序无法确定 Mod 应该装到哪里。请清空这两项后再运行，或者手动安装。",
    L"请先关闭游戏",
    L"ACE COMBAT 8 正在运行。关闭游戏后点“重试”。",
    L"安装 AC8 G-Meter {}",
    L"将安装：\n• UE4SS v3.0.1 (e3ba1016)：Mod 加载器，AC8 的其他 Mod 也使用这个版本\n• AC8 G-Meter Mod\n• 离线启动配置 EasyAntiCheat\\AC8_Offline.json\n\n仅限离线单人游戏。",
    L"检测到游戏里已经装了 UE4SS，会直接使用它，不做改动。\n将安装 AC8 G-Meter Mod 和离线启动配置。\n\n仅限离线单人游戏。",
    L"安装",
    L"AC8 G-Meter 已安装（版本 {}）",
    L"更新 / 修复\n安装版本 {}，保留设置和任务记录",
    L"卸载\n移除 Mod。UE4SS 只有在是本程序安装、且没有其他 Mod 使用时才会移除",
    L"卸载 AC8 G-Meter？",
    L"卸载后，记得删除 Steam 里设置的启动选项，才能恢复正常联机。",
    L"同时删除任务记录、设置和飞行录制",
    L"安装完成",
    L"还差最后一步：在 Steam 中右键 ACE COMBAT 8 → 属性 → 启动选项，粘贴下面这行（已复制到剪贴板）：\n\n{}\n\n游戏中：Ctrl+F8 打开设置，Ctrl+F9 移动叠加层。\n要恢复联机：删除这个启动选项，然后运行本程序卸载。",
    L"注意：现有 UE4SS 处于禁用状态（Win64 里没有它的加载器 dwmapi.dll），Mod 不会启动。请先重新启用 UE4SS。",
    L"注意：现有 UE4SS 的 UE4SS-settings.ini 里 HookEngineTick = 0，Mod 无法工作。请把它改成 1。",
    L"卸载完成",
    L"如果在 Steam 里设置过启动选项，请把它删除，以便正常联机。",
    L"UE4SS 仍留在游戏里（它不归本程序管理，或者其他 Mod 还在用它）。要联机，除了删除启动选项，还需要停用 UE4SS，例如把 Game\\Binaries\\Win64\\{0} 改名为 {0}.off。",
    L"注意：这个 UE4SS 不归本程序管理，卸载时会保留。要联机，除了删除启动选项，还需要停用它，例如把 Game\\Binaries\\Win64\\{0} 改名为 {0}.off。",
    L"操作失败",
    L"{}\n\n如果游戏装在需要管理员权限的文件夹里，请右键本程序，选择“以管理员身份运行”。",
    L"安装程序已损坏，请重新下载。",
    L"详细信息",
    L"<a href=\"licenses\">第三方软件许可</a>",
};

const Text kEn = {
    L"AC8 G-Meter Setup",
    L"ACE COMBAT 8 not found",
    L"Put this program in the game folder (Steam's \"ACE COMBAT 8\" folder, the one containing Game and EasyAntiCheat) and run it again, or choose the game folder.",
    L"Choose the game folder...",
    L"Cannot install",
    L"Game\\Binaries\\Win64\\dwmapi.dll belongs to another loader, not UE4SS. Remove it, or install UE4SS first, then run this program again.",
    L"The existing UE4SS sets a custom mods folder in UE4SS-settings.ini (ModsFolderPath / ControllingModsTxt), so this program cannot tell where the mod belongs. Clear both settings and run it again, or install manually.",
    L"Close the game first",
    L"ACE COMBAT 8 is running. Close it and click Retry.",
    L"Install AC8 G-Meter {}",
    L"This installs:\n• UE4SS v3.0.1 (e3ba1016), the mod loader other AC8 mods use too\n• the AC8 G-Meter mod\n• the offline launch settings EasyAntiCheat\\AC8_Offline.json\n\nOffline single player only.",
    L"UE4SS is already installed and will be used as it is.\nThis installs the AC8 G-Meter mod and the offline launch settings.\n\nOffline single player only.",
    L"Install",
    L"AC8 G-Meter is installed (version {})",
    L"Update / repair\nInstalls version {}, keeps your settings and mission history",
    L"Uninstall\nRemoves the mod; UE4SS only if this program installed it and no other mod uses it",
    L"Uninstall AC8 G-Meter?",
    L"Afterwards, remove the Steam launch option to play online again.",
    L"Also delete mission history, settings and flight recordings",
    L"Installed",
    L"One last step: in Steam, right-click ACE COMBAT 8 > Properties > Launch Options and paste this line (already copied to the clipboard):\n\n{}\n\nIn game: Ctrl+F8 settings, Ctrl+F9 move the overlay.\nTo play online again: remove the launch option and uninstall with this program.",
    L"Note: the existing UE4SS is disabled (its loader dwmapi.dll is not in Win64), so the mod will not start. Re-enable UE4SS first.",
    L"Note: the existing UE4SS has HookEngineTick = 0 in UE4SS-settings.ini, so the mod cannot run. Set it to 1.",
    L"Uninstalled",
    L"If you set a Steam launch option for the mod, remove it to play online again.",
    L"UE4SS stays in the game (this program does not manage it, or other mods use it). To play online, besides removing the launch option, disable UE4SS, e.g. rename Game\\Binaries\\Win64\\{0} to {0}.off.",
    L"Note: this UE4SS is not managed by this program and stays when uninstalling. To play online, besides removing the launch option, disable it, e.g. rename Game\\Binaries\\Win64\\{0} to {0}.off.",
    L"Something went wrong",
    L"{}\n\nIf the game is in a folder that needs administrator rights, right-click this program and choose \"Run as administrator\".",
    L"The installer is damaged; please download it again.",
    L"Details",
    L"<a href=\"licenses\">Third-party licenses</a>",
};

const Text& text() {
  static const Text& t = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ? kZh : kEn;
  return t;
}

std::wstring wide(const std::string& utf8) { return ft::pathFromUtf8(utf8).wstring(); }

std::vector<PayloadFile> g_payload;

// Writes the license files shipped in the payload to one text file in %TEMP% and opens it.
void openLicenses() {
  std::error_code ec;
  const fs::path out = fs::temp_directory_path(ec) / L"AC8GMeter-THIRD_PARTY_LICENSES.txt";
  std::ofstream file(out, std::ios::binary | std::ios::trunc);
  for (const char* name : {"mod/THIRD_PARTY_NOTICES.txt", "mod/UE4SS_LICENSE.txt", "mod/UE4SS_THIRD_PARTY_LICENSES.txt"})
    for (const PayloadFile& f : g_payload)
      if (f.path == name) {
        file << "==== " << f.path.substr(4) << " ====\r\n\r\n";
        file.write(reinterpret_cast<const char*>(f.data.data()), static_cast<std::streamsize>(f.data.size()));
        file << "\r\n\r\n";
      }
  file.close();
  ShellExecuteW(nullptr, L"open", out.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// --- Dialogs ------------------------------------------------------------------------------

struct Dialog {
  std::wstring main, content, expanded, verification;
  std::vector<std::pair<int, std::wstring>> commandLinks;
  TASKDIALOG_COMMON_BUTTON_FLAGS buttons = TDCBF_OK_BUTTON;
  PCWSTR icon = TD_INFORMATION_ICON;
};

HRESULT CALLBACK onDialogEvent(HWND, UINT msg, WPARAM, LPARAM, LONG_PTR) {
  if (msg == TDN_HYPERLINK_CLICKED) openLicenses();
  return S_OK;
}

int show(const Dialog& d, bool* verified = nullptr) {
  std::vector<TASKDIALOG_BUTTON> links;
  for (const auto& [id, label] : d.commandLinks) links.push_back({id, label.c_str()});
  TASKDIALOGCONFIG c{};
  c.cbSize = sizeof c;
  c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT | TDF_ENABLE_HYPERLINKS |
              (links.empty() ? 0 : TDF_USE_COMMAND_LINKS);
  c.pszWindowTitle = text().title;
  c.pszMainIcon = d.icon;
  c.pszMainInstruction = d.main.c_str();
  c.pszContent = d.content.c_str();
  c.dwCommonButtons = d.buttons;
  c.cButtons = static_cast<UINT>(links.size());
  c.pButtons = links.empty() ? nullptr : links.data();
  if (!links.empty()) c.nDefaultButton = links.front().nButtonID;  // Enter takes the main action
  if (!d.expanded.empty()) {
    c.pszExpandedInformation = d.expanded.c_str();
    c.pszExpandedControlText = text().details;
  }
  if (!d.verification.empty()) c.pszVerificationText = d.verification.c_str();
  c.pszFooter = text().licensesLink;
  c.pfCallback = onDialogEvent;
  int button = IDCANCEL;
  BOOL checked = FALSE;
  if (FAILED(TaskDialogIndirect(&c, &button, nullptr, &checked))) return IDCANCEL;
  if (verified) *verified = checked != FALSE;
  return button;
}

fs::path pickFolder() {
  IFileOpenDialog* dialog = nullptr;
  fs::path picked;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  IShellItem* item = nullptr;
  if (SUCCEEDED(dialog->Show(nullptr)) && SUCCEEDED(dialog->GetResult(&item))) {
    PWSTR path = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) picked = path;
    CoTaskMemFree(path);
    item->Release();
  }
  dialog->Release();
  return picked;
}

void copyToClipboard(const std::wstring& s) {
  if (!OpenClipboard(nullptr)) return;
  EmptyClipboard();
  const std::size_t bytes = (s.size() + 1) * sizeof(wchar_t);
  if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
    if (void* p = GlobalLock(mem)) {
      std::memcpy(p, s.c_str(), bytes);
      GlobalUnlock(mem);
      if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    } else {
      GlobalFree(mem);
    }
  }
  CloseClipboard();
}

// --- Common --------------------------------------------------------------------------------

fs::path executableDir() {
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return fs::current_path();
    if (n < buf.size()) {
      buf.resize(n);
      return fs::path(buf).parent_path();
    }
    buf.resize(buf.size() * 2);
  }
}

bool loadPayload(std::vector<PayloadFile>& out) {
  const HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_PAYLOAD), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
  const HGLOBAL data = res ? LoadResource(nullptr, res) : nullptr;
  const auto* bytes = data ? static_cast<const std::uint8_t*>(LockResource(data)) : nullptr;
  if (!bytes) return false;
  std::string error;
  return readPayload({bytes, SizeofResource(nullptr, res)}, out, &error);
}

std::wstring joinNotes(const Result& r) {
  std::wstring s;
  for (const std::string& n : r.notes) s += (s.empty() ? L"" : L"\n") + wide(n);
  if (!r.error.empty()) s += (s.empty() ? L"" : L"\n") + wide(r.error);
  return s;
}

// --- Silent mode ---------------------------------------------------------------------------

int runSilent(const std::vector<std::wstring>& args) {
  bool doInstall = false, doUninstall = false, deleteData = false;
  fs::path root = executableDir(), report;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const bool hasValue = i + 1 < args.size();
    if (args[i] == L"--install") doInstall = true;
    else if (args[i] == L"--uninstall") doUninstall = true;
    else if (args[i] == L"--delete-data") deleteData = true;
    else if (args[i] == L"--game-dir" && hasValue) root = args[++i];
    else if (args[i] == L"--report" && hasValue) report = args[++i];
    else return 4;
  }
  if (doInstall == doUninstall) return 4;
  const GameState game = inspectGame(root);
  int code = 0;
  Result r;
  if (const Blocker b = doInstall ? installBlocker(game) : (game.valid ? Blocker::None : Blocker::NotGame);
      b != Blocker::None) {
    code = 2;
    r.error = b == Blocker::NotGame ? "not an ACE COMBAT 8 folder" : "blocked (another loader or custom mods folder)";
  } else if (isGameRunning()) {
    code = 3;
    r.error = "the game is running";
  } else {
    r = doUninstall ? uninstall(game, g_payload, deleteData) : install(game, g_payload, FT_APP_VERSION);
    code = r.ok ? 0 : 1;
  }
  if (!report.empty()) {
    std::ofstream out(report, std::ios::binary);
    for (const std::string& n : r.notes) out << n << "\n";
    if (!r.error.empty()) out << "error: " << r.error << "\n";
  }
  return code;
}

// --- Interactive mode ----------------------------------------------------------------------

int runInteractive() {
  const Text& t = text();
  GameState game = inspectGame(executableDir());
  while (!game.valid) {
    Dialog d{t.notFoundMain, t.notFoundContent};
    d.commandLinks = {{100, t.browse}};
    d.buttons = TDCBF_CANCEL_BUTTON;
    d.icon = TD_WARNING_ICON;
    if (show(d) != 100) return 2;
    const fs::path picked = pickFolder();
    if (!picked.empty()) game = inspectGame(picked);
  }
  if (const Blocker b = installBlocker(game); b != Blocker::None && !game.modInstalled) {
    Dialog d{t.blockedMain, b == Blocker::ForeignLoader ? t.foreignLoader : t.customModsFolder};
    d.icon = TD_ERROR_ICON;
    show(d);
    return 2;
  }
  while (isGameRunning()) {
    Dialog d{t.runningMain, t.runningContent};
    d.buttons = TDCBF_RETRY_BUTTON | TDCBF_CANCEL_BUTTON;
    d.icon = TD_WARNING_ICON;
    if (show(d) != IDRETRY) return 3;
  }

  bool doUninstall = false, deleteData = false;
  if (game.modInstalled) {
    const std::wstring installed = game.installedVersion.empty() ? L"?" : wide(game.installedVersion);
    Dialog d{std::vformat(t.manageMain, std::make_wformat_args(installed))};
    d.commandLinks = {{100, std::vformat(t.updateButton, std::make_wformat_args(kVersion))}, {101, t.uninstallButton}};
    d.buttons = TDCBF_CANCEL_BUTTON;
    const int choice = show(d);
    if (choice == 101) doUninstall = true;
    else if (choice != 100) return 0;
  } else {
    Dialog d{std::vformat(t.installMain, std::make_wformat_args(kVersion)),
             game.ue4ss.present ? t.installExisting : t.installBundled};
    d.commandLinks = {{100, t.installButton}};
    d.buttons = TDCBF_CANCEL_BUTTON;
    if (show(d) != 100) return 0;
  }
  if (doUninstall) {
    Dialog d{t.uninstallMain, t.uninstallContent};
    d.verification = t.deleteData;
    d.buttons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
    d.icon = TD_WARNING_ICON;
    if (show(d, &deleteData) != IDOK) return 0;
  }

  const Result r = doUninstall ? uninstall(game, g_payload, deleteData) : install(game, g_payload, FT_APP_VERSION);
  const std::wstring loader = game.ue4ss.loaderName.empty() ? L"dwmapi.dll" : game.ue4ss.loaderName;
  if (!r.ok) {
    const std::wstring error = wide(r.error);
    Dialog d{t.failedMain, std::vformat(t.failedContent, std::make_wformat_args(error)), joinNotes(r)};
    d.icon = TD_ERROR_ICON;
    show(d);
    return 1;
  }
  Dialog d{doUninstall ? t.uninstalledMain : t.installedMain, {}, joinNotes(r)};
  if (doUninstall) {
    d.content = t.uninstalledContent;
    if (r.ue4ssRemains && !game.ue4ss.loaderSwitchedOff)  // already switched off: nothing to do
      d.content += L"\n\n" + std::vformat(t.ue4ssRemains, std::make_wformat_args(loader));
  } else {
    copyToClipboard(kLaunchOption);
    const std::wstring option = kLaunchOption;
    d.content = std::vformat(t.installedContent, std::make_wformat_args(option));
    if (!r.ue4ssOurs && !r.ue4ssDisabled) d.content += L"\n\n" + std::vformat(t.ue4ssNotOurs, std::make_wformat_args(loader));
    for (const auto& [warn, message] : {std::pair{r.ue4ssDisabled, t.disabledWarning}, {r.engineTickOff, t.tickOffWarning}})
      if (warn) {
        d.content += std::wstring(L"\n\n") + message;
        d.icon = TD_WARNING_ICON;
      }
  }
  show(d);
  return 0;
}

int run() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::wstring> args;
  if (argv) {
    args.assign(argv + 1, argv + argc);
    LocalFree(argv);
  }
  const bool silent = !args.empty();
  if (!loadPayload(g_payload)) {
    if (!silent) MessageBoxW(nullptr, text().payloadBroken, text().title, MB_ICONERROR);
    return 1;
  }
  return silent ? runSilent(args) : runInteractive();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  int code = 1;
  try {
    code = run();
  } catch (const std::exception&) {  // e.g. out of memory: never crash without a word
    MessageBoxW(nullptr, text().payloadBroken, text().title, MB_ICONERROR);
  }
  CoUninitialize();
  return code;
}
