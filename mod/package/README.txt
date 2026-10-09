AC8 G-Meter - in-game G meter overlay for ACE COMBAT 8: Wings of Theve
=======================================================================

OFFLINE SINGLE PLAYER ONLY. The mod refuses to start unless the game was launched with the
anti-cheat null client (see 2.). Never use mods online.

1. Requirements
   - UE4SS installed in Game\Binaries\Win64 (dwmapi.dll + ue4ss folder) with HookEngineTick=1.
     The UE4SS builds used by AC8 mods (v3.0.1 03dbd5c0, e3ba1016, experimental 6eb3d9bc) work.
   - Borderless or windowed display mode recommended.

2. Offline launch (community standard method)
   - Copy EasyAntiCheat\Settings.json to EasyAntiCheat\AC8_Offline.json and add the line
       "allow_null_client": "true",
   - Steam launch option:
       cmd /d /c "set EOS_USE_ANTICHEATCLIENTNULL=1&& %command% -anticheat_settings=AC8_Offline.json"
   To play online again: remove the launch option and disable UE4SS (rename dwmapi.dll).
   (The AC8 Tweaks offline launcher, which sets AC8TWEAKS_OFFLINE=1, is accepted too.)

3. Install
   Copy the AC8GMeter folder into Game\Binaries\Win64\ue4ss\Mods\ (the folder contains
   enabled.txt, so no mods.txt entry is needed).

4. Use
   Ctrl+F8  settings menu          Ctrl+F9  move the overlay (drag), again to finish
   Ctrl+F7  read-only state probe (diagnostics: writes probe-*.txt)
   After each sortie the debrief opens with the G chart. Files in the mod folder:
     settings.json, missions\ (mission history), recordings\ (flight CSV, see
     Scripts\config.lua), ac8gmeter.log

5. Known limitations (first version)
   A mission counts from taking control of the aircraft until it has been gone for 3 s
   (results, menus, loading; paused time does not count). Quitting a mission is recorded like
   a finished one; a retry whose aircraft appears within 3 s merges with the previous attempt;
   a cutscene of more than 3 s without the aircraft splits a mission in two.

Third-party software: see THIRD_PARTY_NOTICES.txt.
