# AC8 G-Meter

A G-meter for **Ace Combat 8: Wings of Theve**. It draws a clean, HUD-styled text overlay with your live G load, peak G and other flight data. It never touches the game's own HUD. After each sortie it opens a full debrief with the mission's G curve.

- **Accurate live G.** Load factor (Nz) is estimated from the aircraft's position, velocity and attitude. Filtering adapts to the input's sample rate and noise, so on clean 60 Hz data the display lags only about 25 ms.
- **High G is never clipped.** There is no magnitude cap. Glitches are rejected only when they are inconsistent with the motion, so real high-G readings always come through.
- **Configurable overlay.** Drag it into place, lock it, and set font size, opacity and colour. Choose which metrics to show: G, max/min G, onset rate (G/s), AoA, high-G time, sideslip and speed. Settings save automatically.
- **Mission debrief.** Includes an interactive full-mission G chart (zoom, pan, hover). It shows peak G and sustained G (spikes shorter than 0.2 s removed), plus the best and worst 1/5/10/30/60 s average G and high-G episodes.
- **Mission history.** Every sortie is saved as JSON plus a CSV trace that opens in Excel or Python. Past debriefs can be reopened from the history window.

Written in C++20 with Dear ImGui, as layered libraries (core / config / ui / storage) designed to drop into a UE4SS mod. Current stage: a standalone macOS prototype (SDL3 + Metal) driven by a physics-based flight simulator and replayable flight logs. The Windows / UE4SS integration comes next.

---

**AC8 G-Meter（AC8 过载表）**：《皇牌空战8：Wings of Theve》的 G 值显示叠加层 Mod。在游戏画面上以透明文字的形式实时显示 G 值、G Onset、迎角等飞行数据，并在任务结束后给出飞行统计。不修改原版 HUD。

**当前阶段：** macOS 独立原型。使用模拟飞行数据和可回放的 CSV 日志，不依赖游戏本体。之后会移植到 Windows，通过 UE4SS 接入 AC8。

## 功能

- **实时指标：** 当前 G、MAX G、MIN G（默认显示，MAX/MIN 为峰值，与实时 G 同步更新）。可选显示持续 G 最大值（SUS G）、G Onset（G/s）、AoA、当前高 G 时间（HI-G，按实时 G 计时的当前高 G 段时长）、侧滑角、速度。
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
  - 1080p 下字号 16 px，随分辨率缩放；
  - 正常模式下不拦截鼠标；
  - 编辑模式下可以拖动，支持位置锁定、字号、行距、透明度、颜色、背景设置，以及选择显示哪些指标。
