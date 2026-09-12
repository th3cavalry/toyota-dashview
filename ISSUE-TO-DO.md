# Issue Tracking: Touch Screen Functionality [RESOLVED]

## Problem Statement
The TRD logo displays correctly but when moving to the next screen, the touch doesn't work properly. The screen also tweaks/side to side in different parts.

## Root Cause Analysis & Resolution
1. **Screen Tweaking / Side-to-Side Shearing**:
   - PCLK was originally running at 16 MHz with insufficient porches, causing GDMA FIFO starvation on heavy bus load. Locked `pclk_hz = 14000000` with standard ST7262 porches and `pclk_active_neg = 1`.
   - Full frame repaints during active scanout were eliminated by switching to dirty rect caching and offscreen staging buffers (`_staging_fb`).
2. **Touch Sluggishness & Missed Gestures**:
   - `SWIPE_MIN_DIST_PX` reduced from 400px to 120px for natural finger swipes.
   - Tap hold duration filter relaxed from 600ms to 1200ms.
   - Bottom navbar hit-test boxes mapped directly for `< PREV` (x <= 220), `NEXT >` (x >= 580), and page dots (x = 328..472, y >= 425), with immediate frame trigger on release.
3. **Missing Scanlines / Cut-off Text**:
   - ESP32-S3 DCache writeback synchronization added via `esp_cache_msync()` with `ESP_CACHE_MSYNC_FLAG_DIR_C2M`.

## Acceptance Criteria Status
- [x] Touch functionality works correctly on all screens
- [x] Profile picker touch areas are properly mapped and functional
- [x] No screen tweaking or side-to-side movement during touch operations
- [x] All UI elements respond to touch as expected

## Tasks Completed
- [x] Review the current touch handling implementation
- [x] Verify the touch coordinate mappings for all screens
- [x] Test and validate the profile picker touch areas
- [x] Implement any necessary fixes
- [x] Document the solution (verified on bare-metal Waveshare 4.3B LCD)