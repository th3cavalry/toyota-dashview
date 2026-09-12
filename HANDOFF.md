# HANDOFF — bench session on bare-metal FlowZ13 (4.3B directly attached)

Write this file's sibling knowledge lives in `AGENTS.md` (project state) and
`BENCH.md` (bring-up checklist). This file = bridge from the container session.

Last updated: 2026-09-12, branch `feat/lvgl-port` (commit `b832bcf`).

## Display Architecture & Hardware Status (2026-09-12 Update)

- **Display Panel & Direct Scanout**: The RGB LCD (800x480, ST7262) on the Waveshare 4.3B is driven via `esp_lcd_new_rgb_panel` with a direct PSRAM frame buffer (768 KB) using `num_fbs = 1`, `flags.fb_in_psram = 1`, `dma_burst_size = 64`, and `bounce_buffer_size_px = 0`.
- **Pixel Clock Locked at 14 MHz**: `pclk_hz = 14000000`. **CRITICAL RULE**: Do NOT increase `pclk_hz` to 16 MHz. Under active Wi-Fi AP and CAN traffic, 16 MHz pulls pixels faster than GDMA can refill the internal FIFO over the Octal PSRAM bus, starving the scanout beam and shearing lines horizontally ("screen shaking side-to-side"). 14 MHz with standard ST7262 porches is rock solid.
- **ST7262 PCLK Phase & Timing Flags Fixed (`9c39d25`)**: Set `pclk_active_neg = 1` and polarities `hsync_idle_low = 0`, `vsync_idle_low = 0`, `de_idle_high = 0`. The ST7262 panel samples data on the falling clock edge; this eliminated subtle letter shimmering and color fringing.
- **CPU DCache Write-Back Synchronization (`8dc426b`)**: On ESP32-S3, PSRAM writes pass through a 32 KB write-back L1 DCache. Direct GDMA scanout (`bounce_buffer_size_px = 0`) reads physical external PSRAM directly via DMA/AXI, completely bypassing the CPU cache. Without cache sync, dirty cache lines remain in the CPU cache while GDMA scans out stale RAM, causing missing horizontal scanlines ("text cut off at top and middle") and "unpacking" delays. Added `esp_cache_msync()` with `ESP_CACHE_MSYNC_FLAG_DIR_C2M` in `endOffscreen()`, `endOffscreenRows()`, and steady-state `syncCache()`. Hardware text is now 100% crisp and solid.
- **Cache Panic Eliminated**: Because `bounce_buffer_size_px` is 0, GDMA streams directly from Octal PSRAM using hardware bus-master DMA with NO CPU interrupt. SPI flash operations (Wi-Fi, NVS, SD) disabling the CPU DCache no longer trigger cache disabled ISR panics.
- **DO NOT reintroduce bounce buffers or `no_fb` mode**: `no_fb` mode breaks `get_frame_buffer()`, and bounce buffers re-introduce the cache-disabled ISR panic.
- **Flicker-Free Dirty-Rect Rendering & Offscreen Staging**: In single-buffer direct PSRAM scanout, the beam scans the buffer continuously at ~33 Hz. Page switches clear only the content area (0..440), leaving bottom navbar untouched. Steady-state updates use minimal dirty box updates or off-screen staging (`beginOffscreen()` / `endOffscreen()`).
- **Touch Navigation Verified**:
  - `SWIPE_MIN_DIST_PX` set to 120px for natural gestures.
  - Bottom navigation bar has direct hit tests for `< PREV` (x <= 220), `NEXT >` (x >= 580), and page dots (x = 328..472, y >= 425). Tap detection filter relaxed to 1200ms.
- **Vehicle Profiles Library**: 66 vendor-neutral profiles imported into `profiles/` from WiFlash catalogs (Toyota P34/P5, Ford MG1, Subaru BRZ/GR86).
- **Build & Flash**: Verified building cleanly with PlatformIO and flashing to physical hardware via `/dev/ttyACM0`. Live serial boots cleanly with all peripherals active. Native tests 46/46 green.


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
4. **Active Roadmap Milestone — LVGL-S2 (Issue #11)**:
   - Port `renderDashboard()` (Page 0) and `custom_dash.inl` (Page 1) to native LVGL widgets in `src/ui.cpp`.
   - Wire 6 gauges to profile accessors (`getSignalCount()`, `getSignalByIndex()`, etc.).
   - Custom Dash dynamic add/drag/resize/styles + edit mode.
   - Maintain 2-second stale signal blanking (`--`).
5. **Subsequent Milestones**:
   - S3 (Issue #12): Sniffer / Logger / Settings / System screens in LVGL.
   - S4 (Issue #13): OOBE wizard + OTA updater.
   - S5 (Issue #14): Telemetry simulator + native tests.
   - S6 (Issue #15): Cutover & LGFX removal, bump to v0.5.0, merge PR #8.

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
