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
| `position_only_turn.csv` | turn, 30 s | no velocity columns (estimator differentiates position over the longer `accelWindowPositionOnlyS`); `# check: latency_ms<=180` |

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
0.5 s after estimator resets, and prints two errors:

- **zero-lag** RMS: estimate vs truth at the same instant, i.e. what the pilot sees. It includes
  the estimator delay, so it also guards against latency regressions.
- **aligned** RMS: after shifting the truth by the delay (0–300 ms) that best aligns the two,
  i.e. the estimation quality with the delay removed. The delay itself is reported as `lag`.

The check passes when, for G, the zero-lag RMS is at most 0.70 G and the aligned RMS at most
0.15 G; for AoA, the aligned RMS is at most 0.5°; and the delay is at most 120 ms. A log can
override any of these with a comment line such as
`# check: g_rms0<=0.8 g_rms<=0.35 aoa_rms<=1.5 latency_ms<=180` (`g_rms` is the aligned limit).

With the default config (median 5 samples, τ = 0.05 s, 0.08 s velocity window) the delay is
~105 ms with velocity input and ~145 ms position-only (0.15 s window); the zero-lag G RMS is
0.27–0.54 G on the manoeuvring logs (dogfight_mix is the worst; the 0.70 G limit is ~25% above it).
