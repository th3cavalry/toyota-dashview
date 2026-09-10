## LVGL 9 migration — replaces the LovyanGFX canvas UI

**Why:** the flicker is structural in LGFX's RGB path — `Bus_RGB::init()` allocates ONE PSRAM framebuffer and GDMA scans it straight from Octal PSRAM (`use_psram` is dead code through 1.2.28). Canvas writes into the same PSRAM GDMA is streaming -> shear/flicker ~ content-change rate. LGFX will never fix this here.

**The fix:** `esp_lcd_panel_rgb` + LVGL 9 — SRAM bounce buffers (scanout stops contending with CPU PSRAM writes), DMA-2D, and dirty-rect rendering instead of full-frame canvas repaints.

### Progress
- **S1 done + bench-verified** (01ea707): LVGL 9.3 in the build, esp_lcd RGB + ST7701 init, single PSRAM FB in DIRECT mode, GT911 indev (11a218d track-register fix kept), CH422G backlight, flip180. 14 MHz pixel clock is the hard constraint on this panel — do not raise. Splash renders, 60 Hz steady, zero flicker under Wi-Fi AP + CAN load.
- **Profile engine landed** (0782bca): OBD addressing via `getReqId()/getRespId()`, `syncProfileSignals()` -> telemetry, NVS `dashview/prof` + `/profiles/<id>.json` load at boot, SD picker scan. Adding a vehicle = JSON on the SD card, no rebuild.
- **S2 in progress:** `src/ui.h` scaffold merged; `ui.cpp` (widget tree) lands next.

### Plan (one PR per section onto this branch)
S2 dashboard+custom-dash · S3 sniffer/logger/settings/system · S4 OOBE+OTA (PR#6 port) · S5 simulator+native tests (>=46/46) · S6 cutover: delete LGFX+canvas, v0.5.0.

### Bench gate (every merge, BENCH.md)
Splash -> tap -> steady; 6 screens swiped; no shear at Wi-Fi AP + CAN idle; profile hot-swap without reboot; SD pull -> `--` <=2 s; reinsert -> rescan.
