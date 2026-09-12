// =========================================================================
// LVGL 9 UI shell (S2+): the custom UI as real widgets. The 7 pages live in
// one horizontal scroll-snap strip; header + nav bars are widgets, the custom
// dash is real LVGL objects. The headless engine keeps its API surface and
// reads/writes the accessors below; telemetry lands via syncProfileSignals().
//
// Engine -> UI wiring points (main.cpp): uiInit() once at boot; uiUpdate()
// after syncProfileSignals() at the 30 FPS cadence; nextScreen()/prevScreen()
// replace the monolith nav; uiScreenChanged(s) on every page change.
// =========================================================================
#pragma once
#include <Arduino.h>
#include <lvgl.h>

// ---- Live telemetry the engine feeds every loop ----
struct TacomaTelemetry {
    char gear[4] = "P";         // P, R, N, 1..6
    bool tccLocked = false;     // Torque Converter Lockup (TCC)
    int rpm = 0;
    int speedMph = 0;
    float commandedAfr = 14.7f; // Commanded / Commanded AFR (lambda ref)
    float actualAfr = 14.7f;    // closed-loop wideband lambda
    float kclv = 20.0f;         // learned Knock Correct Learn Value
    float knockFB = 0.0f;       // Knock Feedback (deg)
    int throttlePct = 0;        // throttle %
    int engineLoadPct = 0;      // computed engine load %
    int coolantTempC = 88;      // coolant temp C
    int iatC = 25;              // intake air temp C
    float mafGps = 0.0f;        // MAF g/s
    float timingDeg = 10.0f;    // ignition timing advance deg
};
extern TacomaTelemetry vehicleData;

// ---- Shared UI state the engine writes through ----
enum DisplayScreen {
    SCREEN_DASHBOARD = 0,
    SCREEN_CUSTOM    = 1,  // fully user-customizable gauge dash
    SCREEN_SNIFFER   = 2,
    SCREEN_LOGGER    = 3, // dedicated datalog + CAN logger control page
    SCREEN_WIFI      = 4,
    SCREEN_SYSTEM    = 5,
    SCREEN_SETTINGS  = 6, // settings + 180-deg flip page
    SCREEN_COUNT     = 7
};
extern DisplayScreen currentScreen;

extern bool isDisplayFlipped;   // persistent: false = normal, true: 180 flip
extern bool backlightEnabled;   // persistent: CH422G backlight line
extern bool isPidConfigOpen;         // PID picker modal open
extern bool isRawSnifferModalOpen;   // raw packet terminal modal open

// ---- TRD palette (16-bit 565) ----
#define C_DARK_BG     0x0861   // #0a0e1a-ish dark bg
#define C_CARD_BG     0x18E3  // card fill
#define C_CARD_INNER  0x2124  // card inner rect (darker card)
#define C_CARD_BORDER 0x4224  // card outline gray-blue
#define C_TRD_RED     0x8A20  // TRD red accent
#define C_TRD_ORANGE  0xFD20
#define C_TRD_BURGUNDY 0x5A11
#define C_TEXT_WHITE  0xFFFF
#define C_TEXT_MUTED  0x8C71  // gray labels
#define C_TEXT_CYAN   0x053F
#define C_GREEN_OK    0x4E68
#define C_GOLD_LOCK   0xFD20
#define TFT_BLACK     0x0000
extern const uint16_t NAV_BG, NAV_DOT, CARD_BORDER_C;

// ---- Engine -> UI ----
void uiInit();                          // build the widget tree once (after displayInit)
void uiUpdate();                        // push fresh telemetry into widgets
void uiNextScreen();                    // next page (wraps)
void uiPrevScreen();                    // prev page, wraps
void uiScreenChanged(DisplayScreen s);  // called on tab change

// ---- Profile engine (unchanged headless engine; profile.cpp) ----
int  getSignalCount();

// ---- Custom-dash module (LVGL port of the monolith module) ----
void cdInit();         // load prefs + build the page-2 tab page once
void cdUpdate();       // refresh gauge widgets from engine accessors
void cdAppendQueries(uint8_t modes[], uint8_t pids[], int& count, int cap);

// ---- Splash ----
void splashInit();       // TRD logo splash as LVGL overlay
void splashDismiss();  // tap-to-dismiss -> dashboard

// ---- Touch: legacy gesture engine (display.cpp GT911 read cb) ----
bool pollTouch(int& x, int& y);   // GT911 -> LVGL indev read cb
void handleTouch();                     // monolith gesture engine -> nextScreen/prevScreen