- **设置菜单：** 按快捷键打开。设置自动保存为 JSON，支持恢复默认值。
- **任务总结：** 任务结束后自动弹出，内容包括整场任务的 G 曲线、峰值 G 与持续 G（最大/最小及发生时刻）、Onset 极值、最大 AoA、持续高 G 时间、1/5/10/30/60 s 平均 G 的最大/最小值和数据质量统计。
  - **峰值与持续值（双值）：** 峰值 G（Peak G，叠加层上的 MAX/MIN G）是实时 G 显示过的每一个读数，包括真实的高 G 尖峰，不设上限、不延迟。持续 G（Sustained G）是“保持至少约 W 秒的最高/最低水平”：短于 W 的突跳/突降（碰撞、物理抖动）被去除，因此即使是真实机动，持续值也可能低于峰值（例如只触及 8.9 G 的三角形拉杆，持续值为 8.2 G）。高 G 段（次数、最长、累计）按持续 G 统计，短促尖峰不会产生高 G 段。W 由设置菜单中的“Sustained G: ignore bumps shorter than”设定（默认 0.2 s，0 = 持续值等于峰值）；持续值比实时 G 最多晚约 2W 得出。数据噪声较大时，预平滑会按实测噪声自动加宽（W/4 至 W/2）：此时只保证去除短于约 W/2 的突跳，更长的部分保留；持续极值仍可能比真实保持水平偏外约 0.1 G（60 fps、速度噪声 1 m/s 这类极噪数据下约 0.35 G）。
  - G 曲线可交互：滚轮以光标为中心缩放，拖动平移，双击复位；悬停显示该时刻的 G（均值/最小/最大、持续值）、Onset、AoA、速度和高度。曲线以 20 Hz 记录。浅色带为实时 G 的最小–最大范围（单帧峰值和短促尖峰都保留），主曲线为持续 G；橙色圆点标出峰值最大/最小，白色菱形标出持续最大/最小（两者数值相差 ≤ 0.05 G 且位置相近时合并为一个标签，如 “MAX +8.2 G (sus +8.2)”；标签被挤开时用细线连回标记点）。橙色区域为持续高 G 段，可叠加显示 Onset 或 AoA（右侧坐标轴）。
  - “Average G over window”表列出各窗口长度下实时 G 平均值的最大/最小值及起始时刻；悬停某一行会在同一帧于 G 曲线上高亮对应时间段（最大值为橙色，最小值为蓝色）；若该时间段不在当前缩放范围内，曲线会在开始悬停时自动平移（或缩小）以显示它。最后一个记录格通常不满 1/20 s，按实际时长计权；窗口长于实际飞行时间则不给出（例如 29.987 s 的任务没有 30 s 值）。
- **任务历史：** 每场有 G 数据的任务结束后自动保存（可在设置菜单关闭，默认保留最近 50 场，0 = 不限数量）。保存由计算管线在任务结束时直接触发，不依赖叠加层是否绘制了那一帧，连续结束的多场任务都会保存。把“Keep missions”调低到少于已保存的场数时，会先弹窗确认（“下次保存时将删除 N 场较早的任务”）；键入的数字在按回车、Tab 或离开输入框时才生效（不会逐位生效），−/+ 按钮立即生效。从设置菜单或任务总结中的“Mission history...”打开历史窗口，点击任意一行即可重新打开该任务的总结和 G 曲线，也可以删除单条记录（需确认）。保存、读取和删除都在后台线程完成，不会卡住画面。

## 快捷键

| 按键 | 作用 |
|---|---|
| `Ctrl+F8` | 打开/关闭设置菜单 |
| `Ctrl+F9` | 进入/退出叠加层编辑（拖动）模式 |
| `Ctrl+F1` | 原型调试面板（仅 macOS 原型） |

在 macOS 上，按 `Cmd` 代替 `Ctrl` 也可以触发。设置菜单里可以修改快捷键。

在原型的 Manual 场景中可以用键盘驾驶：`W/S` 或 `↑/↓` 拉杆/推杆，`A/D` 或 `←/→` 滚转，`Shift` 加油门，`Z` 收油门。

## 构建（macOS）

依赖：Xcode Command Line Tools，以及通过 Homebrew 安装的 CMake、Ninja 和 SDL3。

```bash
brew install cmake ninja sdl3
```

```bash
cmake -S . -B build -G Ninja && cmake --build build
```

Dear ImGui 1.92、nlohmann/json 和 doctest 会在配置时由 CMake 的 FetchContent 自动下载。

## 运行

```bash
./build/apps/macos_proto/ac8_gmeter_proto
```

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

## 任务历史文件

