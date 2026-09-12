# DashView Project Collaboration Plan

## Current Status

The DashView project is on branch `feat/lvgl-port` (tracking PR #8). The device is connected to FlowZ13 via `/dev/ttyACM0` and running live firmware.

### Recent Bench Fixes Completed (2026-09-12):
1. **Touch Navigation & Hitboxes**: Resolved. Swipe threshold adjusted to 120px, bottom navbar hit-test boxes mapped (`< PREV`, `NEXT >`, and dots), tap release detection instant.
2. **Display Stability & Timing**:
   - PCLK phase set to falling edge (`pclk_active_neg = 1`) and sync polarities aligned (`hsync_idle_low = 0`, `vsync_idle_low = 0`), eliminating letter shimmering and color fringing.
   - Fixed raw CAN sniffer page tearing and horizontal jitter under heavy CAN traffic.
3. **CPU DCache Write-Back Synchronization (`8dc426b`)**:
   - Resolved the issue where glyph scanlines were missing or cut off on the physical screen.
   - Added `esp_cache_msync()` with `ESP_CACHE_MSYNC_FLAG_DIR_C2M` across offscreen staging and steady-state dirty flushes so CPU writes to PSRAM commit before GDMA direct scanout.
4. **Vehicle Profiles**: WiFlash signal catalogs imported (`b832bcf`), providing 66 profile JSONs in `profiles/`.

## Roles and Responsibilities

### FlowZ13 (Primary Coder / Hardware Operator)
- Direct access to physical Waveshare 4.3B hardware at `/dev/ttyACM0`.
- Responsible for flashing, capturing serial/screenshots, and verifying LCD behavior.

### Hermes Agent
- Architecture, non-hardware features, profile expansion, code review, and widget design.

## Active Milestone: LVGL-S2 (Issue #11)
- Port `renderDashboard()` (Page 0) and `custom_dash.inl` (Page 1) to native LVGL widgets in `src/ui.cpp`.
- Gauge data driven by profile accessors (`getSignalCount()`, `getSignalByIndex()`).
- Maintain 2s staleness gate (`--`) and profile hot-swapping.