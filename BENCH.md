# Bench bring-up — Waveshare ESP32-S3-Touch-LCD-4.3B

Firmware: `.pio/build/waveshare-touch-43b/firmware.bin` (md5 `02cce2b5...`, head `2c8674d` = PR #4 tip)
Serial: 115200, USB-CDC (`ARDUINO_USB_CDC_ON_BOOT=1`) — the port is the ESP32 itself.

## 0. Flash & Monitor Setup
The board is physically connected via USB to FlowZ13 (`192.168.8.20`) and bridged over LAN via the `waveshare-bridge` HTTP service (port 5050).

Commands available inside Hermes:
- **Check device status**: `waveshare-status`
- **Build & Flash via PlatformIO**:
  ```bash
  source /opt/data/pio-venv/bin/activate
  cd /opt/data/dashview
  pio run -e waveshare-touch-43b -t upload
  ```
- **Direct Flash**: `waveshare-flash [path/to/firmware.bin]`
- **Live Serial Stream**: `waveshare-logs -f`
- **Recent Serial Logs**: `waveshare-logs 50`
- **Remote Hard Reset**: `waveshare-reset`

## 1. Boot smoke (no vehicle, just power)
- [ ] Backlight on, TRD splash appears; tap to dismiss
- [ ] Serial shows `[PROFILE] Profile active: Toyota Tacoma (3rd Gen)` (built-in fallback)
- [ ] `[CDASH] Loaded N gauges`, no brown-out/reboot loop (PSRAM OK = `qio_opi` build)
- [ ] Swipe between all 6 screens; no missed swipes at splash→cluster
- [ ] Settings: flip180 toggle, backlight off/on (CH422G EXIO2)

## 2. CAN physical (OBD-II plug: pin 6 CAN-H, pin 14 CAN-L, pin 4/5 ground, ignition ON)
- [ ] Raw sniffer page shows frames flowing (engine off = body/bus still quiet-ish; start engine)
- [ ] Cluster: RPM moves, speed reads 0 at standstill, gear shows P/R/N/D
- [ ] Serial `[CAN] TWAI init OK` at 500k; NO `[CAN-TX] SAFETY` spam at idle

## 3. Profile picker / SD
- [ ] Seed card: `sudo ./format_sd.sh /dev/sdX`, copy `profiles/*.json` into `/profiles/`
- [ ] Settings → profile cells appear; tap = instant hot-swap (no reboot), NVS persists over power cycle
- [ ] Pull SD while running → gauges keep working on built-in; reinsert → re-scan

## 4. Custom Dash (signal-backed gauges)
- [ ] + ADD → page 2 (green cells) lists profile signals: `kclv`, `knockfb`, `afr_actual`...
- [ ] Add `kclv` + `tcc_locked`; drag/resize; styles; warn thresholds flash
- [ ] Kill ignition → gauges blank to `--` within 2 s (stale gate), not frozen values

## 5. Datalogger
- [ ] Configure signals → page ALL/NONE; NONE keeps polls silent
- [ ] Start PID datalog; drive 2 min; CSV: header = selected keys, cells filled, gear text lands
- [ ] `[CAN-TX] SAFETY` test (paranoid pass): briefly disconnect a CAN wire → serial logs pause 5 s, resumed after
- [ ] CAN-raw logger + SavvyCAN stream: connect ONE client (UI says 1 max)

## 6. Drive-log calibration (issue #1 TODO: speed scale)
- [ ] Log `speed` alongside known reference (phone GPS) on the highway; pin the 0x0B4 factor
- [ ] Save the CSV → we update `scale` in `profiles/toyota_tacoma_2016_2023.json`

## Known watch-items
- Gear shows P/R/N only until v2 schema (two-byte logic is legacy-decode; sync ordering keeps it right)
- If backlight/touch dead → CH422G address/exio mapping (README wiring table)
- Watch heap in Diagnostics page after 30 min (profile JSON String load)
