# AC8 Flight Telemetry

《皇牌空战8：Wings of Theve》的飞行遥测叠加层 Mod。在游戏画面上以透明文字的形式实时显示 G 值、G Onset、迎角等飞行数据，并在任务结束后给出飞行统计。不修改原版 HUD。

**当前阶段：** macOS 独立原型。使用模拟飞行数据和可回放的 CSV 日志，不依赖游戏本体。之后会移植到 Windows，通过 UE4SS 接入 AC8。

## 功能

- **实时指标：** 当前 G、MAX G、MIN G（默认显示）。可选显示 G Onset（G/s）、AoA、持续高 G 时间、侧滑角、速度。
- **数据处理：**
  - 用最小二乘法从带时间戳的速度或位置估计加速度，能适应帧间隔不均匀的情况；
  - 中值滤波加 EMA 平滑；
  - 识别并处理异常值：尖峰、卡顿、瞬移、重复帧、NaN。
- **叠加层：**
  - 透明背景，浅绿色 `#93F6A8` 文字，带轻微阴影；
  - 默认位置为屏幕的 (4.5%, 16%)，1080p 下约 (86, 173)；
  - 1080p 下字号 16 px，随分辨率缩放；
  - 正常模式下不拦截鼠标；
  - 编辑模式下可以拖动，支持位置锁定、字号、行距、透明度、颜色、背景设置，以及选择显示哪些指标。
- **设置菜单：** 按快捷键打开。设置自动保存为 JSON，支持恢复默认值。
- **任务总结：** 任务结束后自动弹出，内容包括最大/最小 G 及发生时刻、Onset 极值、最大 AoA、持续高 G 时间、G 区间停留时间直方图、G 时间曲线和数据质量统计。

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
./build/apps/macos_proto/ft_macos_proto
```

常用参数：

| 参数 | 作用 |
|---|---|
| `--scenario SustainedTurn` | 选择模拟场景。可选值：LevelCruise / SustainedTurn / PullUpPushOver / DogfightMix / Manual |
| `--size 1280x720` | 设置窗口尺寸 |
| `--settings <file.json>` | 使用指定的设置文件。默认为 `~/Library/Application Support/AC8FlightTelemetry/settings.json` |
| `--menu` / `--edit` | 启动时直接打开设置菜单 / 进入编辑模式 |
| `--end-mission-at <s>` | 在第 s 秒自动结束任务，并弹出任务总结 |
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

## 测试

```bash
ctest --test-dir build --output-on-failure
```

测试包括：

- core、sources、ui 三层各自的 doctest 单元测试；
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
src/ui        仅依赖 Dear ImGui：HUD 叠加层、设置菜单、任务总结、布局计算
apps/macos_proto  SDL3 + Metal 宿主：模拟的游戏画面和调试面板
apps/replay_cli   无界面回放、统计、回归检查、日志生成
tests/        doctest 测试
data/replays  参考飞行日志
assets/fonts  B612 Mono（SIL OFL 1.1）
```

各层依赖关系：`core ← sources`，`core ← config ← ui`。core、config、ui 将来直接复用到 UE4SS Mod 中，只需新增 Windows 专用的数据源（`ITelemetrySource`）和 DX Present Hook 宿主。

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
