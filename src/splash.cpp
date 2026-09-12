// Boot splash — "DashView" wordmark, drawn straight into the PSRAM framebuffer.
//
// S1: the old TRD logo PNG is gone (with it, the whole LVGL image-decoder path).
// A plain fill + text render into the FB is what the splash needs: zero decoder
// state, zero 768 KB decode buffers, nothing that can trip the PSRAM/cache panic.
// The canvas shim already paints through the DCache, so the GDMA bounce scan-out
// sees every pixel. Must be called after canvas.init() so the FB exists.

#include "splash.h"

bool drawToyotaBootSplash(LVGLCanvas& canvas) {
    // Deep Jet Black Background
    canvas.fillScreen(canvas.color565(10, 12, 16));

    // TRD Motorsport Heritage Tri-Color Banner (Top Bar)
    canvas.fillRect(0, 0, 266, 6, canvas.color565(245, 130, 32)); // TRD Orange
    canvas.fillRect(266, 0, 268, 6, canvas.color565(235, 10, 30));  // TRD Red
    canvas.fillRect(534, 0, 266, 6, canvas.color565(150, 15, 25)); // TRD Burgundy

    // Center TRD Heritage Tri-Color Accent Pill
    int badgeY = 85;
    canvas.fillRoundRect(345, badgeY, 30, 8, 4, canvas.color565(245, 130, 32)); // Orange
    canvas.fillRoundRect(385, badgeY, 30, 8, 4, canvas.color565(235, 10, 30));  // Red
    canvas.fillRoundRect(425, badgeY, 30, 8, 4, canvas.color565(150, 15, 25)); // Burgundy

    // Main Title: Bold 48px "DASHVIEW"
    canvas.setFont(fonts::Font7);                       // Montserrat 48
    canvas.setTextColor(canvas.color565(255, 255, 255));
    canvas.drawCenterString("DASHVIEW", 400, 120);

    // Subtitle in TRD Orange
    canvas.setFont(fonts::Font4);                       // Montserrat 20
    canvas.setTextColor(canvas.color565(245, 130, 32)); // Heritage Orange
    canvas.drawCenterString("TOYOTA MOTORSPORT TELEMETRY", 400, 195);

    // Active Profile Box
    int boxW = 380;
    int boxH = 54;
    int boxX = 400 - boxW / 2;
    int boxY = 245;
    canvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, canvas.color565(18, 22, 30));
    canvas.drawRoundRect(boxX, boxY, boxW, boxH, 8, canvas.color565(40, 48, 65));
    canvas.fillRect(boxX + 2, boxY + 2, 6, boxH - 4, canvas.color565(40, 220, 100));

    canvas.setFont(fonts::Font2);                       // Montserrat 14
    canvas.setTextColor(canvas.color565(130, 140, 160));
    canvas.drawCenterString("SYSTEM ACTIVE", 400, boxY + 8);
    canvas.setTextColor(canvas.color565(40, 220, 100)); // Nominal Green
    canvas.drawCenterString("TACOMA 3RD GEN TELEMETRY", 400, boxY + 30);

    // "TAP TO START" prompt
    canvas.setFont(fonts::Font2);
    canvas.setTextColor(canvas.color565(0, 220, 255));   // Ice Cyan
    canvas.drawCenterString("< TAP ANYWHERE TO START >", 400, 360);

    // Version Footer
    canvas.setFont(fonts::Font0);                       // Montserrat 8
    canvas.setTextColor(canvas.color565(90, 100, 120));
    canvas.drawCenterString("DashView (ESP32-S3 Touch LCD 4.3B 800x480)", 400, 440);

    canvas.setTextDatum(0); // Reset to standard top-left datum
    canvas.markDirty();
    return true;
}
