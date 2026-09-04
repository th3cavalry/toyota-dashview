// =========================================================================
// Custom Dash — user-configurable gauge page (snap-grid drag & drop)
//
// A grid of cells covers the content area below the header. Each gauge
// occupies a rectangle of whole cells; the user drags a gauge's body to
// move it (snapping to free cells) and drags its bottom-right corner to
// resize. Gauges can show a live value as number+unit, a horizontal bar,
// or a vertical bar. Per-gauge warning thresholds drive color, background
// and flash.
//
// Self-contained: defines its own enum values for the screens it checks so
// it never depends on the main.cpp enum's declaration order, and references
// only the globals/structs main.cpp already declares.
// =========================================================================
#pragma once
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <vector>

// ---- Screen ids mirrored from main.cpp DisplayScreen (keep in sync) --------
#define CD_SCREEN_DASHBOARD 0
#define CD_SCREEN_CUSTOM    1

// ---- Grid / layout constants ----------------------------------------------
#define CD_COLS          8
#define CD_ROWS          6
#define CD_GRID_X        12
#define CD_GRID_Y        52
#define CD_GRID_W        776
#define CD_GRID_H        360
#define CD_CELL_W        (CD_GRID_W / CD_COLS)   // 97
#define CD_CELL_H        (CD_GRID_H / CD_ROWS)   // 60
#define CD_CORNER_PX     26                       // resize-handle hit size

#define CD_MAX_GAUGES    12

// ---- Enumerations (stored as uint8_t) -------------------------------------
enum CdStyle    : uint8_t { CD_STYLE_NUMBER = 0, CD_STYLE_HBAR = 1, CD_STYLE_VBAR = 2 };
enum CdWarning  : uint8_t { CD_WARN_OFF = 0, CD_WARN_COLOR = 1, CD_WARN_FLASH = 2 };
enum CdColor    : uint8_t { CD_COL_CYAN = 0, CD_COL_GREEN = 1, CD_COL_ORANGE = 2,
                            CD_COL_RED = 3, CD_COL_WHITE = 4, CD_COL_GOLD = 5 };
enum CdEditor   : uint8_t { CD_EDIT_NONE = 0, CD_EDIT_GAUGE = 1, CD_EDIT_ADD = 2 };

// ---- A single gauge -------------------------------------------------------
struct CdGauge {
    uint8_t  pid;              // index into availablePids[] (0..PID_COUNT-1)
    uint8_t  style;            // CdStyle
    int16_t  x, y, w, h;       // cell coords (x,y = top-left; w,h = size)
    float    minVal, maxVal;   // bar scale + warning reference
    float    warnLo, warnHi;   // warning band (outside => warn); warnHi<=-1e30 => disabled
    uint8_t  warnMode;         // CdWarning
    uint8_t  warnColor;        // CdColor
    uint8_t  decimals;         // 0..2
    bool     valid;            // slot in use
};

// ---- Module state (defined once, in custom_dash.cpp) ----------------------
extern CdGauge         g_cdGauges[CD_MAX_GAUGES];
extern int             g_cdGaugeCount;
extern bool            g_cdEditMode;
extern CdEditor        g_cdEditorKind;
extern int             g_cdEditorIdx;         // gauge index when EDIT_GAUGE
extern int             g_cdDragIdx;           // gauge being dragged (-1 none)
extern int             g_cdDragMode;          // 0 none / 1 move / 2 resize
extern int             g_cdDragOffX, g_cdDragOffY; // finger offset in cells*px

// ---- Persistence ----------------------------------------------------------
void cdLoadPrefs();
void cdSavePrefs();
void cdSeedDefaults();

// ---- Query integration ----------------------------------------------------
// Appends the dash's PIDs (mode,pid) to the active OBD query set so the bus
// actually polls gauges the datalogger selection may not include.
void cdAppendQueries(uint8_t modes[], uint8_t pids[], int& count, int cap);

// ---- Rendering ------------------------------------------------------------
void renderCustomDash();
void renderCustomDashEditor();

// ---- Touch ----------------------------------------------------------------
// Returns true if the gesture was consumed by the dash (suppresses swipe /
// page taps for that release). Called at the top of handleTouch().
bool cdTouchActive();               // true while a drag is in progress
bool cdHandlePress(int x, int y);   // called on press-down
bool cdHandleDrag(int x, int y);    // called on move while dragging
bool cdHandleRelease(int x, int y); // called on release; true = consumed
