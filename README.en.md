# AC8 G-Meter

[简体中文](README.md) | **English**

[![build](https://github.com/BilyHurington/AC8-G-Meter/actions/workflows/build.yml/badge.svg)](https://github.com/BilyHurington/AC8-G-Meter/actions/workflows/build.yml)

A G-meter mod for **ACE COMBAT 8: Wings of Theve**. It draws a clean, HUD-styled text overlay with your live G load, peak G, G onset, angle of attack and other flight data. After each sortie it opens a debrief with the mission's full G curve. The game's own HUD is never touched.

- **Accurate live G.** Load factor (Nz) is estimated from the aircraft's position, velocity and attitude. Filtering adapts to the input's sample rate and noise. On clean 60 Hz data the display lags only about 25 ms.
- **High G is never clipped.** There is no magnitude cap. Only glitches that are inconsistent with the motion are rejected, so real high-G readings always come through.
- **Configurable overlay.** Drag it into place and lock it. Set font size, opacity and colour, and choose which metrics to show. Settings save automatically.
- **Debrief and history.** The full-mission G chart can be zoomed and panned. It shows peak and sustained G and the average G over several time windows. Every sortie is saved as JSON plus CSV and can be reopened at any time.

> **Offline single player only.** The mod starts only when Easy Anti-Cheat is not running. Never use mods online.

**Status:** this is the first test build of the in-game UE4SS mod (0.x, prerelease). There are also standalone prototypes for macOS and Windows. They run on simulated flight data and replayable CSV logs, without the game.

## Download and install

Download one of these from [Releases](https://github.com/BilyHurington/AC8-G-Meter/releases):

| File | Use |
|---|---|
| `AC8GMeter-<version>-Installer.exe` | **Recommended.** Put it in the game folder and run it (see below). |
| `AC8GMeter-<version>-Full.zip` | UE4SS + the mod. Extract into the game folder so that its `Game` folder merges with the game's. Only for a game without UE4SS: it would replace an existing UE4SS and its settings. |
| `AC8GMeter-<version>-ModOnly.zip` | The mod only, for a game that already has UE4SS. Copy the `AC8GMeter` folder into `Game\Binaries\Win64\<UE4SS folder>\Mods\`. UE4SS needs `HookEngineTick = 1` in `UE4SS-settings.ini`. |

`SHA256SUMS.txt` holds the checksums of these files.

**The installer.** The game folder is Steam's `ACE COMBAT 8` folder, the one containing `Game` and `EasyAntiCheat`. The installer does the following:

- **UE4SS.** If the game has no UE4SS yet, it installs the bundled **UE4SS v3.0.1 (e3ba1016)**.
  - This is the build AC8 Tweaks, Per-View FOV and Mouse-Aim use, with the same folder layout as Mouse-Aim.
  - An existing UE4SS is used as it is, so mods don't conflict.
- **The mod.** It installs the mod into `Game\Binaries\Win64\<UE4SS folder>\Mods\AC8GMeter`.
  - The `enabled.txt` in the mod folder makes UE4SS load it, so the shared `mods.txt` is never edited.
  - It warns if the existing UE4SS is switched off or has `HookEngineTick = 0`.
  - It refuses to install when a custom Mods folder is configured.
- **Offline settings.** If `EasyAntiCheat\AC8_Offline.json` is missing, it creates it: the game's `Settings.json` plus `"allow_null_client": "true"`. It also copies the Steam launch option to the clipboard.

Run it again for one of these:

- **Update / repair** keeps your settings, mission history and an edited `config.lua`.
- **Uninstall** can optionally delete the mission history too.

What was installed is listed in `install.json` in the mod folder, and uninstalling removes only those files. UE4SS is removed only if the installer put it there and no other mod uses it. The "Third-party licenses" link at the bottom of the window opens all license texts.

**Uninstalling a zip install:**

- **Full:** delete `dwmapi.dll`, `override.txt` and the `UE4SS` folder in `Game\Binaries\Win64\`.
- **ModOnly:** delete the `AC8GMeter` folder.

The zips do not create `AC8_Offline.json`. To make it yourself, copy `EasyAntiCheat\Settings.json` and add `"allow_null_client": "true"`. Or use launch method 2 or 3 below.

## Offline launch

The mod starts only when Easy Anti-Cheat is not active. Any one of these works:

1. **Steam launch option.** Set it in Steam > ACE COMBAT 8 > Properties > Launch Options (the installer copies this line to the clipboard):
   ```
   cmd /d /c "set EOS_USE_ANTICHEATCLIENTNULL=1&& %command% -anticheat_settings=AC8_Offline.json"
   ```
2. **Direct start.** Start `Game\Binaries\Win64\AceCombat8.exe` directly, with a `steam_appid.txt` containing `2288340` next to it. The anti-cheat service then never starts.
3. **AC8 Tweaks.** Use the AC8 Tweaks offline launcher.

To play online again, launch normally from Steam without the launch option, and uninstall the mod (or disable UE4SS).

## Usage

| Keys | Action |
|---|---|
| `Ctrl+F8` | Open / close the settings menu |
| `Ctrl+F9` | Start / finish moving the overlay (drag) |
| `Ctrl+F7` | Read-only state probe (diagnostics, writes `probe-*.txt`) |
| `Ctrl+F1` | Debug panel (standalone prototypes only) |

The hotkeys can be changed in the settings menu. On the macOS prototype, `Cmd` works in place of `Ctrl`. Borderless or windowed display mode is recommended.

**Overlay visibility:**

- By default the overlay shows only during missions. It is hidden in menus and on the results screen, and always shown while you move it. Turn off "Only during missions" in the settings to show it everywhere.
- `Overlay = false` in `Scripts\config.lua` turns the overlay off completely. The game's rendering is not hooked, but G is still computed and missions are still saved and recorded. Use it to check whether a display or stability problem comes from the overlay.

**Files in the mod folder:**

| File | Contents |
|---|---|
| `settings.json` | Settings |
| `missions\` | Mission history |
| `recordings\` | Flight data CSV for each mission. Replay it with `replay_cli`; switch it in `Scripts\config.lua` |
| `ac8gmeter.log` | Log |

**Debug recording** (`Ctrl+F8` settings menu > Diagnostics) is for checking the G calculation.

- Every frame is written to `debug\ac8_debug_*.csv`. Each row has:
  - all raw aircraft data from the game: position, velocity, axes, rotation, world time and frame time, real time, global and aircraft time dilation, movement component velocity, the game's own speed and altitude readouts (when present), and cutscene and input-block state;
  - the samples the sampler feeds the pipeline, and the pipeline's results;
  - unfiltered finite-difference G (the `fd_*` columns).
- The file grows by about 130 MB per hour at 60 fps.
- The menu switch turns off again at the next game start. To record from the start of the game, set `DebugRecording = true` in `Scripts\config.lua`.

`Ctrl+F7` starts or stops the read-only state probe. It writes changes of the relevant aircraft, controller, HUD and game mode fields to `probe-*.txt`, for finding mission-flow signals.

## Known issues

- **Mission detection (first version):**
  - A mission runs while the player controls an aircraft. Paused time does not count.
  - The mission ends, and the debrief opens, once the aircraft has been gone for 3 s (results, menus, loading).
  - Respawning mid-mission does not end the mission.
  - The end sequence and replay after a mission still count as mission time, so the debrief opens late.
  - Quitting a mission is recorded like a finished one.
  - A restart from the pause menu whose aircraft appears within 3 s (paused time excluded) merges with the previous attempt.
  - A cutscene of more than 3 s without the aircraft, unpaused, splits a mission in two.
- **The game's manoeuvres can really reach very high G.** Readings above 100 G in testing match the game's own speed data: they are the game's actual kinematics, not a calculation error. Collision impacts count toward peak G but not toward sustained G.
- **DLSS Frame Generation:** one game crash was seen in testing with DLSS Frame Generation on. The cause is not yet isolated. If the game crashes, try turning Frame Generation off, or set `Overlay = false`.

## Features in detail

- **Live metrics:**
  - Shown by default: current G, MAX G and MIN G. MAX/MIN are peaks and update with live G.
  - Optional:
    - maximum sustained G (SUS G);
    - G onset (G/s);
    - AoA;
    - current high-G time (HI-G): the length of the current high-G stretch, timed on live G;
    - sideslip;
    - speed.
- **Data processing:**
  - Acceleration is estimated by least squares from time-stamped velocity or position, so uneven frame times are handled.
  - Median filtering plus EMA smoothing.
  - Outliers are detected and handled: spikes, stalls, teleports, repeated frames, NaN.
  - **Spike gate for live G.** A velocity sample inconsistent with recent motion is held back until later samples show whether it is real. As a result:
    - a real single-frame G impact is shown in full, about 3 frames late (about 67 ms at 60 Hz);
    - data glitches of 1–3 samples are removed entirely and never shown;
    - the cost is that a real 2-frame collision double spike (for example +13 G then −11 G) cannot be told from a glitch, so it is also removed from live G;
    - changes lasting 4 samples or more are treated as real motion and shown.
- **Overlay:**
  - transparent background, light green `#93F6A8` text with a slight shadow;
  - default position (4.5%, 16%) of the screen, about (86, 173) at 1080p;
  - 20 px font at 1080p, scaled with resolution;
  - mouse input passes through in normal mode;
  - in edit mode it can be dragged. You can lock its position, set font size, line spacing, opacity, colour and background, and choose which metrics to show.
- **Settings menu:** opened with a hotkey. Settings save automatically as JSON and can be reset to defaults.
- **Debrief:** opens automatically when a mission ends. It contains:
  - the G curve of the whole mission;
  - peak and sustained G (maximum, minimum and when they happened);
  - onset extremes, maximum AoA and sustained high-G time;
  - the maximum and minimum of the 1/5/10/30/60 s average G;
  - data quality counts.
- **Peak and sustained values:**
  - **Peak G** (MAX/MIN G on the overlay) is every reading live G has shown. That includes real high-G spikes, with no cap and no delay.
  - **Sustained G** is the highest or lowest level held for at least about W seconds.
    - Jumps and dips shorter than W (collisions, physics jitter) are removed. So even for a real manoeuvre, the sustained value can be lower than the peak: a triangular pull that only touches 8.9 G has a sustained value of 8.2 G.
    - High-G episodes (count, longest, total) are counted on sustained G, so brief spikes never create one.
  - **Setting W:** "Sustained G: ignore bumps shorter than" in the settings menu. The default is 0.2 s; 0 makes sustained equal to peak. The sustained value arrives up to about 2W after live G.
  - **Noisy data:** pre-smoothing widens automatically with the measured noise (W/4 to W/2).
    - Only jumps shorter than about W/2 are then guaranteed to be removed; longer ones are kept.
    - Sustained extremes can sit about 0.1 G beyond the level truly held. For very noisy data, such as 60 fps with 1 m/s velocity noise, it is about 0.35 G.
- **G chart:** recorded at 20 Hz.
  - **Controls:** the mouse wheel zooms around the cursor, dragging pans, and a double click resets.
  - **Hover:** shows G at that moment (mean, minimum, maximum and sustained), onset, AoA, speed and altitude.
  - **Elements:**
    - light band: the minimum–maximum range of live G, keeping single-frame peaks and brief spikes;
    - main line: sustained G;
    - orange dots: peak maximum and minimum;
    - white diamonds: sustained maximum and minimum;
    - orange areas: sustained high-G episodes;
    - onset or AoA can be overlaid on the right axis.
  - **Merged labels:** when the peak and sustained values differ by ≤ 0.05 G and sit close together, they share one label, such as "MAX +8.2 G (sus +8.2)". A label pushed aside is linked back to its marker by a thin line.
- **Average G over window:** this table lists the maximum and minimum of the live-G average for each window length, with start times.
  - Hovering a row highlights that time span on the G chart in the same frame: orange for the maximum, blue for the minimum.
  - If the span is outside the current zoom, the chart pans (or zooms out) to show it when you start hovering.
  - The last recorded cell is usually shorter than 1/20 s and is weighted by its actual length.
  - Windows longer than the flight are not given: a 29.987 s mission has no 30 s value.
- **Mission history:**
  - **Saving:** every mission with G data is saved when it ends.
    - Saving can be switched off in the settings menu. By default the last 50 missions are kept; 0 means no limit.
    - Saving is triggered by the pipeline at mission end, not by the overlay drawing that frame, so back-to-back missions are all saved.
  - **Changing how many are kept:**
    - Lowering "Keep missions" below the number already saved asks first ("N older missions will be deleted at the next save").
    - A typed number takes effect on Enter, Tab or leaving the field, not digit by digit. The −/+ buttons take effect at once.
  - **History window:** open it with "Mission history..." in the settings menu or the debrief.
    - Click a row to reopen that mission's debrief and G chart.
    - Single entries can be deleted (with confirmation).
  - Saving, loading and deleting run on a background thread and never stall the display.

## Mission history files

- **Location:**
  - in-game mod: `missions\` in the mod folder;
  - prototypes: `~/Library/Application Support/AC8GMeter/missions/` (Windows: `%APPDATA%\AC8GMeter\missions\`).
- **Names:** each mission is two files, named from the save time (UTC), for example `20261008-140312_m3`.
  - A name already used within the same second gets `-2`, `-3`, … appended.
  - Over the limit, the oldest missions are deleted, ordered by save time, mission number and suffix.

**`….trace.csv`: the G curve.** It opens directly in spreadsheet software.

- One row per 1/20 s, with columns `t,g_mean,g_min,g_max,g_sustained,onset,aoa,speed,alt,high_g`. Units: mission time in s, G, G/s, degrees, m/s, m.
- `g_mean`/`g_min`/`g_max`: the time-weighted mean and extremes of live G within the cell.
- `g_sustained`: the time-weighted mean of sustained G. Empty means the cell has no sustained value.
- `high_g`: marks sustained high-G episodes.
- Empty `aoa` means no valid angle of attack. All other columns must be finite numbers, or loading fails.
- The leading `#` lines describe the file.

**`….json`: the mission summary (schema 2).**

- Contents:
  - save time, label (for example `Simulator: Dogfight mix`) and G mode;
  - thresholds and W (`min_excursion_s`);
  - all statistics (peak and sustained);
  - the maximum and minimum average G per window;
  - sample counts and the name of the matching CSV.
- Old (schema 1) files still load:
  - their extremes and curve are the old open-close filtered values. They are shown as sustained values, marked "Filtered … (v1 file)";
  - peaks show "--";
  - the chart legend reads "filtered min-max (v1 file)";
  - window averages are recomputed from the curve.
- **Write order:** the JSON is written last. Both files are written atomically (temporary file + rename), so a crash never leaves a partial record.

`replay_cli <log.csv> --save-report <dir>` writes the mission from a replayed log in the same format to any folder.

## Building

### Windows

Requirements: Visual Studio 2022 or Build Tools 2022 with the "Desktop development with C++" workload. It includes the Windows SDK; CMake and Ninja are bundled. In PowerShell 7:

```powershell
. .\scripts\dev-env.ps1      # import the MSVC x64 environment (cl, Windows SDK, VS's CMake/Ninja)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
cmake --build build --target ac8gmeter_dist   # release files: Full/ModOnly zips and installer in build/dist/
```

**Build outputs:**

| File | Contents |
|---|---|
| `build/AC8GMeter-Installer.exe` | The installer |
| `build/mod_package/AC8GMeter/` | The mod folder, for copying by hand into UE4SS's `Mods` folder |
| `build/dist/` | The three release files |

**Installer command line:**

- Usage: `--install` / `--uninstall [--delete-data]` `[--game-dir <dir>] [--report <file>]`.
- It runs without the UI, for automated tests.
- Exit codes:

| Exit code | Meaning |
|---|---|
| 0 | Success |
| 1 | Failure |
| 2 | Not a game folder, or blocked |
| 3 | The game is running |
| 4 | Bad arguments |

**GitHub Actions** (`.github/workflows/build.yml`):

- Every push is built and tested on Windows. Hosted runners have no GPU, so `overlay_harness` is skipped. Build outputs can be downloaded from the Actions page.
- A tag matching `project(VERSION)`, such as `v0.1.0`, creates a Release with the three files and `SHA256SUMS.txt`. 0.x versions are marked as prereleases.

### macOS (standalone prototype only)

Requirements: Xcode Command Line Tools, and CMake, Ninja and SDL3 from Homebrew.

```bash
brew install cmake ninja sdl3
```

```bash
cmake -S . -B build -G Ninja && cmake --build build
```

Dear ImGui 1.92, nlohmann/json and doctest are downloaded by CMake's FetchContent at configure time.

## How the in-game mod works

The mod has two parts:

- **Lua script** (`mod/lua/Scripts/`). Every game frame, on the game thread, it reads the player's aircraft (`ALivePlayerPlane`) and hands the values to the DLL as a list of numbers.
  - It reads position, velocity, axes, game time and pause state.
  - The protocol is in `mod/flight/lua_frame.h`.
- **Native DLL `ac8gmeter.dll`:**
  - **Reading data:** it reads those numbers through UE4SS's exported Lua wrapper: 6 functions of `RC::LuaMadeSimple::Lua`, see `mod/dll/ue4ss_lua.def`.
  - **Computing G:** it uses the same pipeline as the prototypes.
  - **Drawing:** a DXGI Present hook draws the overlay into the game's DX12 swap chain (SDR, scRGB and HDR10).
  - **Compatibility:** building UE4SS is not needed. The mod relies only on those 6 exports and their behaviour.
    - They were checked against the UE4SS builds the AC8 community uses: 03dbd5c0, e3ba1016 and 6eb3d9bc.
    - Other builds are noted in the log.
    - If an export is missing, the mod does not start.

**The bundled UE4SS:**

- It lives in `third_party/ue4ss/`, with its provenance, SHA-256 hashes (checked at build time), its MIT license and the licenses of the components it contains. These license files are also installed into the mod folder.
- The installer payload is compressed with the Windows Compression API (LZMS) and embedded in the exe.

The mod DLL and the installer link the C++ runtime statically, so they need no Visual C++ Redistributable.

## Standalone prototypes

macOS (SDL3 + Metal):

```bash
./build/apps/macos_proto/ac8_gmeter_proto
```

Windows (Win32 + Direct3D 12, the same ImGui backend as the in-game overlay):

```powershell
.\build\apps\win_proto\ac8_gmeter_proto.exe
```

Both take the same options. On Windows `--size` is in pixels, and the default settings file is `%APPDATA%\AC8GMeter\settings.json`.

Common options:

| Option | Effect |
|---|---|
| `--scenario SustainedTurn` | Simulation scenario: LevelCruise / SustainedTurn / PullUpPushOver / DogfightMix / Manual |
| `--size 1280x720` | Window size |
| `--settings <file.json>` | Settings file. Default `~/Library/Application Support/AC8GMeter/settings.json`. Settings and history in an old `AC8FlightTelemetry` folder are migrated on first start |
| `--missions <dir>` | Mission history folder. Default: `missions/` next to the settings file |
| `--history` | Open the mission history window at start |
| `--menu` / `--edit` | Start with the settings menu open / in edit mode |
| `--end-mission-at <s>` | End the mission at second s and open the debrief |
| `--velocity-noise <m/s>` | Add Gaussian noise to the simulator's velocity (standard deviation per axis), to check the display on dirty data |
| `--screenshot out.png --frames N` | Render N frames, save a screenshot and exit, for automated visual checks |

The debug panel (`Ctrl+F1`) can:

- switch between simulator and replay;
- choose the scenario, change the speed, pause;
- start or end a mission, or quit to the menu;
- inject spikes, stalls and teleports;
- toggle noise, dropped and repeated frames, and whether velocity is provided;
- record CSV;
- compare truth and estimate;
- switch the virtual resolution.

In the Manual scenario you can fly with the keyboard:

| Keys | Action |
|---|---|
| `W/S` or `↑/↓` | Pull / push |
| `A/D` or `←/→` | Roll |
| `Shift` | Throttle up |
| `Z` | Throttle down |

## Tests

```bash
ctest --test-dir build --output-on-failure
```

The tests are:

- doctest unit tests for the core, sources, storage and ui layers;
- end-to-end replay regression checks (`replay_cli --check`) on the 6 flight logs with ground truth in `data/replays/*.csv`;
- on Windows, also:
  - unit tests for the mod;
  - end-to-end tests that drive the real DLL through a fake UE4SS;
  - installer tests, including silent install and uninstall with the real exe;
  - overlay tests that need a GPU (`overlay_harness`), covering swap-chain formats and HDR.

To replay one log and print its statistics:

```bash
./build/apps/replay_cli/replay_cli data/replays/dogfight_mix.csv
```

## Project layout

```
src/core            pure C++20, no dependencies: math, filters, kinematics, metrics, mission stats, TelemetryPipeline
src/sources         point-mass flight model (with ground truth), simulated source, CSV replay and recording
src/config          settings model and JSON persistence (lenient load, range clamping, atomic write, debounced save)
src/storage         mission history: missions saved as JSON + CSV, list, load, delete, retention limit (worker thread)
src/ui              Dear ImGui only: HUD overlay, settings menu, debrief and G chart, history window, layout
mod/lua             UE4SS Lua scripts: per-frame sampling, state probe, debug data, config.lua
mod/dll             ac8gmeter.dll: UE4SS Lua exports, offline check, entry points
mod/flight          Lua frame protocol, flight sampler (mission detection)
mod/overlay         DXGI Present hook and DX12 overlay (SDR/scRGB/HDR10)
mod/runtime         mod runtime: pipeline, settings and mission saving, recording, debug recording
mod/support         log
mod/package         README and third-party notices for the mod folder
installer           one-file installer (install / update / uninstall)
third_party/ue4ss   bundled UE4SS (provenance, hashes, licenses)
apps/proto_common   shared by both prototype hosts: simulated game view, debug panel, command line
apps/macos_proto    SDL3 + Metal host
apps/win_proto      Win32 + Direct3D 12 host
apps/overlay_harness  overlay test program (stands in for the game's swap chain)
apps/replay_cli     headless replay, statistics, regression checks, log generation
apps/latency_bench  G display latency/error benchmark across sample rates and noise (adaptive vs fixed filtering)
cmake               dependencies, UE4SS hash check, release packaging (ac8gmeter_dist)
tests               doctest tests
data/replays        reference flight logs
assets/fonts        B612 Mono (SIL OFL 1.1)
```

Layer dependencies:

- `core ← sources`
- `core ← config ← ui`
- `core ← storage ← ui`

The in-game mod and both prototypes share core, config, storage and ui.

## Extension points

- **New data source:** implement `ft::ITelemetrySource` (`src/core/source.h`).
- **Subscribe to the metrics stream:** implement `ft::IMetricsConsumer` (`src/core/pipeline.h`). Future physiological models such as blackout and redout will plug in here.
- **New overlay layer:** implement `ft::IOverlayLayer` (`src/ui/overlay_layer.h`) and register it with `TelemetryUi::addLayer()`.

## Acknowledgements

- [RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) (bundled, MIT)
- [Dear ImGui](https://github.com/ocornut/imgui)
- [SDL](https://github.com/libsdl-org/SDL)
- [nlohmann/json](https://github.com/nlohmann/json)
- [doctest](https://github.com/doctest/doctest)
- [B612](https://github.com/polarsys/b612) font

Reference projects: [AC8-Mouse-Aim](https://github.com/FletcherMiya/AC8-Mouse-Aim), AC8 Tweaks, AC8 Per-View FOV, AC7 Cockpit.
