# PROJECT STATE — for AI assistants

> Read this first. It is the authoritative snapshot of what is done, in progress,
> and open. Update it in the same commit as the work it describes.
> Human-facing docs: README.md. Profile JSON schema: profiles/SCHEMA.md.

Last updated: 2026-09-04 (feat/4.3b-migration)

## What this project is

Toyota DashView: an ESP32-S3 (Waveshare ESP32-S3-Touch-LCD-4.3B, 800x480 RGB)
standalone dash / CAN tool for Toyota vehicles — decodes CAN broadcasts + OBD-II
(ISO-TP) traffic, renders gauges, datalogs to MicroSD, streams frames to SavvyCAN
over Wi-Fi. Target vehicle today: 2016-2023 Tacoma (2GR-FKS / AC60).

## Architecture (how the pieces fit)

- `src/main.cpp` — monolith (~2.4k lines): TWAI CAN driver, ISO-TP reassembly,
  OBD poller, LVGL-free LovyanGFX UI (6 screens via `currentScreen`, software-
  rotated RGB framebuffer in PSRAM), datalogging, Wi-Fi SavvyCAN bridge, touch.
- `src/profile.h/.cpp` — **vehicle profile engine** (done). Parses a JSON profile
  into a signal table; `onBroadcastFrame()` decodes CAN broadcasts (Motorola/
  Intel bitfields, enums), `onObdPollResponse()` decodes polled PIDs (a*(A|B)+c,
  lambda→AFR, Toyota 0x21 modes). Accessors: `getSignal/getSignalText/signalAge/
  getReqId/getRespId/getProfileId/...`. Max 48 signals, 8 enum entries each.
- `profiles/*.json` — vehicle data files (see SCHEMA.md). `j1979_base` is the
  inherited universal baseline; `toyota_tacoma_2016_2023` layers proprietary deltas.
  Profile = data, never code: adding a car should mean adding a JSON file.
- Runtime flow: `processCAN()` → `decodeTacomaFrame()` (legacy hardcoded decode,
  still present) + `onBroadcastFrame()`/ISO-TP→`decodeObdPayload()`→
  `onObdPollResponse()` → at 30 FPS `syncProfileSignals()` copies FRESH profile
  signals (< 1.5 s old) into `vehicleData` (the legacy gauge struct) → `updateDisplay()`.
  Profile values win over legacy decode because sync runs after CAN processing.

## DONE (verified, building)

- [x] Profile engine (`profile.h/.cpp`, ArduinoJson 7) — compiles, wired.
- [x] Profile-driven OBD addressing: hardcoded 0x7E0/0x7E8 macros removed;
  TX uses `getReqId()`, ISO-TP RX keys on `getRespId()` (commit a989735).
- [x] `syncProfileSignals()` freshness-gated copy into `vehicleData` (a989735).
- [x] SD profile override in `setup()` step 7b: NVS `dashview/prof` →
  `/profiles/<id>.json` → `loadProfile()`; built-in Toyota profile is fallback.
- [x] Settings screen Vehicle Profile picker: `scanProfileDir()` lists up to 6
  `/profiles/*.json` (UI shows 3 + BUILT-IN cell); tap = `applyProfileSelection()`
  hot-swaps engine (no reboot) + persists NVS; re-scan on SD hot-remount.
- [x] `format_sd.sh` prints profile-seeding instructions.
- [x] **Native unit tests** (`tests/native/run.sh`, 41 assertions): profile
  engine runs natively via a 12-line Arduino shim (only `PROGMEM`/`millis()`
  needed; ArduinoJson is header-only). Covers load/inheritance, Motorola
  bitfield decode, gear enum + default map, OBD poll math (incl. Toyota
  0x21 clamps + signed knockfb, lambda→AFR), freshness, signal metadata /
  poll-id (Custom Dash binding), malformed-JSON robustness.
  Two real bugs found & fixed: `getSignalMeta` auto-decimals used `scale`
  instead of `a` for obd_poll; `loadProfile("{}")` wiped the live signal
  table (now requires `signals` or `inherits`).
