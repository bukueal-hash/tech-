# Debug channel status

Retirement rule: **5–7 clean completed raids** with no junk/errors → remove that channel's NDJSON logging.
User override 2026-07-19: retire healthy channels now so logs stay focused on remaining work.

Last reviewed: 2026-07-19 (Fix #14 retirement pass)

| Channel | Clean raids | Status | Notes |
|---|---:|---|---|
| `name_trace_world` | 7 | **RETIRED** | Real labels only. |
| `name_trace_bot` | 7 | **RETIRED** | Real bot names only. |
| `name_trace_player` | 7 | **RETIRED** | Real Steam names. |
| `bot_label_miss` | 7 | **RETIRED** | 0 hits. |
| `paint_frame` | — | **RETIRED** | 240fps / 4–5ms healthy. |
| `home_paint` / `esp_paint` | — | **RETIRED** | Paint path healthy. |
| `frame_build` / `pos_refresh` / `cam_refresh_gap` | — | **RETIRED** | Camera + refresh healthy (cam ~27ms). |
| `player_collect` / `player_ghost` / `player_admit_ring` / `ally_team_ids` | — | **RETIRED** | Players + ally hide healthy. |
| `bot_scan` / `bot_admit_batch` / `bot_retain_batch` / `bot_retain_defer` / `bot_grace` / `bot_verify_fail` | — | **RETIRED** | Steady bot pipeline; verify memo holding. |
| `flicker_score` | — | KEEP | paintWorld projFail still spikes. |
| `perf_spike` | 0 | KEEP | Scan passes still heavy under load. |
| `scan_gate` | 0 | KEEP | Coupled to perf. |
| `bot_nopos` / `bot_pos_freeze` | 0 | KEEP | Live Wasps / Vaporizer still zero-pos. |
| `container_*` / `item_*` / `world_draw_caps` | — | KEEP | Still validating loot/containers. |
| `raid_hb` / `raid_entered` / `raid_left` | — | KEEP | Watcher depends on these. |

When retiring: delete only `#region agent log` / NDJSON emitters; leave functional gates/fixes.