默认保存在 `~/Library/Application Support/AC8GMeter/missions/`（Windows：`%APPDATA%\AC8GMeter\missions\`），每场任务两个文件，文件名以保存时刻（UTC）开头，例如 `20261008-140312_m3`（同一秒内重名时追加 `-2`、`-3`…；保留数量按保存时刻、任务编号、追加序号排序，删除最旧的）：

- `….trace.csv`：G 曲线，每 1/20 s 一行，列为 `t,g_mean,g_min,g_max,g_sustained,onset,aoa,speed,alt,high_g`（任务时间 s、G、G/s、度、m/s、m）。`g_mean`/`g_min`/`g_max` 为实时 G 在该格内的时间加权均值和极值，`g_sustained` 为持续 G 的时间加权均值（为空表示该格没有持续值），`high_g` 为持续高 G 段标记；`aoa` 为空表示无有效迎角，其余列必须是有限数值，否则读取失败。开头的 `#` 行是说明。可直接用表格软件打开。
- `….json`：任务摘要（格式版本 schema 2），包括保存时间、标签（如 `Simulator: Dogfight mix`）、G 模式、阈值和 W（`min_excursion_s`）、全部统计值（峰值与持续值）、各窗口平均 G 的最大/最小值、样本计数以及对应 CSV 的文件名。旧版（schema 1）文件仍可读取：其极值和曲线是旧版的开闭滤波值，读取后作为持续值显示（标注 “Filtered … (v1 file)”），峰值显示“--”，曲线图例标为 “filtered min-max (v1 file)”；窗口平均值由曲线重新计算。JSON 最后写入，两个文件都通过临时文件 + 重命名原子写入，中途崩溃不会留下残缺记录。

`replay_cli <log.csv> --save-report <dir>` 可以把回放日志得到的任务以同样格式写入任意目录。

## 测试

```bash
ctest --test-dir build --output-on-failure
```

测试包括：

- core、sources、storage、ui 各层的 doctest 单元测试；
- 对 `data/replays/*.csv` 中 6 份带真值的飞行日志做端到端回放回归检查（`replay_cli --check`）。

单独回放一份日志并查看统计：

```bash
./build/apps/replay_cli/replay_cli data/replays/dogfight_mix.csv
```

## 项目结构

```
src/core      纯 C++20、零依赖：数学、滤波、运动学、指标、任务统计、TelemetryPipeline
src/sources   质点飞行模型（带真值）、模拟数据源、CSV 回放与录制
src/config    设置模型与 JSON 持久化（宽松读取、范围钳制、原子写入、防抖保存）
src/storage   任务历史：任务保存为 JSON + CSV、列表、读取、删除、保留数量上限（后台工作线程）
src/ui        仅依赖 Dear ImGui：HUD 叠加层、设置菜单、任务总结与 G 曲线、任务历史窗口、布局计算
apps/macos_proto  SDL3 + Metal 宿主：模拟的游戏画面和调试面板
apps/replay_cli   无界面回放、统计、回归检查、日志生成
apps/latency_bench  不同采样率与噪声下 G 显示延迟/误差基准（自适应 vs 固定滤波）
tests/        doctest 测试
data/replays  参考飞行日志
assets/fonts  B612 Mono（SIL OFL 1.1）
```

各层依赖关系：`core ← sources`，`core ← config ← ui`，`core ← storage ← ui`。core、config、storage、ui 将来直接复用到 UE4SS Mod 中，只需新增 Windows 专用的数据源（`ITelemetrySource`）和 DX Present Hook 宿主。

## 扩展点

- **新数据源：** 实现 `ft::ITelemetrySource`（`src/core/source.h`）。
- **订阅指标流：** 实现 `ft::IMetricsConsumer`（`src/core/pipeline.h`）。后续的黑视、红视等生理模型会通过它接入。
- **新的叠加绘制层：** 实现 `ft::IOverlayLayer`（`src/ui/overlay_layer.h`）并调用 `TelemetryUi::addLayer()` 注册。

## 致谢

- [Dear ImGui](https://github.com/ocornut/imgui)
- [SDL](https://github.com/libsdl-org/SDL)
- [nlohmann/json](https://github.com/nlohmann/json)
- [doctest](https://github.com/doctest/doctest)
- [B612](https://github.com/polarsys/b612) 字体

参考项目：AC8 Tweaks、AC8 Per-View FOV、AC7 Cockpit。
