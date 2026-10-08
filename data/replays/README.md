# Replay logs

Simulated flight logs with exact ground truth (`true_nz`, `true_aoa`), produced by the
point-mass flight model in `src/sources`. Format: see `src/sources/csv_format.h`.
Every log flies with phase `active` and ends with ~1 s of `ended`, so a replay produces a
mission report. Each `*.csv` here is registered as a ctest (`replay_cli <log> --check`).

| File | Scenario | Notes |
|---|---|---|
| `level_cruise.csv` | level, 30 s | ~1 G baseline |
| `sustained_turn.csv` | turn, 40 s | ~82° bank, ~7 G held 8 s, alternating direction |
| `pull_push.csv` | pullpush, 30 s | +5 G pull, unload, −2 G push |
| `dogfight_mix.csv` | dogfight, 90 s | seeded random manoeuvres, about −3 to +9 G |
| `dirty_dogfight.csv` | dogfight, 60 s | noise, jitter, drops, duplicates, 1 spike, 1 hitch, 1 teleport; looser `# check:` limits |
| `position_only_turn.csv` | turn, 30 s | no velocity columns (estimator differentiates position) |

Regenerate (from the repo root, after building `replay_cli`):

```sh
R=build/apps/replay_cli/replay_cli
$R --generate level    --duration 30 --out data/replays/level_cruise.csv       --seed 1
$R --generate turn     --duration 40 --out data/replays/sustained_turn.csv     --seed 2
$R --generate pullpush --duration 30 --out data/replays/pull_push.csv          --seed 3
$R --generate dogfight --duration 90 --out data/replays/dogfight_mix.csv       --seed 4
$R --generate dogfight --duration 60 --out data/replays/dirty_dogfight.csv     --seed 5 --dirty
$R --generate turn     --duration 30 --out data/replays/position_only_turn.csv --seed 6 --no-velocity
```

`--check` compares the pipeline's G (Nz) and AoA with the truth over valid samples, skipping
0.5 s after estimator resets. Smoothing delays the estimate by design, so the tool also finds
the delay (0–300 ms) that best aligns estimate and truth. The check passes when the
**aligned** RMS error is within the limits (default 0.15 G / 0.5°, or a
`# check: g_rms<=X aoa_rms<=Y` comment in the log) and the delay is at most 250 ms. The output
also prints the unaligned error.
