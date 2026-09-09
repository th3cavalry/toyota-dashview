// Boot splash — "DashView" wordmark, drawn straight into the PSRAM framebuffer.
//
// S1: the old TRD logo PNG is gone (with it, the whole LVGL image-decoder path).
// A plain fill + text render into the FB is what the splash needs: zero decoder
// state, zero 768 KB decode buffers, nothing that can trip the PSRAM/cache panic.
// The canvas shim already paints through the DCache, so the GDMA bounce scan-out
// sees every pixel. Must be called after canvas.init() so the FB exists.

#include "splash.h"

bool drawToyotaBootSplash(LVGLCanvas& canvas) {
    canvas.fillScreen(canvas.color565(13, 17, 23));      // C_DARK_BG

    // TRD-red accent bar across the top, 4 px into the header band.
    canvas.fillRect(0, 0, 800, 4, canvas.color565(204, 0, 38));

    canvas.setFont(fonts::Font7);                       // Montserrat 28
    canvas.setTextDatum(4);                             // ML_CENTER
    canvas.setTextColor(canvas.color565(255, 255, 255));
    canvas.drawString("DashView", 400, 240);            // dead center of 800x480

    canvas.setFont(fonts::Font2);                       // Montserrat 14
    canvas.setTextColor(canvas.color565(110, 116, 130));
    canvas.drawString("Toyota Tacoma", 400, 300);       // sub-line

    canvas.markDirty();
    return true;
}
