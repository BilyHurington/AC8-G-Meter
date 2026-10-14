**Offline single player only.** The mod starts only when Easy Anti-Cheat is not active. Never use mods online.
**仅限离线单人模式。** 仅在 Easy Anti-Cheat 未运行时启动，切勿联机使用 mod。

| File | Use |
| --- | --- |
| `AC8GMeter-*-Installer.exe` | **Recommended.** Put it in the ACE COMBAT 8 folder (the one with `Game` and `EasyAntiCheat`) and run it. Installs UE4SS (only if missing) and the mod; run it again to update or uninstall. <br>推荐：放到游戏根目录双击运行；再次运行可更新或卸载。 |
| `AC8GMeter-*-Full.zip` | UE4SS + the mod. Extract into the ACE COMBAT 8 folder (merges with `Game`). Only for a game without UE4SS. <br>含 UE4SS：解压到游戏根目录；仅适用于尚未安装 UE4SS 的情况。 |
| `AC8GMeter-*-ModOnly.zip` | The mod only, for an existing UE4SS: copy `AC8GMeter` into `Game\Binaries\Win64\<UE4SS folder>\Mods\`. <br>不含 UE4SS：将 `AC8GMeter` 复制到已有 UE4SS 的 `Mods` 目录。 |
| `SHA256SUMS.txt` | Checksums of the files above. |

Offline launch, controls and known limitations: see `AC8GMeter-README.txt` in the zips (or `README.txt` in the installed mod folder).
离线启动方法、快捷键（Ctrl+F8 设置，Ctrl+F9 移动）与已知限制见压缩包内的 `AC8GMeter-README.txt`。

Known issue: one game crash with DLSS Frame Generation on was seen in testing (cause not yet isolated). If the game crashes, try turning Frame Generation off, or set `Overlay = false` in `Scripts\config.lua`.
已知问题：测试中出现过一次开启 DLSS 帧生成时的崩溃（原因尚未确定）。如遇崩溃，请尝试关闭帧生成，或在 `Scripts\config.lua` 中设置 `Overlay = false`。

Bundled: [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) v3.0.1 (e3ba1016, MIT) — licenses included.
