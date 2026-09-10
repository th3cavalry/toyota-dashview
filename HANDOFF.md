# HANDOFF — bench session on bare-metal FlowZ13 (4.3B directly attached)

Write this file's sibling knowledge lives in `AGENTS.md` (project state) and
`BENCH.md` (bring-up checklist). This file = bridge from the container session.

Last updated: 2026-09-09, branch `feat/lvgl-port` (commit `d7f1b10`).

## Display Architecture & Hardware Status (2026-09-09 Update)

- **Display Panel & Backlight Fixed**: The RGB LCD (800x480) on the Waveshare 4.3B is now driven via `esp_lcd_new_rgb_panel` with a direct PSRAM frame buffer (768 KB) using `num_fbs = 1`, `flags.fb_in_psram = 1`, and `bounce_buffer_size_px = 0`.
- **Cache Panic Eliminated**: Because `bounce_buffer_size_px` is 0, GDMA streams directly from Octal PSRAM using hardware bus-master DMA with NO CPU interrupt. SPI flash operations (Wi-Fi, NVS, SD) disabling the CPU DCache no longer trigger `Guru Meditation Error: Core 1 panic (Cache disabled but cached memory region accessed)`.
- **DO NOT reintroduce bounce buffers or `no_fb` mode**: `no_fb` mode breaks `get_frame_buffer()`, and bounce buffers re-introduce the cache-disabled ISR panic.
- **Boot Splash & Auto-Dismiss**: Boot splash renders "DashView" wordmark and auto-dismisses after 3 seconds into the Main Dashboard (Page 0), or immediately upon screen tap.
- **Display Flicker Root Causes & Fixes**:
  1. *Unbuffered Full-Screen Wipe*: `updateDisplay()` was calling `canvas.fillScreen(C_DARK_BG)` every 16ms (60 FPS) on the active scanout buffer. Wiping and redrawing unbuffered into the active buffer causes severe tearing and strobing. Redraw only dirty gauge rects/labels in place, or switch to double-buffering (`num_fbs = 2`).
  2. *Low Refresh Rate (~33 Hz)*: 14 MHz `pclk_hz` with total clock count 423,120 yields only 33 Hz. Increasing `pclk_hz` (e.g., 16 MHz) with tuned porches (pulse=10, back=10, front=10) brings refresh rate to 50-60 Hz without bus starvation.
- **Touch**: GT911 at `0x5D` on shared I2C bus is wired to `displayTouchRead()` and `pollTouch()`.
- **Build & Flash**: Working cleanly via PlatformIO in the container environment.


## Device facts (verified 2026-09-05)

- Board: Waveshare ESP32-S3-Touch-LCD-4.3B, factory firmware NOT flashed —
  screen dark until we flash is NORMAL (backlight only lights once our FW runs).
- Native USB port on the board = the one wired to the ESP32-S3 (labeled
  COM/UART on silkscreen); it enumerates as `303A:1001` (USB-CDC, no bridge chip).
  The other USB-C is not the flashing port.
- Firmware uses `ARDUINO_USB_CDC_ON_BOOT=1` → the SAME port is both flash
  and monitor at 115200. No CP210x driver needed on Linux.
- Bootloader force: hold BOOT, tap RST, release BOOT → still `303A:1001`.
- If no /dev/ttyACM0 appears on a systemd box with a populated /dev: check
  `dmesg | tail`. (In the container we had to `mknod /dev/ttyACM0 c 166 0` —
  should NOT be needed bare-metal; if it is, udev is masked, fix that instead.)

## Flash + monitor (this machine)

```bash
git clone https://github.com/th3cavalry/toyota-dashview.git && cd toyota-dashview
git checkout feat/4.3b-migration
pip install -U platformio          # or pipx/venv
sudo usermod -aG dialout $USER     # then re-login (one-off: sudo chmod 666 /dev/ttyACM0)
pio run -e waveshare-touch-43b -t upload -t monitor
```
Expected first boot serial (115200):
```
=== Toyota DashView ... (ESP32-S3 Touch LCD 4.3B) ===
[PROFILE] Profile active: Toyota Tacoma (3rd Gen)
[CDASH] Loaded N gauges
[CAN] TWAI init OK
```
If upload can't sync: board is running app firmware that hogs USB-CDC — use
BOOT+RST, or `pio run -t upload --upload-port /dev/ttyACM0` after reset.

## Work order for this session (BENCH.md has the full checklist)

1. **Flash + boot smoke** (BENCH §1). Dark screen after flash = backlight/CH422G
   issue → check serial for `CH422G` init lines; likely EXIO mapping.
2. **SD card**: `sudo ./format_sd.sh /dev/sdX`, copy `profiles/*.json` →
   `/profiles/` on the card. Verify Settings picker + hot-swap (BENCH §3).
3. **Vehicle/bench CAN**: OBD-II plug pin 6 = CAN-H, 14 = CAN-L, ground.
   Ignition ON → raw sniffer must show frames. Watch for `[CAN-TX] SAFETY`
   spam (should be absent at idle).
4. **New UI surfaces needing touch-geometry verification** (never had a real
   panel; these are the highest-probability first bugs, all in PR #4):
   - Custom Dash ADD picker paged grid (signal list, PREV/NEXT) — `custom_dash.inl`
   - Datalog signal picker (21/page, `<> ALL NONE DONE`) — `main.cpp` renderPidSelector/handleTouch
   - Logger main screen tag list truncation with 15+ signals
   If a tap misses: render rect and hit-test rect must match — grep the coords.
5. **Speed calibration (issue #1 TODO)**: drive log `speed` vs phone GPS, then
   fix `scale` in `profiles/toyota_tacoma_2016_2023.json` (0x0B4, currently
   0.00621371, flagged `_calibration_warning`).
6. After bench pass: merge PR #4, then TODO #6 (first-boot profile wizard) or
   #5 (v2 schema expr hooks) per AGENTS.md.

## Known open bugs/limits (don't re-discover these)

- Gear gauge: profile JSON can only do P/R/N (single byte); real 1-6 logic is
  in the legacy decoder and wins by sync ordering. Fine for now.
- CSV logger staleness gate is 5 s (poller rotation), display is 1.5 s, custom
  dash is 2 s — deliberate, documented in main.cpp comments.
- `pio` toolchains in container live at /opt/data/.platformio; bare-metal will
  re-download (~few min, needs network).
- ESP32 SdFat quirk: `entry.name()` returns FULL path, not basename.

## Commit hygiene (user's standing rules)

- Update AGENTS.md in the SAME commit as the work it describes.
- Conventional commits. Push branch; PR #4 already open — add commits to it.
- Verification before claiming done: native tests + `pio run` + (now) bench.
