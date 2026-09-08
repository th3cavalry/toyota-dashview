# LVGL 9 Migration Plan

Branch: `feat/lvgl-port` · Base: `main` @ 6b1a554 · PR #8 (tracking)

## Why this migration (the flicker, for real)

The flicker is structural in LovyanGFX's RGB path, not a timing bug:

- LGFX `Bus_RGB::init()` allocates **one** PSRAM framebuffer (`heap_alloc_psram`,
  verified in the pinned 1.1.16) and GDMA scans it out **from Octal PSRAM** live.
  `cfg.use_psram = 1` is dead code ("unimplemented… TODO" — still true in 1.2.28,
  where Panel_RGB.hpp is marked 暫定実装 / single PSRAM buffer).
- PR #7 (merged) removed the per-frame 768 KB canvas→FB copy and aliased canvas
  onto the panel FB. Writes now hit the same PSRAM GDMA is streaming — bursts of
  PSRAM contention starve the LCD FIFO → horizontal shear. Measured on the bench:
  Wi-Fi screen flickers ~1/s (= its 1 Hz refresh), dynamic screens ~10/s
  (= telemetry poller rate). Flicker events ≈ content changes: the tell.

**The fix is the migration.** `esp_lcd_panel_rgb` + LVGL 9 gives what LGFX never
will here: `bounce_buffer_size_px` keeps scan-out in **internal SRAM** (CPU PSRAM
writes stop contending with GDMA), DMA-2D copies bounce→FB, and LVGL 9's partial
renderer invalidates **dirty rects**, not the whole 800×480 frame.

## Scope (what gets ported)

| Area | Current source | Target |
|---|---|---|
| Panel + RGB bus init | LGFX classes inline, `main.cpp:36–117` (16×RGB565 D0–15, DE=5 VS=3 HS=46 PCLK=7, pclk 14 MHz) | `esp_lcd_panel_rgb` only — verified: LGFX `pin_cs` defaults −1 and main.cpp never sets it → ST7701 SPI init never runs, board is pure RGB scanout |
| Touch GT911 @0x5D INT GPIO4 | `pollTouch()` raw I2C | LVGL indev; keep 11a218d's track-register fix (old UI navbar hit-boxes die with the custom UI) |
| Backlight CH422G EXIO2 | `dimScreen/wakeScreen` `main.cpp:866/875` | keep CH422G driver; LVGL `lv_display_set_brightness` |
| Boot splash (TRD logo) | `toyota_splash.h` (1021-line bitmap arr) | `lv_image` + `LV_IMG_CF_TRUE_COLOR` |
| Custom UI (6 screens) | `main.cpp` render fns + `custom_dash.inl` | LVGL screens: header + tabview |
| Profile engine / CAN / OBD / datalogger / Wi-Fi / SD | `profile.*`, TWAI, SD SPI, SavvyCAN | **unchanged** — headless logic keeps existing API; UI reads via accessors |
| OOBE wizard (PR#6) | `main.cpp:2196` (802e648) | wizard screen, 3 steps |
| OTA updater (PR#6) | `updater.cpp` (302 ln, 802e648) | confirm dialog + progress bar; `default_16MB.csv` A/B |
| Telemetry sim (PR#6) | `telemetry_sim.cpp` (149 ln) | simulator screen |
| Native tests | `tests/native` (46 tests) | extend to profile/sim/updater logic |
| Docs | AGENTS.md / BENCH.md / HANDOFF.md / ISSUE-TO-DO.md | rewrite for LVGL architecture |

## Sections (each = one PR onto `feat/lvgl-port`)

- **S1 — Skeleton & platform**: LVGL 9.3 (`lib_deps = lvgl/lvgl@^9.3`), `lv_conf.h`
  via `-DLV_CONF_PATH` (`LV_LVGL_H_INCLUDE_SIMPLE`), 16-bit color, `LV_COLOR_16_SWAP`
  off (RGB565 byte order via esp_lcd), `LV_USE_LOG` off. esp_lcd RGB + SPI-IO init,
  ST7701 3-wire-SPI init seq, `bounce_buffer_size_px = 800` (~7 KB SRAM/line × N),
  DMA-2D on. Display bound to the single PSRAM FB in **DIRECT** mode. GT911 indev
  (register-offset fix), CH422G backlight, flip180. Merge gate: splash renders,
  60 Hz steady, touch reads correct coords, **zero flicker under Wi-Fi AP + CAN load**.
- **S2 — Dashboard + Custom Dash**: header pill, 6 gauges (rpm/speed/temp/fuel/
  gear/text) via profile accessors; custom-dash add/drag/resize/styles/edit-mode;
  splash → dashboard tap-to-dismiss.
- **S3 — Sniffer / Logger / Settings / System**: raw-CAN modal, PID selector modal,
  datalogger config + SD state, Settings cards (profile picker cells y272–314 /
  x34/222/410/598 preserved), Wi-Fi + System screens, stale-signal `--` gate.
- **S4 — OOBE + OTA (PR#6 port)**: port 802e648's logic onto LVGL; 3-step wizard
  (vehicle / toggles / orientation); SD OTA dialog + progress bar, `update.bin`
  rename; web portal `192.168.4.1:80 /update`; `huge_app.csv` → `default_16MB.csv`
  (A/B + rollback); `memory_type=qio_opi` kept.
- **S5 — Telemetry simulator + tests**: `telemetry_sim.cpp` ported; sim toggle pill
  preserved; native harness covers profile + sim + updater logic (target ≥46/46, CI).
- **S6 — Cutover**: delete LGFX + custom UI (`custom_dash.inl`, splash bitmap
  → LVGL asset, `src/main.cpp` render fns, old touch handler); AGENTS.md
  architecture section rewritten; BENCH/HANDOFF/ISSUE-TO-DO refresh; v0.5.0.

## Bench acceptance (BENCH.md, every merge)

Boot splash → tap → cluster steady; all 6 screens swiped; no shear at Wi-Fi AP +
CAN idle; profile hot-swap without reboot; pull SD → `--` ≤2 s; reinsert → rescan.
