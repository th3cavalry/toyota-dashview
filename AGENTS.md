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
- [x] **Hardware Flash & Boot Verified (2026-09-05)**: Successfully flashed to
  physical Waveshare 4.3B hardware via `/dev/ttyACM0`. Verified live boot log:
  CH422G IO expander OK, 800x480 PSRAM framebuffer OK, GT911 touch OK, PCF85063
  RTC OK, TWAI CAN initialized (TX:15, RX:16 @ 500k), Wi-Fi AP + SavvyCAN server live,
  Profile engine active (`toyota_tacoma_2016_2023`).
- Build: `pio run` SUCCESS — RAM 20.2% (66 KB), Flash 35.9% (1.13 MB / 3 MB).

## IN PROGRESS

- (none in code) Hardware validation on the bench is the active work: flash the
  build, verify profile picker touch geometry (cells y=272-314, x=34/222/410/598),
  and confirm hot-swap on a live bus.

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
