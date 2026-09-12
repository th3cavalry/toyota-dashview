// Boot splash — "DashView" wordmark rendered into the PSRAM framebuffer at boot.
//
// S1: plain canvas drawing (fill + Montserrat text) — no PNG decode, no LVGL
// image decoder. The canvas shim paints through the DCache so the GDMA bounce
// scan-out picks the pixels up without any extra push.

#pragma once
#include <lvgl.h>

// Render the splash into the canvas framebuffer. Always succeeds.
// Must be called after canvas.init() so the FB exists.
#include "display.h"
bool drawToyotaBootSplash(LVGLCanvas& canvas);
