// TRD boot splash — decodes the embedded PNG into the PSRAM framebuffer at boot.
// Uses LVGL 9's image decoder (lodepng backend registered by lv_lodepng_init).
//
// S1: draw once to the FB before the first lv_timer_handler run; the canvas
// fillScreen(0) fallback covers a decode failure.

#pragma once
#include <lvgl.h>

// Decode trd_splash_png (PROGMEM, in toyota_splash.h) into the canvas framebuffer.
// Returns true on success. Must be called after canvas.init() so the FB exists.
#include "display.h"
bool drawToyotaBootSplash(LVGLCanvas& canvas);