- [x] Waveshare 4.3B migration (PR#2 lineage): ISO-TP, bus-off recovery, NVS
  file counter, CH422G expander, GT911 touch, PCF85063 RTC, TRD UI.
- [x] **Hardware Flash & Boot Verified (2026-09-05 / 2026-09-11)**: Successfully flashed to
  physical Waveshare 4.3B hardware via `/dev/ttyACM0`. Verified live boot log:
  CH422G IO expander OK, 800x480 PSRAM framebuffer OK, GT911 touch OK, PCF85063
  RTC OK, TWAI CAN initialized (TX:15, RX:16 @ 500k), Wi-Fi AP + SavvyCAN server live,
  Profile engine active (`toyota_tacoma_2016_2023`).
- [x] **Production-Grade Visual Overhaul & Zero-Overlap Screen Redesign (2026-09-11)**:
  - Added public `textWidth()` and `fontHeight()` methods to `LVGLCanvas` for accurate layout bounding.
  - Added Montserrat 24 (`Font5`) and Montserrat 28 (`Font6`) to font subsystem for balanced automotive typographic hierarchy.
  - Redesigned Header: TRD tri-color accent bar, Montserrat 20 screen title, recessed message-rate badge with live status dot, styled SD/REC status pill.
  - Redesigned Bottom Nav: Sleek interactive `< PREV` and `NEXT >` pill buttons, centered active capsule and inactive dot page indicators.
  - Cluster Dashboard (`renderDashboard`): Full-width recessed tachometer track (0-6000 RPM) with redline zone (5200-6000), graduated color bands (Cyan -> Orange -> Redline), tick marks, and prominent digital readout (`Font5`); balanced hero cards for Transmission (centered Font7 gear and Gold TCC lockup pill), AFR Wideband (target vs actual wells, lambda sub-metrics, and horizontal bar with 14.7 center pip), Knock Health (octane learning KCLV, retard wells, and dynamic status pill), mini throttle & load gauges with recessed tracks, and 4 inset status tiles (Wi-Fi, CAN frames, Free Heap/PSRAM, RTC).
  - Cleaned up Secondary Screens: Standardized System screen rows (`rowStep = 44`) with row dividers to eliminate bottom edge clipping and fixed RTC partial update row target; Sniffer and Logger cards with inset metric wells and styled launcher buttons; Wi-Fi screen with inset status rows and structured help box; Custom Dash recessed hbar/vbar wells and Font5 readouts.
  - Touch Alignment: Synchronized hitboxes in `handleTouch()` with cards and buttons across all screens.
  - Direct On-Device Screenshot Capture (`tools/capture_screenshot.py`): Added serial framebuffer streaming (`'c'` command dumps 768 KB raw RGB565 over USB CDC in <1s), enabling direct pixel-accurate PNG captures from the physical hardware.
  - Text Datum Alignment Bug Fixed: Discovered and resolved persistent `_datum = 1` from boot splash which was erroneously shifting all `drawString` calls left by half their width (causing truncation of left characters and multi-column overlap). Enforced `_datum = 0` default on screen fills and headers.
  - Build & Bench Verification: Clean compilation with 0 compiler warnings/errors (RAM 37.6%, Flash 23.0%), native tests 46/46 passed, flashed and verified on physical Waveshare 4.3B hardware via `/dev/ttyACM0`.
  - PSRAM Contention & Lower Display Artifacts Eliminated (2026-09-11):
    - Root-caused bottom-screen shearing and stretching: 1 Hz Status Ribbon updates were redrawing entire 776x72 outer card, 4 inner cards, and borders to PSRAM while GDMA was scanning out at 14 MHz, causing PSRAM FIFO underflow. Fixed with dirty caching: outer card and borders drawn strictly on `forceFull`; 1 Hz loop only clears minimal text bounding box (`tw - 8, 28`) when values change, reducing periodic PSRAM write bandwidth by >97%.
    - Removed harsh 800px full-width `drawFastHLine` in `drawBottomNavBar()` that caused a floating glitch line between `< PREV` and `NEXT >`.
    - Modernized `fillCircleHelper`, `fillCircle`, `drawRoundRect`, and `fillRoundRect` in `src/display.cpp` to use continuous scanline algorithms with zero gaps or ragged corner artifacts.
    - Seamless Bottom Navbar Page Transitions (2026-09-11):
    - Root cause of bottom blanking / sequential redraw: on screen change, `canvas.fillScreen(C_DARK_BG)` was wiping scanlines 440..480 to black, leaving the bottom area blank for 2-3 frames while the upper cards rendered. Then `drawBottomNavBar()` repainted the background, `< PREV`, each dot sequentially, and `NEXT >`.
    - Fix: `updateDisplay()` now clears strictly the content area (`canvas.fillRect(0, 0, UI_W, UI_H - UI_NAVBAR_H, C_DARK_BG)`), leaving the bottom navbar completely untouched. `drawBottomNavBar(forceFull)` caches the background, `< PREV`, and `NEXT >` pills persistently across page switches, updating only the 176x16 px dot indicator bounding box in 50 microseconds before content renders. Bottom navigation is now rock-solid and never blinks or redraws sequentially.
  - **Performance & Touch Responsiveness Overhaul (2026-09-11)**:
    - Root-caused screen "unpacking" / slow wipe: 16-bit unaligned `std::fill` across 440 lines in Octal PSRAM triggered read-modify-write stalls, taking 50–80 ms per transition.
    - Added `LVGLCanvas::fillContentArea(uint16_t c)`: 32-bit burst write loop (`uint32_t *p32 = c32`) over upper 440 scanlines, reducing clear time from ~80 ms to <2 ms while preserving bottom navbar and resetting `_datum = 0`.
    - Rewrote `LVGLCanvas::drawFastVLine`: replaced pixel-by-pixel `px()` call loop with direct row-stride pointer arithmetic (`uint16_t *p += stride`), bringing vertical line drawing to hardware bus memory speed.
    - Tachometer Sweep Optimization: replaced loop of hundreds of single vertical line calls with 1–3 direct block `fillRect` calls.
    - Instant Touch Page Switching: removed frame timer latency; `nextScreen()`, `prevScreen()`, and dot taps in `handleTouch()` invoke `updateDisplay()` immediately on tap/swipe release.
    - Improved Tap Detection: relaxed tap duration filter from 600 ms to 1200 ms and extended navbar hit-box upward by 15 px (`touchLastY >= 425`), preventing missed or sluggish taps.
  - **Display Stability, Timing & DCache Write-Back Fixes (2026-09-12)**:
    - **ST7262 PCLK Phase & Timing Flags (`9c39d25`)**: Fixed letter shimmering, color fringing, and micro-jitter by setting `pclk_active_neg = 1` and panel sync polarities (`hsync_idle_low = 0`, `vsync_idle_low = 0`, `de_idle_high = 0`). The ST7262 panel samples data on the falling PCLK edge.
    - **Raw CAN Sniffer Modal / Page Shearing Fix (`3c5f17d`)**: Eliminated horizontal screen shifting and tearing during active CAN streaming by avoiding direct PSRAM contention and staging terminal redraws.
    - **CPU DCache Write-Back Synchronization (`8dc426b`)**: Root-caused missing horizontal glyph scanlines ("text cut off at top and middle") and "unpacking" delay. The ESP32-S3 uses write-back L1 DCache for PSRAM, but direct GDMA scanout (`bounce_buffer_size_px = 0`) reads physical external PSRAM directly via DMA/AXI. Added `esp_cache_msync()` with `ESP_CACHE_MSYNC_FLAG_DIR_C2M` in `endOffscreen()`, `endOffscreenRows()`, and `syncCache()`, flushing dirty cache lines to physical RAM before scanout. Pre-sampled RTC over I2C in `renderSystem()` and wrapped diagnostic updates in offscreen staging.
    - **WiFlash Signal Catalogs Imported (`b832bcf`)**: Added 66 vendor-neutral profile JSONs to `profiles/` covering Toyota P34/P5 (1,962 signals across 61 files), Ford MG1 (104 signals across 3 files), and Subaru BRZ / GR86 (83 signals across 2 files), plus converter tool `tools/wiflash_to_dashview.py`.
- Build: `pio run` SUCCESS (0 warnings) — RAM 37.6% (123 KB), Flash 23.0% (1.50 MB / 6.5 MB). Native tests 46/46 passed. Hardware running on `/dev/ttyACM0`.

## IN PROGRESS

- LVGL-S2 (Issue #11): Porting Dashboard + Custom Dash screens to native LVGL widgets in `src/ui.cpp`.

## TODO / OPEN WORK

1. **Custom Dash gauges are still a hardcoded table.** `src/custom_dash.inl`
   `cdGetValue()` maps uppercase names ("RPM","SPEED",...) to `vehicleData`.
   **DONE:** drive gauges from `getSignalCount()`/`getSignalByIndex()` so any
   profile's signals appear without firmware changes. Gauge↔signal key naming
   needs a mapping convention (profile keys are lowercase snake_case).
   `cdAppendQueries()` now polls profile signals (if they are `obd_poll` kind).
2. ~~`isListenOnly()` unused~~ DONE: OBD polling gated by `obdTxCleared()` TX
   failsafe — polls only go out when the profile allows TX, at least one
   gauge/logger actually requires a PID (necessity gate in
   sendToyotaObdQueries), and the TWAI
   controller is error-active with TEC/REC <= 8; any ERR_PASS alert, bus-off,
   or elevated counter inhibits polling for a 5 s cooldown that re-arms only
   after a clean window (serial: "[CAN-TX] SAFETY..." / "...resumed").
   `isListenOnly()` remains as a per-profile kill-switch. NOTE: `listen_only`
   semantics are "no TX ever" — a profile with `obd_poll` signals must set it
   `false` (Tacoma profile was corrected from true to false; kclv/knockfb +
   the whole J1979 baseline need polling).
3. **`isCanFd()` / `getArbBitrate()` unused.** TWAI init is hardcoded 500 k
   classic. CAN-FD SKU needs the MCP2518FD path; bitrate should come from profile.
4. **Speed scale unverified.** 0x0B4 decode (both legacy and profile JSON) uses
   0.00621371 — flagged `_calibration_warning` in the profile; calibrate against
   a known-speed log. Toyota spec likely 0.05625/0.0625 km/h per bit.
5. **Gear + TCC single-field limits.** Profile can't express gear's two-byte
   logic (lever + data[2]&0x0F) or TCC's OR-of-two-bits; legacy decode covers it.
   v2 schema needs an `expr`/codec hook or multi-field enums (SCHEMA.md notes).
6. **First-boot profile wizard** using profile `match{}` metadata (make/model/
   year) — currently selection is manual via Settings.
7. **Profile JSON size**: `pf.readString()` loads the whole file into a String
   (~5 KB typical, fine) — revisit if profiles grow past ~64 KB free heap.
8. ~~Datalog CSV columns still tied to legacy PID list~~ DONE: datalogger is
   fully profile-driven. CSV columns = selected profile signals in profile
   order (enum signals write text, values format via SignalMeta decimals,
   cells blank past a 5 s staleness gate — longer than the display's 1.5 s
   because the poller rotates ~11 queries at 4/s). Poll set derives from the
   selection via `profileSignalPollId()`. Selection persists by signal KEY in
   NVS (`dl_sel` + `dl_prof` + `dl_set`); a selection saved under a foreign
   profile that matches nothing falls back to log-everything, while an
   explicit empty pick (NONE) is honored. Picker pages the live signal table
   (21/page). `availablePids[]` now only backs Custom Dash *legacy* gauges.

## Conventions & gotchas

- Touch handling: `handleTouch()` branches per screen on raw `touchLastX/Y`;
  every render-card rect MUST have a matching touch y-range (see Settings cards).
- `preferences` is the global Preferences instance; namespace "dashview"; keys:
  `flip180`, `bl_on`, `prof`, per-prefix log counters.
- NVS writes need `preferences.begin("dashview", false)` … `.end()`; reads `true`.
- Datalog selection is by profile signal KEY (`dl_sel` CSV string, `dl_set`
  flag, `dl_prof` provenance) — never by signal index; indices shift when the
  profile hot-swaps.
- UI style: `C_CARD_BG/CARD_BORDER/CARD_INNER/C_TRD_RED/C_TEXT_MUTED`, Font2
  labels + Font4 buttons, 800x480, navbar at bottom 40 px, header top 44 px.
- Legacy decode paths are intentionally kept during migration; profile wins by
  sync ordering. Delete legacy only after profile parity is bench-verified.
- ESP32 SdFat: `entry.name()` returns full path ("/profiles/foo.json").
- Signal freshness check: `signalAge(key, millis()) < 1500`.

## Build / test & Bench Flashing

```bash
source /opt/data/pio-venv/bin/activate
cd /opt/data/dashview && pio run          # SUCCESS = deployable
bash tests/native/run.sh                  # profile-engine unit tests (no HW)

# Direct hardware access on Zimaboard 2:
pio run -e waveshare-touch-43b -t upload  # build and flash directly to /dev/ttyACM0
pio device monitor -p /dev/ttyACM0 -b 115200 # live serial monitor
/opt/data/pio-venv/bin/esptool.py --port /dev/ttyACM0 chip_id # check device status
```
Verification = native tests green + clean build + bench flash via local /dev/ttyACM0. SD card: FAT32 via
`format_sd.sh`, logs at root, profiles in `/profiles/`.
