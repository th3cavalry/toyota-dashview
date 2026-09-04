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
- [x] Waveshare 4.3B migration (PR#2 lineage): ISO-TP, bus-off recovery, NVS
  file counter, CH422G expander, GT911 touch, PCF85063 RTC, TRD UI.
- Build: `pio run` SUCCESS — RAM 19.6% (64 KB), Flash 35.8% (1.12 MB / 3 MB).
  Toolchain: `/opt/data/pio-venv` (has pip; uv venvs don't).

## IN PROGRESS

- (none in code) Hardware validation on the bench is the active work: flash the
  build, verify profile picker touch geometry (cells y=272-314, x=34/222/410/598),
  and confirm hot-swap on a live bus.

## TODO / OPEN WORK

1. **Custom Dash gauges are still a hardcoded table.** `src/custom_dash.inl`
   `cdGetValue()` maps uppercase names ("RPM","SPEED",...) to `vehicleData`.
   Next phase: drive gauges from `getSignalCount()`/`getSignalByIndex()` so any
   profile's signals appear without firmware changes. Gauge↔signal key naming
   needs a mapping convention (profile keys are lowercase snake_case).
2. ~~`isListenOnly()` unused~~ DONE: OBD polling in loop() is gated on
   `!isListenOnly()`. NOTE: `listen_only` semantics are "no TX ever" — a profile
   with `obd_poll` signals must set it `false` (Tacoma profile was corrected
   from true to false; kclv/knockfb + the whole J1979 baseline need polling).
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
8. Datalog CSV columns still tied to legacy PID list, not profile signals.

## Conventions & gotchas

- Touch handling: `handleTouch()` branches per screen on raw `touchLastX/Y`;
  every render-card rect MUST have a matching touch y-range (see Settings cards).
- `preferences` is the global Preferences instance; namespace "dashview"; keys:
  `flip180`, `bl_on`, `prof`, per-prefix log counters.
- NVS writes need `preferences.begin("dashview", false)` … `.end()`; reads `true`.
- UI style: `C_CARD_BG/CARD_BORDER/CARD_INNER/C_TRD_RED/C_TEXT_MUTED`, Font2
  labels + Font4 buttons, 800x480, navbar at bottom 40 px, header top 44 px.
- Legacy decode paths are intentionally kept during migration; profile wins by
  sync ordering. Delete legacy only after profile parity is bench-verified.
- ESP32 SdFat: `entry.name()` returns full path ("/profiles/foo.json").
- Signal freshness check: `signalAge(key, millis()) < 1500`.

## Build / test

```bash
source /opt/data/pio-venv/bin/activate
cd /opt/data/dashview && pio run          # SUCCESS = deployable
pio run -t upload -t monitor              # USB CDC (ARDUINO_USB_CDC_ON_BOOT=1)
```
No unit tests; verification = clean build + bench flash. SD card: FAT32 via
`format_sd.sh`, logs at root, profiles in `/profiles/`.
