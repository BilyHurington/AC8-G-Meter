# AC8 G-Meter

**简体中文** | [English](README.en.md)

[![build](https://github.com/BilyHurington/AC8-G-Meter/actions/workflows/build.yml/badge.svg)](https://github.com/BilyHurington/AC8-G-Meter/actions/workflows/build.yml)

《皇牌空战8：Wings of Theve》（ACE COMBAT 8）的 G 值显示 Mod（AC8 过载表）。在游戏画面上以 HUD 风格的透明文字实时显示 G 值、峰值 G、G Onset、迎角等飞行数据，任务结束后弹出带整场 G 曲线的飞行总结。不修改原版 HUD。

- **准确的实时 G：** 由飞机的位置、速度和姿态估计法向过载（Nz），滤波随输入的采样率和噪声自适应，干净的 60 Hz 数据下显示延迟仅约 25 ms。
- **高 G 不截断：** 没有数值上限，只剔除与运动不一致的数据毛刺，真实的高 G 读数总会显示出来。
- **可配置的叠加层：** 拖动定位、锁定，调整字号、透明度、颜色，自选显示的指标。设置自动保存。
- **任务总结与历史：** 可缩放、平移的整场 G 曲线，峰值 G 与持续 G，各时间窗口的平均 G；每场任务保存为 JSON + CSV，可随时重新打开。

> **仅限离线单人游戏。** Mod 只在 Easy Anti-Cheat 未运行时启动。切勿在联机模式下使用 Mod。

**当前阶段：** 游戏内 UE4SS Mod 首个测试版（0.x，预发布）。另有 macOS 与 Windows 独立原型，使用模拟飞行数据和可回放的 CSV 日志，不依赖游戏本体。

## 下载与安装

在 [Releases](https://github.com/BilyHurington/AC8-G-Meter/releases) 页面下载，三选一：

| 文件 | 用法 |
|---|---|
| `AC8GMeter-<版本>-Installer.exe` | **推荐。** 放到游戏根目录双击运行（见下文） |
| `AC8GMeter-<版本>-Full.zip` | UE4SS + Mod。解压到游戏根目录，与其中的 `Game` 文件夹合并。仅适用于尚未安装 UE4SS 的游戏（否则会覆盖已有的 UE4SS 及其设置） |
| `AC8GMeter-<版本>-ModOnly.zip` | 仅 Mod，用于已安装 UE4SS 的游戏：把 `AC8GMeter` 文件夹复制到 `Game\Binaries\Win64\<UE4SS 文件夹>\Mods\`。UE4SS 需要在 `UE4SS-settings.ini` 中设置 `HookEngineTick = 1` |

`SHA256SUMS.txt` 是以上文件的校验值。

**安装程序：** 游戏根目录指 Steam 的 `ACE COMBAT 8` 文件夹，里面有 `Game` 和 `EasyAntiCheat`。安装程序会：

- 游戏里还没有 UE4SS 时，安装随附的 **UE4SS v3.0.1 (e3ba1016)**。这是 AC8 Tweaks、Per-View FOV、Mouse-Aim 等 Mod 使用的版本，目录结构与 Mouse-Aim 相同。已有 UE4SS 时直接使用、不做改动，避免多个 Mod 互相冲突。
- 把 Mod 装到 `Game\Binaries\Win64\<UE4SS 文件夹>\Mods\AC8GMeter`。Mod 文件夹里的 `enabled.txt` 让 UE4SS 自动加载它，不会改动各 Mod 共用的 `mods.txt`。现有 UE4SS 处于禁用状态，或 `HookEngineTick = 0` 时会给出提示；设置了自定义 Mods 文件夹时拒绝安装。
- 没有 `EasyAntiCheat\AC8_Offline.json` 时生成它（游戏的 `Settings.json` 加上 `"allow_null_client": "true"`），并把 Steam 启动选项复制到剪贴板。

再次运行可以选择：

- **更新/修复**：保留设置、任务记录和改过的 `config.lua`。
- **卸载**：可选同时删除任务记录。

安装内容记录在 Mod 文件夹的 `install.json`，卸载只删除这些文件。UE4SS 只有在是本程序装的、且没有其他 Mod 在用时才会删除。界面底部的“第三方软件许可”链接会打开全部许可文本。

**压缩包的卸载：**

- Full：删除 `Game\Binaries\Win64\` 下的 `dwmapi.dll`、`override.txt` 和 `UE4SS` 文件夹。
- ModOnly：删除 `AC8GMeter` 文件夹。

压缩包不会生成 `AC8_Offline.json`。需要时可以手动复制一份 `EasyAntiCheat\Settings.json`，加上 `"allow_null_client": "true"`；也可以改用下面的 b、c 两种启动方式。

## 离线启动

Mod 只在 Easy Anti-Cheat 未激活时启动。以下任一方式即可：

1. **Steam 启动选项**：在 Steam 中打开 ACE COMBAT 8 的属性，填入“启动选项”（安装程序已复制到剪贴板）：
   ```
   cmd /d /c "set EOS_USE_ANTICHEATCLIENTNULL=1&& %command% -anticheat_settings=AC8_Offline.json"
   ```
2. **直接运行游戏程序**：运行 `Game\Binaries\Win64\AceCombat8.exe`，旁边放一个内容为 `2288340` 的 `steam_appid.txt`。这样 EAC 服务不会启动。
3. **AC8 Tweaks 的离线启动器。**

恢复联机：从 Steam 正常启动（去掉启动选项），并卸载 Mod 或禁用 UE4SS。

## 使用

| 按键 | 作用 |
|---|---|
| `Ctrl+F8` | 打开/关闭设置菜单 |
| `Ctrl+F9` | 进入/退出叠加层编辑（拖动）模式 |
| `Ctrl+F7` | 只读状态探测（诊断用，写入 `probe-*.txt`） |
| `Ctrl+F1` | 调试面板（仅独立原型） |

在设置菜单里可以修改快捷键。macOS 原型上用 `Cmd` 代替 `Ctrl` 也可以触发。建议使用无边框或窗口化显示模式。

**叠加层显示：**

- 默认只在任务中显示：菜单、结算画面隐藏，编辑位置时总是显示。可在设置中关闭 “Only during missions”。
- `Scripts\config.lua` 中的 `Overlay = false` 可以完全关闭叠加层：不 Hook 游戏画面，G 照常计算，任务照常保存和录制。可用来排查画面或稳定性问题是否与叠加层有关。

**Mod 文件夹中的文件：**

| 文件 | 内容 |
|---|---|
| `settings.json` | 设置 |
| `missions\` | 任务历史 |
| `recordings\` | 每场任务的飞行数据 CSV，可用 `replay_cli` 回放；在 `Scripts\config.lua` 中开关 |
| `ac8gmeter.log` | 日志 |

**调试记录**（`Ctrl+F8` 设置菜单 > Diagnostics）：用于核对 G 的计算。

- 每一帧写入 `debug\ac8_debug_*.csv`，内容包括：
  - 游戏给出的全部原始数据：位置、速度、三轴朝向、旋转角、世界时间与帧间隔、真实时间、全局与飞机自身的时间缩放、运动组件速度、游戏自身的速度/高度读数（若有）、过场镜头与输入屏蔽状态；
  - 采样器推入管线的样本，以及管线的计算结果；
  - 不经任何滤波的逐帧差分 G（`fd_*` 列）。
- 60 fps 下约每小时 130 MB。
- 菜单里的开关在下次启动游戏时自动关闭。若要从游戏启动起就记录，在 `Scripts\config.lua` 中设置 `DebugRecording = true`。

`Ctrl+F7` 开始/停止只读的状态探测，把飞机、控制器、HUD、游戏模式的相关字段变化写入 `probe-*.txt`，用于寻找任务流程信号。

## 已知问题

- **任务判定（第一版）：**
  - 玩家驾驶飞机即视为任务进行中；游戏暂停时统计暂停；
  - 飞机消失超过 3 秒（结算、菜单、读盘）即视为任务结束，弹出任务总结；
  - 中途重生不会结束任务；
  - 任务结束后的结算动画和回放期间仍在计时，因此总结会晚一些弹出；
  - 中途退出任务也按“完成”记录；
  - 从暂停菜单重开任务时，若飞机在 3 秒（不含暂停时间）内重新出现，两次尝试会合并为一场；
  - 超过 3 秒不操控飞机、且未暂停的过场，会把一场任务拆成两场。
- **游戏机动的 G 值本身可以很高：** 测试中出现的 100 G 以上读数，与游戏自身的速度数据一致，是游戏的实际运动学结果，不是计算错误。碰撞造成的冲击会计入峰值 G，但不计入持续 G。
- **DLSS 帧生成：** 测试中出现过一次开启 DLSS 帧生成时的游戏崩溃，原因尚未确定。如遇崩溃，请尝试关闭帧生成，或设置 `Overlay = false`。

## 功能详解

- **实时指标：**
  - 默认显示：当前 G、MAX G、MIN G。MAX/MIN 为峰值，与实时 G 同步更新。
  - 可选显示：
    - 持续 G 最大值（SUS G）；
    - G Onset（G/s）；
    - AoA；
    - 当前高 G 时间（HI-G）：按实时 G 计时的当前高 G 段时长；
    - 侧滑角；
    - 速度。
- **数据处理：**
  - 用最小二乘法从带时间戳的速度或位置估计加速度，能适应帧间隔不均匀的情况；
  - 中值滤波加 EMA 平滑；
  - 识别并处理异常值：尖峰、卡顿、瞬移、重复帧、NaN。
  - **尖峰门（spike gate）对实时 G 的取舍：** 与近期运动不一致的速度样本先被“暂扣”，由后续样本判定真伪，因此：
    - 真实的单帧 G 冲击会完整显示，但晚约 3 帧（60 Hz 下约 67 ms）；
    - 1–3 个样本的数据毛刺会被整体剔除，不会出现在显示上；
    - 代价是：真实的 2 帧碰撞双峰（例如 +13 G 紧接 −11 G）与毛刺无法区分，同样会从实时 G 中剔除；
    - 持续 4 个样本及以上的突变按真实运动处理并显示。
- **叠加层：**
  - 透明背景，浅绿色 `#93F6A8` 文字，带轻微阴影；
  - 默认位置为屏幕的 (4.5%, 16%)，1080p 下约 (86, 173)；
  - 1080p 下字号 20 px，随分辨率缩放；
  - 正常模式下不拦截鼠标；
  - 编辑模式下可以拖动，支持位置锁定、字号、行距、透明度、颜色、背景设置，以及选择显示哪些指标。
- **设置菜单：** 按快捷键打开。设置自动保存为 JSON，支持恢复默认值。
- **任务总结：** 任务结束后自动弹出。内容包括：
  - 整场任务的 G 曲线；
  - 峰值 G 与持续 G（最大/最小及发生时刻）；
  - Onset 极值、最大 AoA、持续高 G 时间；
  - 1/5/10/30/60 s 平均 G 的最大/最小值；
  - 数据质量统计。
- **峰值与持续值（双值）：**
  - **峰值 G**（Peak G，即叠加层上的 MAX/MIN G）：实时 G 显示过的每一个读数，包括真实的高 G 尖峰，不设上限、不延迟。
  - **持续 G**（Sustained G）：保持至少约 W 秒的最高/最低水平。
    - 短于 W 的突跳/突降（碰撞、物理抖动）会被去除，因此即使是真实机动，持续值也可能低于峰值。例如只触及 8.9 G 的三角形拉杆，持续值为 8.2 G。
    - 高 G 段（次数、最长、累计）按持续 G 统计，短促尖峰不会产生高 G 段。
  - **W 的设置：** 设置菜单中的 “Sustained G: ignore bumps shorter than”，默认 0.2 s；设为 0 时持续值等于峰值。持续值比实时 G 最多晚约 2W 得出。
  - **噪声较大时：** 预平滑会按实测噪声自动加宽（W/4 至 W/2）。
    - 此时只保证去除短于约 W/2 的突跳，更长的部分保留。
    - 持续极值可能比真实保持水平偏外约 0.1 G；在 60 fps、速度噪声 1 m/s 这类极噪数据下约 0.35 G。
- **G 曲线：** 以 20 Hz 记录。
  - **操作：** 滚轮以光标为中心缩放，拖动平移，双击复位。
  - **悬停：** 显示该时刻的 G（均值/最小/最大、持续值）、Onset、AoA、速度和高度。
  - **图形元素：**
    - 浅色带：实时 G 的最小–最大范围，单帧峰值和短促尖峰都保留；
    - 主曲线：持续 G；
    - 橙色圆点：峰值最大/最小；
    - 白色菱形：持续最大/最小；
    - 橙色区域：持续高 G 段；
    - 可叠加显示 Onset 或 AoA（右侧坐标轴）。
  - **标签合并：** 峰值与持续值相差 ≤ 0.05 G 且位置相近时，合并为一个标签，如 “MAX +8.2 G (sus +8.2)”。标签被挤开时用细线连回标记点。
- **窗口平均 G：** “Average G over window”表列出各窗口长度下实时 G 平均值的最大/最小值及起始时刻。
  - 悬停某一行，会在同一帧于 G 曲线上高亮对应时间段：最大值为橙色，最小值为蓝色。
  - 若该时间段不在当前缩放范围内，曲线会在开始悬停时自动平移（或缩小）以显示它。
  - 最后一个记录格通常不满 1/20 s，按实际时长计权。
  - 窗口长于实际飞行时间时不给出该值。例如 29.987 s 的任务没有 30 s 值。
- **任务历史：**
  - **自动保存：** 每场有 G 数据的任务结束后自动保存。
    - 可在设置菜单关闭；默认保留最近 50 场，0 = 不限数量。
    - 保存由计算管线在任务结束时直接触发，不依赖叠加层是否绘制了那一帧；连续结束的多场任务都会保存。
  - **调整保留数量：**
    - 把 “Keep missions” 调低到少于已保存的场数时，会先弹窗确认（“下次保存时将删除 N 场较早的任务”）。
    - 键入的数字在按回车、Tab 或离开输入框时才生效，不会逐位生效；−/+ 按钮立即生效。
  - **历史窗口：** 从设置菜单或任务总结中的 “Mission history...” 打开。
    - 点击任意一行即可重新打开该任务的总结和 G 曲线；
    - 可以删除单条记录（需确认）。
  - 保存、读取和删除都在后台线程完成，不会卡住画面。

## 任务历史文件

- **保存位置：**
  - 游戏内 Mod：Mod 文件夹的 `missions\`；
  - 原型：`~/Library/Application Support/AC8GMeter/missions/`（Windows：`%APPDATA%\AC8GMeter\missions\`）。
- **文件命名：** 每场任务两个文件，文件名以保存时刻（UTC）开头，例如 `20261008-140312_m3`。
  - 同一秒内重名时追加 `-2`、`-3`…
  - 超过保留数量时删除最旧的；新旧按保存时刻、任务编号、追加序号排序。

**`….trace.csv`：G 曲线，可直接用表格软件打开。**

- 每 1/20 s 一行，列为 `t,g_mean,g_min,g_max,g_sustained,onset,aoa,speed,alt,high_g`。单位依次为：任务时间 s、G、G/s、度、m/s、m。
- `g_mean`/`g_min`/`g_max`：实时 G 在该格内的时间加权均值和极值。
- `g_sustained`：持续 G 的时间加权均值；为空表示该格没有持续值。
- `high_g`：持续高 G 段标记。
- `aoa` 为空表示无有效迎角；其余列必须是有限数值，否则读取失败。
- 开头的 `#` 行是说明。

**`….json`：任务摘要（格式版本 schema 2）。**

- 内容包括：
  - 保存时间、标签（如 `Simulator: Dogfight mix`）、G 模式；
  - 阈值和 W（`min_excursion_s`）；
  - 全部统计值（峰值与持续值）；
  - 各窗口平均 G 的最大/最小值；
  - 样本计数，以及对应 CSV 的文件名。
- 旧版（schema 1）文件仍可读取：
  - 其极值和曲线是旧版的开闭滤波值，读取后作为持续值显示，标注 “Filtered … (v1 file)”；
  - 峰值显示 “--”；
  - 曲线图例标为 “filtered min-max (v1 file)”；
  - 窗口平均值由曲线重新计算。
- 写入顺序：JSON 最后写入。两个文件都通过“临时文件 + 重命名”原子写入，中途崩溃不会留下残缺记录。

`replay_cli <log.csv> --save-report <dir>` 可以把回放日志得到的任务以同样格式写入任意目录。

## 构建

### Windows

依赖：Visual Studio 2022 或 Build Tools 2022，选择“使用 C++ 的桌面开发”工作负载（含 Windows SDK，CMake 和 Ninja 随附）。在 PowerShell 7 中运行：

```powershell
. .\scripts\dev-env.ps1      # 导入 MSVC x64 环境（cl、Windows SDK、VS 自带的 CMake/Ninja）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
cmake --build build --target ac8gmeter_dist   # 发布文件：build/dist/ 下的 Full/ModOnly 压缩包和安装程序
```

**构建产物：**

| 文件 | 内容 |
|---|---|
| `build/AC8GMeter-Installer.exe` | 安装程序 |
| `build/mod_package/AC8GMeter/` | Mod 文件夹，可手动复制到 UE4SS 的 `Mods` 文件夹 |
| `build/dist/` | 三个发布文件 |

**安装程序命令行：**

- 用法：`--install` / `--uninstall [--delete-data]` `[--game-dir <dir>] [--report <file>]`。
- 不显示界面，用于自动化测试。
- 退出码：

| 退出码 | 含义 |
|---|---|
| 0 | 成功 |
| 1 | 失败 |
| 2 | 不是游戏目录，或被阻止 |
| 3 | 游戏正在运行 |
| 4 | 参数错误 |

**GitHub Actions**（`.github/workflows/build.yml`）：

- 每次推送都会在 Windows 上构建并运行测试。构建机没有 GPU，所以跳过 `overlay_harness`；构建产物可在 Actions 页面下载。
- 推送与 `project(VERSION)` 一致的标签（如 `v0.1.0`）会自动创建 Release，并上传三个发布文件和 `SHA256SUMS.txt`。0.x 版本标记为预发布。

### macOS（仅独立原型）

依赖：Xcode Command Line Tools，以及通过 Homebrew 安装的 CMake、Ninja 和 SDL3。

```bash
brew install cmake ninja sdl3
```

```bash
cmake -S . -B build -G Ninja && cmake --build build
```

Dear ImGui 1.92、nlohmann/json 和 doctest 会在配置时由 CMake 的 FetchContent 自动下载。

## 游戏内 Mod 的实现

Mod 由两部分组成：

- **Lua 脚本**（`mod/lua/Scripts/`）：每个游戏帧在游戏线程读取玩家飞机（`ALivePlayerPlane`）的数据，打包成一组数值交给 DLL。
  - 读取内容：位置、速度、三轴朝向、游戏时间和暂停状态。
  - 数据协议见 `mod/flight/lua_frame.h`。
- **原生 DLL `ac8gmeter.dll`**：
  - **读取数据：** 通过 UE4SS 导出的 Lua 封装读取 Lua 脚本给出的数值。用到的是 `RC::LuaMadeSimple::Lua` 的 6 个函数，见 `mod/dll/ue4ss_lua.def`。
  - **计算 G：** 用与原型相同的管线。
  - **绘制：** 通过 DXGI Present Hook 把叠加层画进游戏的 DX12 交换链，支持 SDR、scRGB 和 HDR10。
  - **兼容性：** 不需要编译 UE4SS，只依赖上述 6 个导出函数及其行为。
    - 已核对 AC8 社区使用的 UE4SS 版本：03dbd5c0、e3ba1016、6eb3d9bc；
    - 其他版本会在日志中提示；
    - 缺少导出函数时 Mod 不启动。

**随附的 UE4SS：**

- 放在 `third_party/ue4ss/`，附有来源说明、SHA-256（构建时校验）、MIT 许可证，以及其内含第三方组件的许可清单。这些许可文件也会装进 Mod 文件夹。
- 安装包内容用 Windows 自带的压缩 API（LZMS）压缩后，嵌入安装程序 exe。

Mod DLL 和安装程序都静态链接 C++ 运行库，不依赖 VC++ 运行库。

## 独立原型

macOS（SDL3 + Metal）：

```bash
./build/apps/macos_proto/ac8_gmeter_proto
```

Windows（Win32 + Direct3D 12，与游戏内叠加层使用相同的 ImGui 后端）：

```powershell
.\build\apps\win_proto\ac8_gmeter_proto.exe
```

两个平台的参数相同。Windows 上 `--size` 以像素为单位，默认设置文件为 `%APPDATA%\AC8GMeter\settings.json`。

常用参数：

| 参数 | 作用 |
|---|---|
| `--scenario SustainedTurn` | 选择模拟场景。可选值：LevelCruise / SustainedTurn / PullUpPushOver / DogfightMix / Manual |
| `--size 1280x720` | 设置窗口尺寸 |
| `--settings <file.json>` | 使用指定的设置文件。默认为 `~/Library/Application Support/AC8GMeter/settings.json`。旧版本 `AC8FlightTelemetry` 目录中的设置和任务历史会在首次启动时自动迁移 |
| `--missions <dir>` | 任务历史目录。默认为设置文件旁的 `missions/` |
| `--history` | 启动时打开任务历史窗口 |
| `--menu` / `--edit` | 启动时直接打开设置菜单 / 进入编辑模式 |
| `--end-mission-at <s>` | 在第 s 秒自动结束任务，并弹出任务总结 |
| `--velocity-noise <m/s>` | 模拟器速度数据加入高斯噪声（每轴标准差），用于检查脏数据下的显示 |
| `--screenshot out.png --frames N` | 渲染 N 帧后截图并退出，用于自动化的视觉检查 |

在调试面板（`Ctrl+F1`）里可以：

- 切换模拟器或回放；
- 选择场景，调整倍速，暂停；
- 开始、结束任务或退出到菜单；
- 注入尖峰、卡顿、瞬移；
- 开关噪声、丢帧、重复帧，切换是否提供速度；
- 录制 CSV；
- 对比真值和估计值；
- 切换虚拟分辨率。

在 Manual 场景中可以用键盘驾驶：

| 按键 | 作用 |
|---|---|
| `W/S` 或 `↑/↓` | 拉杆/推杆 |
| `A/D` 或 `←/→` | 滚转 |
| `Shift` | 加油门 |
| `Z` | 收油门 |

## 测试

```bash
ctest --test-dir build --output-on-failure
```

测试包括：

- core、sources、storage、ui 各层的 doctest 单元测试；
- 对 `data/replays/*.csv` 中 6 份带真值的飞行日志做端到端回放回归检查（`replay_cli --check`）；
- Windows 上另有以下测试：
  - Mod 的单元测试；
  - 用模拟 UE4SS 驱动真实 DLL 的端到端测试；
  - 安装程序测试，包括实际运行 exe 的静默安装和卸载；
  - 需要 GPU 的叠加层测试（`overlay_harness`），覆盖各种交换链格式与 HDR。

单独回放一份日志并查看统计：

```bash
./build/apps/replay_cli/replay_cli data/replays/dogfight_mix.csv
```

## 项目结构

```
src/core            纯 C++20、零依赖：数学、滤波、运动学、指标、任务统计、TelemetryPipeline
src/sources         质点飞行模型（带真值）、模拟数据源、CSV 回放与录制
src/config          设置模型与 JSON 持久化（宽松读取、范围钳制、原子写入、防抖保存）
src/storage         任务历史：任务保存为 JSON + CSV、列表、读取、删除、保留数量上限（后台工作线程）
src/ui              仅依赖 Dear ImGui：HUD 叠加层、设置菜单、任务总结与 G 曲线、任务历史窗口、布局计算
mod/lua             UE4SS Lua 脚本：每帧采样、状态探测、调试数据、config.lua
mod/dll             ac8gmeter.dll：UE4SS Lua 导出、离线检查、入口
mod/flight          Lua 帧协议、飞行采样器（任务判定）
mod/overlay         DXGI Present Hook 与 DX12 叠加层（SDR/scRGB/HDR10）
mod/runtime         Mod 运行时：管线、设置与任务保存、录制、调试记录
mod/support         日志
mod/package         Mod 文件夹中的 README 与第三方许可说明
installer           单文件安装程序（安装 / 更新 / 卸载）
third_party/ue4ss   随附的 UE4SS（含来源、校验值与许可）
apps/proto_common   两个原型宿主共用：模拟的游戏画面、调试面板、命令行
apps/macos_proto    SDL3 + Metal 宿主
apps/win_proto      Win32 + Direct3D 12 宿主
apps/overlay_harness  叠加层测试程序（模拟游戏的交换链）
apps/replay_cli     无界面回放、统计、回归检查、日志生成
apps/latency_bench  不同采样率与噪声下 G 显示延迟/误差基准（自适应 vs 固定滤波）
cmake               依赖、UE4SS 校验、发布打包（ac8gmeter_dist）
tests               doctest 测试
data/replays        参考飞行日志
assets/fonts        B612 Mono（SIL OFL 1.1）
```

各层依赖关系：

- `core ← sources`
- `core ← config ← ui`
- `core ← storage ← ui`

游戏内 Mod 与两个原型复用 core、config、storage 和 ui 这四层。

## 扩展点

- **新数据源：** 实现 `ft::ITelemetrySource`（`src/core/source.h`）。
- **订阅指标流：** 实现 `ft::IMetricsConsumer`（`src/core/pipeline.h`）。后续的黑视、红视等生理模型会通过它接入。
- **新的叠加绘制层：** 实现 `ft::IOverlayLayer`（`src/ui/overlay_layer.h`），并调用 `TelemetryUi::addLayer()` 注册。

## 致谢

- [RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS)（随附，MIT）
- [Dear ImGui](https://github.com/ocornut/imgui)
- [SDL](https://github.com/libsdl-org/SDL)
- [nlohmann/json](https://github.com/nlohmann/json)
- [doctest](https://github.com/doctest/doctest)
- [B612](https://github.com/polarsys/b612) 字体

参考项目：[AC8-Mouse-Aim](https://github.com/FletcherMiya/AC8-Mouse-Aim)、AC8 Tweaks、AC8 Per-View FOV、AC7 Cockpit。
