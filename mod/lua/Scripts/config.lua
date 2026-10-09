-- AC8 G-Meter: options read when the game starts. Everything else is in the in-game
-- settings menu (Ctrl+F8), saved to settings.json in the mod folder.
return {
    -- Write every mission's flight data to recordings\ac8_flight_*.csv in the mod folder
    -- (about 6 MB per 10 minutes at 60 fps). Used to tune the G filters on real game data.
    RecordCsv = true,
}
