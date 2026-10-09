# UE4SS runtime (vendored)

The AC8 G-Meter installer ships this UE4SS build so players do not have to install UE4SS
separately. It is the build most AC8 mods are made for (AC8 Tweaks, AC8 Per-View FOV,
AC8-Mouse-Aim), which keeps the mods compatible when they share one UE4SS install.

- **Version:** RE-UE4SS `v3.0.1-1152-ge3ba1016` (experimental-latest of 2026-09-29),
  commit [`e3ba1016`](https://github.com/UE4SS-RE/RE-UE4SS/commit/e3ba1016562d6c0868c410d0a71e88bfcdbf691b).
- **License:** MIT, see [LICENSE](LICENSE). Components built into `UE4SS.dll`:
  [notices/THIRD_PARTY_LICENSES.txt](notices/THIRD_PARTY_LICENSES.txt), with the full license
  texts; the installer puts it next to `UE4SS.dll`.
- **Source of these files:** the official release asset was removed when
  `experimental-latest` moved on. These are byte-identical copies from
  [AC8-Mouse-Aim](https://github.com/FletcherMiya/AC8-Mouse-Aim/tree/052cd6aeef89aa431fff35bf9180331be5d99a4f/Payload/Game/Binaries/Win64)
  (commit `052cd6ae`), which redistributes the official release. `UE4SS.dll` has the
  same SHA-256 as the build AC8 Per-View FOV pins.
- **Settings:** `UE4SS-settings.ini` is the AC8 community file (`HookEngineTick = 1`, GUI
  console off). The folder layout (`UE4SS\` + `override.txt`) matches AC8-Mouse-Aim's.

`payload/` is copied as is into `Game\Binaries\Win64\` by the installer:

| File | SHA-256 |
|---|---|
| `dwmapi.dll` (UE4SS injection proxy) | `cf440b9eb8643bb7c434acfda696aee57fd981d185dca5e57fb8dbb18f8fc1cd` |
| `override.txt` | `3f49fec48a6980e3df3dbf85dda29577dfb65a48af76755c11922e9f3d276a94` |
| `UE4SS/UE4SS.dll` | `680a026890abb4d0df2211251f8defc1681a584275f1521dcc0fe30af480006f` |
| `UE4SS/UE4SS-settings.ini` | `88962063029d9f1b67b1eebb903b0c8509a7864e81c70ac03e13d99d07542539` |
| `UE4SS/UE4SS_SDK_Backends/UE4SS.json` | `2c309da34012ed0cc0d3a4b6a34c8287ec6b0011b1f864fd3e8424edff4fb154` |
| `UE4SS/LICENSE` | `ddc030e25d0ea87aca4ae84c0ed3f868d69273c00c0c12ea1e26f1c6130f5d2e` |

The build checks these hashes (`cmake/Ue4ssPayload.cmake`), so a changed file fails the build.
