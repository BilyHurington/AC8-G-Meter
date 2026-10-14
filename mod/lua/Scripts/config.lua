-- AC8 G-Meter: options read when the game starts. Everything else is in the in-game
-- settings menu (Ctrl+F8), saved to settings.json in the mod folder.
return {
    -- Write every mission's flight data to recordings\ac8_flight_*.csv in the mod folder
    -- (about 6 MB per 10 minutes at 60 fps). Used to tune the G filters on real game data.
    RecordCsv = true,

    -- Debug recording from the start of the game (every frame's raw data to debug\ac8_debug_*.csv,
    -- about 130 MB per hour). It can also be switched on and off in game: Ctrl+F8 > Diagnostics;
    -- that switch is off again at the next start.
    DebugRecording = false,

    -- Draw the overlay (HUD, menus, debrief) into the game. With false nothing is drawn and the
    -- game's rendering is not hooked at all; G is still computed, missions are still saved and
    -- recorded. For finding out whether a graphics problem comes from the overlay.
    Overlay = true,
}
