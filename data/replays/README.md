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
| `position_only_turn.csv` | turn, 30 s | no velocity columns (estimator differentiates position over the fixed, longer `accelWindowPositionOnlyS`); looser `# check:` limits |

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

The check passes when, for G, the zero-lag RMS is at most 0.17 G and the aligned RMS at most
0.15 G; for AoA, the aligned RMS is at most 0.5°; and the delay is at most 35 ms. These defaults
are for clean velocity logs. A log can override any of them with a comment line such as
`# check: g_rms0<=0.8 g_rms<=0.35 aoa_rms<=1.5 latency_ms<=180` (`g_rms` is the aligned limit).

The default config uses adaptive filtering: the velocity least-squares window, EMA τ and onset
window follow the measured sample rate and velocity noise (`PipelineConfig::adaptiveFiltering`),
and the median filter is off. Measured (limits are ~30% above):

| Log | Filter (end of log) | Delay | Zero-lag G RMS |
|---|---|---|---|
| clean velocity logs (60 Hz) | adaptive, window 42 ms, τ 17 ms | 25 ms | 0.00–0.13 G (dogfight_mix worst) |
| `dirty_dogfight.csv` (0.05 m/s noise) | adaptive, window ~115 ms, τ ~45 ms | 85 ms | 0.42 G |
| `position_only_turn.csv` | fixed, 0.15 s position window, τ 50 ms | 110 ms | 0.29 G |

With the old fixed defaults (0.08 s window, median 5, τ 0.05 s) the delay was ~105 ms on every
velocity log and the zero-lag RMS up to 0.54 G. `build/apps/latency_bench/latency_bench`
compares adaptive and fixed filtering across sample rates and noise levels.
