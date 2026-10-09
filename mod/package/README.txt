AC8 G-Meter - in-game G meter overlay for ACE COMBAT 8: Wings of Theve
=======================================================================

OFFLINE SINGLE PLAYER ONLY. The mod refuses to start unless the game was launched with the
anti-cheat null client (see 2.). Never use mods online.

1. Install / update / uninstall
   Put AC8GMeter-Installer.exe in the ACE COMBAT 8 folder (the one containing Game and
   EasyAntiCheat) and run it. It installs:
   - UE4SS v3.0.1 (e3ba1016), the mod loader most AC8 mods use, only if the game has no UE4SS
     yet (an existing UE4SS is used as it is);
   - this mod into Game\Binaries\Win64\<UE4SS folder>\Mods\AC8GMeter;
   - EasyAntiCheat\AC8_Offline.json for the offline launch (if missing).
   Run it again to update (settings and mission history are kept) or to uninstall. UE4SS is
   removed only if the installer put it there and no other mod uses it.

   Manual install: copy this AC8GMeter folder into ...\Win64\<UE4SS folder>\Mods\. UE4SS needs
   HookEngineTick = 1 in UE4SS-settings.ini.

2. Offline launch
   Steam > ACE COMBAT 8 > Properties > Launch Options (the installer copies this line):
       cmd /d /c "set EOS_USE_ANTICHEATCLIENTNULL=1&& %command% -anticheat_settings=AC8_Offline.json"
   EasyAntiCheat\AC8_Offline.json is the game's Settings.json plus "allow_null_client": "true".
   To play online again: remove the launch option and uninstall (or disable UE4SS).
   (The AC8 Tweaks offline launcher, which sets AC8TWEAKS_OFFLINE=1, is accepted too.)

3. Use
   Ctrl+F8  settings menu          Ctrl+F9  move the overlay (drag), again to finish
   Ctrl+F7  read-only state probe (diagnostics: writes probe-*.txt)
   After each sortie the debrief opens with the G chart. Files in the mod folder:
     settings.json, missions\ (mission history), recordings\ (flight CSV, see
     Scripts\config.lua), ac8gmeter.log
   Borderless or windowed display mode recommended.

4. Known limitations (first version)
   A mission counts from taking control of the aircraft until it has been gone for 3 s
   (results, menus, loading; paused time does not count). Quitting a mission is recorded like
   a finished one; a retry whose aircraft appears within 3 s merges with the previous attempt;
   a cutscene of more than 3 s without the aircraft splits a mission in two.

Third-party software: see THIRD_PARTY_NOTICES.txt (UE4SS: UE4SS_LICENSE.txt,
UE4SS_THIRD_PARTY_LICENSES.txt).
