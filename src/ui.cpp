// =========================================================================
// LVGL 9 UI shell (S2): the UI globals the headless engine reads/writes.
// The monolith renderers still paint through the canvas shim; the widget
// strip arrives next. Globals live here — main.cpp keeps only definitions.
// =========================================================================
#include "ui.h"
#include "display.h"
#include "fonts.h"

// ---- Engine <-> UI contract (shared globals; externs in ui.h) -------------
TacomaTelemetry vehicleData;
DisplayScreen currentScreen = SCREEN_DASHBOARD;
bool isDisplayFlipped = false;
bool backlightEnabled = true;

// Shared palette (16-bit 565)
const uint16_t NAV_BG        = 0x0861;   // nav bar bg
const uint16_t NAV_DOT       = 0x8C71;
const uint16_t CARD_BORDER_C = 0x4224;

// UI-only state the engine reads to gate navigation
bool isPidConfigOpen = false;
bool isRawSnifferModalOpen = false;

// GT911 -> LVGL indev read cb (display.cpp owns the GT911)
bool pollTouch(int& x, int& y) { return displayTouchRead(x, y); }
