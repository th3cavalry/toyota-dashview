// =========================================================================
// Custom Dash implementation. Textually included from main.cpp AFTER all
// globals (canvas, availablePids, vehicleData, preferences, colors) are
// declared, so it references them directly. Do NOT compile standalone.
// =========================================================================

// ---- Module state definitions ---------------------------------------------
CdGauge  g_cdGauges[CD_MAX_GAUGES];
int      g_cdGaugeCount   = 0;
bool     g_cdEditMode     = false;
CdEditor g_cdEditorKind   = CD_EDIT_NONE;
int      g_cdEditorIdx    = -1;
int      g_cdDragIdx      = -1;
int      g_cdDragMode     = 0;      // 1 move / 2 resize
int      g_cdDragOffX = 0, g_cdDragOffY = 0;

// ---- Per-PID metadata (unit / default scale / decimals) -------------------
struct CdMeta { const char* unit; float mn, mx; uint8_t dec; };
static CdMeta cdMeta(int pidIdx) {
    const char* k = availablePids[pidIdx].idStr;
    if (!strcmp(k, "RPM"))    return { "",       0, 6000, 0 };
    if (!strcmp(k, "SPEED"))  return { "mph",    0, 120,  0 };
    if (!strcmp(k, "THR"))    return { "%",      0, 100,  0 };
    if (!strcmp(k, "LOAD"))   return { "%",      0, 100,  0 };
    if (!strcmp(k, "AFR"))    return { ":1",    10,  20,  1 };
    if (!strcmp(k, "KCLV"))   return { "",       0,  30,  1 };
    if (!strcmp(k, "KFB"))    return { "deg",  -12,  12,  1 };
    if (!strcmp(k, "ECT"))    return { "C",    -40, 150,  0 };
    if (!strcmp(k, "IAT"))    return { "C",    -40,  80,  0 };
    if (!strcmp(k, "MAF"))    return { "g/s",    0, 250,  1 };
    if (!strcmp(k, "TIMING")) return { "deg",  -20,  50,  1 };
    return { "", 0, 100, 0 }; // GEAR handled as text elsewhere
}
static float cdVal(const CdGauge& g) {
    const char* k = availablePids[g.pid].idStr;
    if (!strcmp(k,"RPM"))    return vehicleData.rpm;
    if (!strcmp(k,"SPEED"))  return vehicleData.speedMph;
    if (!strcmp(k,"THR"))    return vehicleData.throttlePct;
    if (!strcmp(k,"LOAD"))   return vehicleData.engineLoadPct;
    if (!strcmp(k,"AFR"))    return vehicleData.actualAfr;
    if (!strcmp(k,"KCLV"))   return vehicleData.kclv;
    if (!strcmp(k,"KFB"))    return vehicleData.knockFB;
    if (!strcmp(k,"ECT"))    return vehicleData.coolantTempC;
    if (!strcmp(k,"IAT"))    return vehicleData.iatC;
    if (!strcmp(k,"MAF"))    return vehicleData.mafGps;
    if (!strcmp(k,"TIMING")) return vehicleData.timingDeg;
    return 0; // GEAR
}
static bool cdIsGear(int pidIdx) { return !strcmp(availablePids[pidIdx].idStr, "GEAR"); }
static uint16_t cdColor(CdColor c) {
    switch (c) {
        case CD_COL_CYAN:   return C_TEXT_CYAN;
        case CD_COL_GREEN:  return C_GREEN_OK;
        case CD_COL_ORANGE: return C_TRD_ORANGE;
        case CD_COL_RED:    return C_TRD_RED;
        case CD_COL_GOLD:   return C_GOLD_LOCK;
        default:            return C_TEXT_WHITE;
    }
}
// warning active = value outside [warnLo, warnHi]; warnHi<=-1e30 => disabled
static bool cdWarnOn(const CdGauge& g) {
    if (g.warnMode == CD_WARN_OFF || g.warnHi <= -1e29f) return false;
    float v = cdVal(g);
    return (v < g.warnLo || v > g.warnHi);
}

// ---- Grid geometry helpers ------------------------------------------------
static void cdCellRect(int cx, int cy, int cw, int ch, int& x, int& y, int& w, int& h) {
    x = CD_GRID_X + cx * CD_CELL_W;
    y = CD_GRID_Y + cy * CD_CELL_H;
    w = cw * CD_CELL_W;
    h = ch * CD_CELL_H;
}
// cell under a pixel, or -1
static void cdPixelToCell(int px, int py, int& cx, int& cy) {
    cx = (px - CD_GRID_X) / CD_CELL_W;
    cy = (py - CD_GRID_Y) / CD_CELL_H;
    if (cx < 0) cx = 0; if (cx >= CD_COLS) cx = CD_COLS - 1;
    if (cy < 0) cy = 0; if (cy >= CD_ROWS) cy = CD_ROWS - 1;
}
// occupancy test: does any OTHER gauge overlap rect (x,y,w,h in cells)?
static bool cdOverlap(int idx, int x, int y, int w, int h) {
    for (int i = 0; i < CD_MAX_GAUGES; i++) {
        if (i == idx || !g_cdGauges[i].valid) continue;
        CdGauge& o = g_cdGauges[i];
        if (x < o.x + o.w && o.x < x + w && y < o.y + o.h && o.y < y + h) return true;
    }
    return false;
}

// ---- Persistence ----------------------------------------------------------
void cdLoadPrefs() {
    preferences.begin("dashview", true);
    int n = preferences.getUChar("cd_n2", 0);
    if (n > CD_MAX_GAUGES) n = 0;
    uint8_t blob[sizeof(CdGauge) * CD_MAX_GAUGES];
    size_t got = preferences.getBytes("cd_g2", blob, sizeof(blob));
    preferences.end();
    g_cdGaugeCount = 0;
    for (int i = 0; i < CD_MAX_GAUGES; i++) g_cdGauges[i].valid = false;
    if (n > 0 && got == sizeof(CdGauge) * (size_t)n) {
        memcpy(g_cdGauges, blob, got);
        g_cdGaugeCount = n;
    }
    Serial.printf("[CDASH] Loaded %d gauges\n", g_cdGaugeCount);
}
void cdSavePrefs() {
    preferences.begin("dashview", false);
    preferences.putUChar("cd_n2", (uint8_t)g_cdGaugeCount);
    preferences.putBytes("cd_g2", g_cdGauges, sizeof(CdGauge) * g_cdGaugeCount);
    preferences.end();
    Serial.printf("[CDASH] Saved %d gauges\n", g_cdGaugeCount);
}
static CdGauge cdDefaultGauge(int pidIdx, int cx, int cy, int cw, int ch) {
    CdGauge g{}; g.valid = true; g.pid = (uint8_t)pidIdx;
    g.style = CD_STYLE_HBAR; g.x = cx; g.y = cy; g.w = cw; g.h = ch;
    CdMeta m = cdMeta(pidIdx);
    g.minVal = m.mn; g.maxVal = m.mx; g.decimals = m.dec;
    g.warnLo = m.mn; g.warnHi = -1e30f;      // warnings off by default
    g.warnMode = CD_WARN_FLASH; g.warnColor = CD_COL_RED;
    return g;
}
// find first free cell scanning the grid for a w x h block
static bool cdFreeBlock(int w, int h, int& ox, int& oy) {
    for (int y = 0; y + h <= CD_ROWS; y++)
        for (int x = 0; x + w <= CD_COLS; x++)
            if (!cdOverlap(-1, x, y, w, h)) { ox = x; oy = y; return true; }
    return false;
}
void cdSeedDefaults() {
    g_cdGaugeCount = 0;
    for (int i = 0; i < CD_MAX_GAUGES; i++) g_cdGauges[i].valid = false;
    int rpm = 0, spd = 1, afr = 4;  // indices into availablePids
    int x, y;
    if (cdFreeBlock(4, 2, x, y))   g_cdGauges[g_cdGaugeCount++] = cdDefaultGauge(rpm, x, y, 4, 2);
    if (cdFreeBlock(4, 2, x, y))   g_cdGauges[g_cdGaugeCount++] = cdDefaultGauge(spd, x, y, 4, 2);
    if (cdFreeBlock(2, 2, x, y))   g_cdGauges[g_cdGaugeCount++] = cdDefaultGauge(afr, x, y, 2, 2);
    // RPM redline warning as a sensible starter cue
    for (int i = 0; i < g_cdGaugeCount; i++)
        if (!strcmp(availablePids[g_cdGauges[i].pid].idStr, "RPM")) {
            g_cdGauges[i].warnHi = 6200; g_cdGauges[i].warnLo = 0;
            g_cdGauges[i].style = CD_STYLE_NUMBER;
        }
    cdSavePrefs();
}

// ---- Query integration: poll the PIDs this dash displays ------------------
// (called from sendToyotaObdQueries to append dash PIDs to the query set)
void cdAppendQueries(uint8_t modes[], uint8_t pids[], int& count, int cap) {
    for (int i = 0; i < g_cdGaugeCount && count < cap; i++) {
        uint8_t idx = g_cdGauges[i].pid;
        if (availablePids[idx].mode == 0x00) continue;
        bool exists = false;
        for (int q = 0; q < count; q++)
            if (modes[q] == availablePids[idx].mode && pids[q] == availablePids[idx].pid) { exists = true; break; }
        if (!exists) { modes[count] = availablePids[idx].mode; pids[count] = availablePids[idx].pid; count++; }
    }
}

// =========================================================================
// GAUGE RENDERING
// =========================================================================
static void cdDrawGauge(const CdGauge& g) {
    int x, y, w, h; cdCellRect(g.x, g.y, g.w, g.h, x, y, w, h);
    // 2px inset gutter so gauges never touch
    x += 2; y += 2; w -= 4; h -= 4;
    bool warn = cdWarnOn(g);
    uint16_t border = warn ? cdColor((CdColor)g.warnColor) : C_CARD_BORDER;

    bool flashOn = true;
    if (warn && g.warnMode == CD_WARN_FLASH) flashOn = ((millis() / 300) % 2) == 0;

    uint16_t bg = warn ? cdColor((CdColor)g.warnColor) : C_CARD_BG;
    if (!flashOn) bg = C_CARD_INNER;
    canvas.fillRoundRect(x, y, w, h, 8, bg);
    canvas.drawRoundRect(x, y, w, h, 8, border);

    CdMeta m = cdMeta(g.pid);
    const char* label = availablePids[g.pid].idStr;
    bool gear = cdIsGear(g.pid);
    float v = cdVal(g);
    uint16_t valColor = warn ? (flashOn ? TFT_BLACK : C_TEXT_WHITE) : C_TEXT_WHITE;

    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(warn && flashOn ? TFT_BLACK : C_TEXT_MUTED, bg);
    canvas.drawString(label, x + 8, y + 6);

    char buf[24];
    if (gear) {
        snprintf(buf, sizeof(buf), "%s%s", vehicleData.gear,
                 (vehicleData.tccLocked && vehicleData.gear[0] >= '1' && vehicleData.gear[0] <= '6') ? "L" : "");
    } else {
        snprintf(buf, sizeof(buf), "%.*f%s", g.decimals, v, m.unit[0] ? " " : "");
    }

    int valSize = (h > 70) ? 2 : ((h > 46) ? 1 : 0);
    canvas.setFont(valSize == 2 ? &fonts::Font4 : &fonts::Font2);
    canvas.setTextColor(valColor, bg);

    if (g.style == CD_STYLE_NUMBER || (g.style != CD_STYLE_NUMBER && h < 44)) {
        canvas.drawCenterString(buf, x + w / 2, y + h / 2 - 12);
    } else if (g.style == CD_STYLE_HBAR) {
        canvas.drawRightString(buf, x + w - 8, y + 6);
        int barX = x + 8, barY = y + h - 26, barW = w - 16, barH = 14;
        canvas.drawRoundRect(barX, barY, barW, barH, 3, warn && flashOn ? TFT_BLACK : C_CARD_BORDER);
        float frac = (g.maxVal > g.minVal) ? (v - g.minVal) / (g.maxVal - g.minVal) : 0;
        if (frac < 0) frac = 0; if (frac > 1) frac = 1;
        int fillW = (int)(frac * (barW - 4));
        if (fillW > 0)
            canvas.fillRect(barX + 2, barY + 2, fillW, barH - 4,
                            warn ? (flashOn ? TFT_BLACK : cdColor((CdColor)g.warnColor)) : C_TEXT_CYAN);
    } else { // VBAR
        canvas.drawCenterString(buf, x + w / 2, y + 6);
        int barX = x + 8, barY = y + 26, barW = 16, barH = h - 34;
        canvas.drawRoundRect(barX, barY, barW, barH, 3, warn && flashOn ? TFT_BLACK : C_CARD_BORDER);
        float frac = (g.maxVal > g.minVal) ? (v - g.minVal) / (g.maxVal - g.minVal) : 0;
        if (frac < 0) frac = 0; if (frac > 1) frac = 1;
        int fillH = (int)(frac * (barH - 4));
        if (fillH > 0)
            canvas.fillRect(barX + 2, barY + barH - 2 - fillH, barW - 4, fillH,
                            warn ? (flashOn ? TFT_BLACK : cdColor((CdColor)g.warnColor)) : C_TEXT_CYAN);
    }
}

// Customize pill (top-right of content strip, normal mode only)
#define CD_BTN_CUST_X 664
#define CD_BTN_CUST_Y 52
#define CD_BTN_CUST_W 124
#define CD_BTN_CUST_H 24
static void cdDrawCustomizePill() {
    canvas.fillRoundRect(CD_BTN_CUST_X, CD_BTN_CUST_Y, CD_BTN_CUST_W, CD_BTN_CUST_H, 4, canvas.color565(25, 35, 52));
    canvas.drawRoundRect(CD_BTN_CUST_X, CD_BTN_CUST_Y, CD_BTN_CUST_W, CD_BTN_CUST_H, 4, canvas.color565(60, 100, 160));
    canvas.setTextColor(C_TEXT_WHITE); canvas.setFont(&fonts::Font2);
    canvas.drawCenterString("CUSTOMIZE", CD_BTN_CUST_X + CD_BTN_CUST_W / 2, CD_BTN_CUST_Y + 4);
}

void renderCustomDash() {
    if (g_cdEditorKind != CD_EDIT_NONE) { renderCustomDashEditor(); return; }
    drawHeaderBar(g_cdEditMode ? "CUSTOM DASH - EDIT MODE" : "CUSTOM DASH");

    if (!g_cdEditMode) {
        if (g_cdGaugeCount == 0) {
            canvas.setFont(&fonts::Font4); canvas.setTextColor(C_TEXT_MUTED);
            canvas.drawCenterString("No gauges yet.", 400, 220);
            canvas.drawCenterString("Tap CUSTOMIZE to add one.", 400, 250);
        }
        for (int i = 0; i < CD_MAX_GAUGES; i++)
            if (g_cdGauges[i].valid) cdDrawGauge(g_cdGauges[i]);
        cdDrawCustomizePill();
        drawBottomNavBar();
        return;
    }

    // EDIT MODE: dim grid lines + drag affordances
    canvas.drawRect(CD_GRID_X, CD_GRID_Y, CD_GRID_W, CD_GRID_H, C_CARD_BORDER);
    for (int c = 1; c < CD_COLS; c++)
        canvas.drawLine(CD_GRID_X + c * CD_CELL_W, CD_GRID_Y, CD_GRID_X + c * CD_CELL_W, CD_GRID_Y + CD_GRID_H, canvas.color565(30, 36, 50));
    for (int r = 1; r < CD_ROWS; r++)
        canvas.drawLine(CD_GRID_X, CD_GRID_Y + r * CD_CELL_H, CD_GRID_X + CD_GRID_W, CD_GRID_Y + r * CD_CELL_H, canvas.color565(30, 36, 50));
    for (int i = 0; i < CD_MAX_GAUGES; i++) {
        if (!g_cdGauges[i].valid) continue;
        cdDrawGauge(g_cdGauges[i]);
        // resize handle (bottom-right corner)
        int x, y, w, h; cdCellRect(g_cdGauges[i].x, g_cdGauges[i].y, g_cdGauges[i].w, g_cdGauges[i].h, x, y, w, h);
        canvas.fillTriangle(x + w - 4, y + h - 4, x + w - 22, y + h - 4, x + w - 4, y + h - 22, C_TEXT_CYAN);
    }
    // bottom edit toolbar
    int by = 412;
    canvas.fillRoundRect(12, by, 200, 44, 6, canvas.color565(15, 38, 22));
    canvas.drawRoundRect(12, by, 200, 44, 6, C_GREEN_OK);
    canvas.setTextColor(C_TEXT_WHITE); canvas.setFont(&fonts::Font4);
    canvas.drawCenterString("+ ADD GAUGE", 112, by + 8);
    canvas.fillRoundRect(224, by, 200, 44, 6, canvas.color565(45, 20, 25));
    canvas.drawRoundRect(224, by, 200, 44, 6, C_TRD_BURGUNDY);
    canvas.setTextColor(C_TEXT_WHITE); canvas.drawCenterString("RESET LAYOUT", 324, by + 8);
    canvas.fillRoundRect(436, by, 352, 44, 6, C_TRD_ORANGE);
    canvas.setTextColor(TFT_BLACK); canvas.drawCenterString("DONE EDITING", 612, by + 8);
}

// =========================================================================
// GAUGE EDITOR MODAL (full screen)
// =========================================================================
// shared layout rects used by BOTH render and hit-test
static const int E_TOP = 52, E_ROW = 62;
static int  e_rowY(int r) { return E_TOP + 40 + r * E_ROW; }
static bool inRect(int px, int py, int x, int y, int w, int h) { return px>=x && px<=x+w && py>=y && py<=y+h; }

static void cdEditorHit(int x, int y);  // forward

void renderCustomDashEditor() {
    drawHeaderBar(g_cdEditorKind == CD_EDIT_ADD ? "ADD GAUGE" : "EDIT GAUGE");
    canvas.fillRect(0, E_TOP, 800, 480 - E_TOP - 40, canvas.color565(14, 16, 22));

    if (g_cdEditorKind == CD_EDIT_ADD) {
        canvas.setFont(&fonts::Font2); canvas.setTextColor(C_TEXT_MUTED);
        canvas.drawString("Choose a parameter to add:", 34, E_TOP + 12);
        int colW = 250, rowH = 46, sx = 12, sy = E_TOP + 44;
        for (size_t i = 0; i < PID_COUNT; i++) {
            int col = i % 3, row = i / 3;
            int bx = sx + col * (colW + 8), by = sy + row * (rowH + 6);
            canvas.fillRoundRect(bx, by, colW, rowH, 6, C_CARD_BG);
            canvas.drawRoundRect(bx, by, colW, rowH, 6, C_CARD_BORDER);
            canvas.setTextColor(C_TEXT_WHITE); canvas.setFont(&fonts::Font2);
            canvas.drawString(availablePids[i].label, bx + 10, by + 15);
        }
        // Cancel button (bottom)
        canvas.fillRoundRect(436, 412, 352, 44, 6, canvas.color565(45, 20, 25));
        canvas.setTextColor(C_TEXT_WHITE); canvas.setFont(&fonts::Font4);
        canvas.drawCenterString("CANCEL", 612, 420);
        return;
    }

    if (g_cdEditorIdx < 0 || g_cdEditorIdx >= CD_MAX_GAUGES || !g_cdGauges[g_cdEditorIdx].valid) {
        g_cdEditorKind = CD_EDIT_NONE; return;
    }
    CdGauge& g = g_cdGauges[g_cdEditorIdx];
    char buf[48];
    canvas.setFont(&fonts::Font4); canvas.setTextColor(C_TEXT_WHITE);
    canvas.drawString(availablePids[g.pid].label, 34, E_TOP + 12);
    // DELETE button (top-right)
    canvas.fillRoundRect(656, E_TOP + 6, 132, 34, 6, canvas.color565(45, 18, 22));
    canvas.drawRoundRect(656, E_TOP + 6, 132, 34, 6, C_TRD_RED);
    canvas.setTextColor(canvas.color565(255,120,120)); canvas.setFont(&fonts::Font2);
    canvas.drawCenterString("DELETE", 722, E_TOP + 15);

    const char* styles[3] = {"NUMBER", "HBAR", "VBAR"};
    const char* warnModes[3] = {"OFF", "COLOR", "FLASH"};
    CdColor cols[6] = {CD_COL_CYAN, CD_COL_GREEN, CD_COL_ORANGE, CD_COL_RED, CD_COL_WHITE, CD_COL_GOLD};

    // Row 0: style
    canvas.setFont(&fonts::Font2); canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawString("STYLE", 34, e_rowY(0) + 12);
    for (int i = 0; i < 3; i++) {
        int bx = 160 + i * 150;
        bool on = g.style == i;
        canvas.fillRoundRect(bx, e_rowY(0), 140, 40, 6, on ? C_TRD_RED : C_CARD_BG);
        canvas.drawRoundRect(bx, e_rowY(0), 140, 40, 6, on ? C_TEXT_WHITE : C_CARD_BORDER);
        canvas.setTextColor(C_TEXT_WHITE); canvas.setFont(&fonts::Font2);
        canvas.drawCenterString(styles[i], bx + 70, e_rowY(0) + 12);
    }

    // Row 1: MIN / MAX steppers
    canvas.setFont(&fonts::Font2); canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawString("SCALE", 34, e_rowY(1) + 12);
    snprintf(buf, sizeof(buf), "MIN %.0f", g.minVal);
    canvas.fillRoundRect(160, e_rowY(1), 150, 40, 6, C_CARD_INNER); canvas.drawRoundRect(160, e_rowY(1), 150, 40, 6, C_CARD_BORDER);
    canvas.setTextColor(C_TEXT_MUTED); canvas.drawString("-", 168, e_rowY(1) + 12);
    canvas.setTextColor(C_TEXT_WHITE); canvas.drawCenterString(buf, 250, e_rowY(1) + 12);
    canvas.setTextColor(C_TEXT_MUTED); canvas.drawString("+", 296, e_rowY(1) + 12);
    snprintf(buf, sizeof(buf), "MAX %.0f", g.maxVal);
    canvas.fillRoundRect(330, e_rowY(1), 150, 40, 6, C_CARD_INNER); canvas.drawRoundRect(330, e_rowY(1), 150, 40, 6, C_CARD_BORDER);
    canvas.setTextColor(C_TEXT_MUTED); canvas.drawString("-", 338, e_rowY(1) + 12);
    canvas.setTextColor(C_TEXT_WHITE); canvas.drawCenterString(buf, 420, e_rowY(1) + 12);
    canvas.setTextColor(C_TEXT_MUTED); canvas.drawString("+", 466, e_rowY(1) + 12);

    // Row 2: WARN HIGH value + OFF
    canvas.setFont(&fonts::Font2); canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawString("WARN>", 34, e_rowY(2) + 12);
    snprintf(buf, sizeof(buf), "%.0f", g.warnHi <= -1e29f ? 0 : g.warnHi);
    canvas.fillRoundRect(160, e_rowY(2), 320, 40, 6, g.warnHi <= -1e29f ? C_CARD_INNER : canvas.color565(45, 20, 25));
    canvas.drawRoundRect(160, e_rowY(2), 320, 40, 6, g.warnHi <= -1e29f ? C_CARD_BORDER : C_TRD_RED);
    canvas.setTextColor(C_TEXT_MUTED); canvas.drawString("-", 168, e_rowY(2) + 12);
    canvas.setTextColor(C_TEXT_WHITE); canvas.drawCenterString(g.warnHi <= -1e29f ? "OFF" : buf, 320, e_rowY(2) + 12);
    canvas.setTextColor(C_TEXT_MUTED); canvas.drawString("+", 466, e_rowY(2) + 12);

    // Row 3: warn mode + color swatches
    canvas.setFont(&fonts::Font2); canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawString("CUE", 34, e_rowY(3) + 12);
    for (int i = 0; i < 3; i++) {
        int bx = 160 + i * 90;
        bool on = g.warnMode == i;
        canvas.fillRoundRect(bx, e_rowY(3), 82, 40, 6, on ? C_TRD_ORANGE : C_CARD_BG);
        canvas.drawRoundRect(bx, e_rowY(3), 82, 40, 6, on ? C_TEXT_WHITE : C_CARD_BORDER);
        canvas.setTextColor(on ? TFT_BLACK : C_TEXT_MUTED); canvas.setFont(&fonts::Font0);
        canvas.drawCenterString(warnModes[i], bx + 41, e_rowY(3) + 15);
    }
    for (int i = 0; i < 6; i++) {
        int bx = 450 + i * 46;
        canvas.fillCircle(bx + 16, e_rowY(3) + 20, 16, cdColor(cols[i]));
        if (g.warnColor == i) canvas.drawCircle(bx + 16, e_rowY(3) + 20, 20, C_TEXT_WHITE);
    }

    // Row 4: decimals
    canvas.setFont(&fonts::Font2); canvas.setTextColor(C_TEXT_MUTED);
    canvas.drawString("DEC", 34, e_rowY(4) + 12);
    for (int i = 0; i < 3; i++) {
        int bx = 160 + i * 70;
        bool on = g.decimals == i;
        snprintf(buf, sizeof(buf), "%d", i);
        canvas.fillRoundRect(bx, e_rowY(4), 62, 40, 6, on ? C_TEXT_CYAN : C_CARD_BG);
        canvas.drawRoundRect(bx, e_rowY(4), 62, 40, 6, on ? C_TEXT_WHITE : C_CARD_BORDER);
        canvas.setTextColor(on ? TFT_BLACK : C_TEXT_MUTED); canvas.setFont(&fonts::Font2);
        canvas.drawCenterString(buf, bx + 31, e_rowY(4) + 12);
    }

    // bottom: DONE
    canvas.fillRoundRect(436, 412, 352, 44, 6, C_TRD_ORANGE);
    canvas.setTextColor(TFT_BLACK); canvas.setFont(&fonts::Font4);
    canvas.drawCenterString("DONE", 612, 420);
}

// editor hit-test (returns true if consumed)
static void cdEditorHit(int x, int y) {
    if (g_cdEditorKind == CD_EDIT_ADD) {
        int colW = 250, rowH = 46, sx = 12, sy = E_TOP + 44;
        for (size_t i = 0; i < PID_COUNT; i++) {
            int col = i % 3, row = i / 3;
            int bx = sx + col * (colW + 8), by = sy + row * (rowH + 6);
            if (inRect(x, y, bx, by, colW, rowH)) {
                int ox, oy;
                if (g_cdGaugeCount < CD_MAX_GAUGES && cdFreeBlock(2, 1, ox, oy))
                    g_cdGauges[g_cdGaugeCount++] = cdDefaultGauge((int)i, ox, oy, 2, 1);
                cdSavePrefs();
                g_cdEditorKind = CD_EDIT_NONE;
                return;
            }
        }
        if (inRect(x, y, 436, 412, 352, 44)) g_cdEditorKind = CD_EDIT_NONE;
        return;
    }
    if (g_cdEditorIdx < 0 || g_cdEditorIdx >= CD_MAX_GAUGES || !g_cdGauges[g_cdEditorIdx].valid) { g_cdEditorKind = CD_EDIT_NONE; return; }
    CdGauge& g = g_cdGauges[g_cdEditorIdx];

    if (inRect(x, y, 656, E_TOP + 6, 132, 34)) {  // DELETE
        g.valid = false;
        // compact count boundary
        if (g_cdEditorIdx == g_cdGaugeCount - 1) g_cdGaugeCount--;
        else { // find new highest valid index
            int hi = -1; for (int i = 0; i < CD_MAX_GAUGES; i++) if (g_cdGauges[i].valid) hi = i;
            g_cdGaugeCount = hi + 1;
        }
        cdSavePrefs(); g_cdEditorKind = CD_EDIT_NONE; return;
    }
    if (inRect(x, y, 436, 412, 352, 44)) { cdSavePrefs(); g_cdEditorKind = CD_EDIT_NONE; return; } // DONE

    // style
    for (int i = 0; i < 3; i++) if (inRect(x, y, 160 + i * 150, e_rowY(0), 140, 40)) { g.style = i; cdSavePrefs(); return; }
    // min -/+  (step 10% of span)
    float span = (g.maxVal - g.minVal) > 0 ? (g.maxVal - g.minVal) : 100;
    float step = span / 10.0f;
    if (inRect(x, y, 160, e_rowY(1), 40, 40))      { g.minVal -= step; cdSavePrefs(); return; }
    if (inRect(x, y, 290, e_rowY(1), 20, 40))      { g.minVal += step; cdSavePrefs(); return; }
    if (inRect(x, y, 330, e_rowY(1), 40, 40))      { g.maxVal -= step; cdSavePrefs(); return; }
    if (inRect(x, y, 460, e_rowY(1), 20, 40))      { g.maxVal += step; cdSavePrefs(); return; }
    // warn high -/+ (0 toggles off)
    if (inRect(x, y, 160, e_rowY(2), 40, 40)) {
        if (g.warnHi <= -1e29f) g.warnHi = g.maxVal; else g.warnHi -= step;
        if (g.warnHi <= g.minVal + step * 0.5f) g.warnHi = -1e30f; cdSavePrefs(); return;
    }
    if (inRect(x, y, 460, e_rowY(2), 20, 40)) {
        if (g.warnHi <= -1e29f) g.warnHi = g.maxVal + step; else g.warnHi += step; cdSavePrefs(); return;
    }
    // cue mode
    for (int i = 0; i < 3; i++) if (inRect(x, y, 160 + i * 90, e_rowY(3), 82, 40)) { g.warnMode = i; cdSavePrefs(); return; }
    // color swatches
    for (int i = 0; i < 6; i++) if (inRect(x, y, 450 + i * 46, e_rowY(3), 46, 40)) { g.warnColor = i; cdSavePrefs(); return; }
    // decimals
    for (int i = 0; i < 3; i++) if (inRect(x, y, 160 + i * 70, e_rowY(4), 62, 40)) { g.decimals = i; cdSavePrefs(); return; }
}

// =========================================================================
// TOUCH
// =========================================================================
bool cdTouchActive() { return g_cdDragIdx >= 0; }

bool cdHandlePress(int x, int y) {
    if (g_cdEditorKind != CD_EDIT_NONE) return true;      // modal eats everything
    if (!g_cdEditMode) {
        if (inRect(x, y, CD_BTN_CUST_X, CD_BTN_CUST_Y, CD_BTN_CUST_W, CD_BTN_CUST_H))
            { g_cdEditMode = true; return true; }
        return false;  // let swipe/navigation work in normal mode
    }
    // EDIT mode: check bottom toolbar first
    int by = 412;
    if (y >= by) {
        if (inRect(x, y, 12, by, 200, 44))  { g_cdEditorKind = CD_EDIT_ADD; return true; }
        if (inRect(x, y, 224, by, 200, 44)) { cdSeedDefaults(); return true; }
        if (inRect(x, y, 436, by, 352, 44)) { g_cdEditMode = false; cdSavePrefs(); return true; }
        return true;
    }
    // top-right of a gauge = resize; body = move; tap (no drag) = edit
    for (int i = g_cdGaugeCount - 1; i >= 0; i--) {
        if (!g_cdGauges[i].valid) continue;
        int gx, gy, gw, gh; cdCellRect(g_cdGauges[i].x, g_cdGauges[i].y, g_cdGauges[i].w, g_cdGauges[i].h, gx, gy, gw, gh);
        if (inRect(x, y, gx + gw - CD_CORNER_PX, gy + gh - CD_CORNER_PX, CD_CORNER_PX, CD_CORNER_PX)) {
            g_cdDragIdx = i; g_cdDragMode = 2; g_cdDragOffX = (gx + gw) - x; g_cdDragOffY = (gy + gh) - y; return true;
        }
        if (inRect(x, y, gx, gy, gw, gh)) {
            g_cdDragIdx = i; g_cdDragMode = 1;
            int cx, cy; cdPixelToCell(x, y, cx, cy);
            g_cdDragOffX = cx - g_cdGauges[i].x; g_cdDragOffY = cy - g_cdGauges[i].y;
            return true;
        }
    }
    return true;  // edit mode consumes empty taps too
}

bool cdHandleDrag(int x, int y) {
    if (g_cdDragIdx < 0) return false;
    CdGauge& g = g_cdGauges[g_cdDragIdx];
    if (g_cdDragMode == 1) {
        int cx, cy; cdPixelToCell(x, y, cx, cy);
        int nx = cx - g_cdDragOffX, ny = cy - g_cdDragOffY;
        if (nx < 0) nx = 0; if (nx + g.w > CD_COLS) nx = CD_COLS - g.w;
        if (ny < 0) ny = 0; if (ny + g.h > CD_ROWS) ny = CD_ROWS - g.h;
        // only move to free space
        if (!cdOverlap(g_cdDragIdx, nx, ny, g.w, g.h)) { g.x = nx; g.y = ny; }
    } else if (g_cdDragMode == 2) {
        int cx, cy; cdPixelToCell(x + g_cdDragOffX, y + g_cdDragOffY, cx, cy);
        int nx = g.x, ny = g.y;
        int nw = (cx + 1) - g.x, nh = (cy + 1) - g.y;
        if (nw < 1) nw = 1; if (nw > CD_COLS - g.x) nw = CD_COLS - g.x;
        if (nh < 1) nh = 1; if (nh > CD_ROWS - g.y) nh = CD_ROWS - g.y;
        if (!cdOverlap(g_cdDragIdx, nx, ny, nw, nh)) { g.w = nw; g.h = nh; }
    }
    return true;
}

bool cdHandleRelease(int x, int y) {
    if (g_cdEditorKind != CD_EDIT_NONE) { cdEditorHit(x, y); return true; }
    if (!g_cdEditMode) {
        if (inRect(x, y, CD_BTN_CUST_X, CD_BTN_CUST_Y, CD_BTN_CUST_W, CD_BTN_CUST_H)) { g_cdEditMode = true; return true; }
        return false;
    }
    if (g_cdDragIdx >= 0) {
        bool wasTap = (abs(x - touchStartX) < 20 && abs(y - touchStartY) < 20 && (millis() - touchStartTime) < 500);
        int idx = g_cdDragIdx;
        g_cdDragIdx = -1; g_cdDragMode = 0;
        if (wasTap) { g_cdEditorIdx = idx; g_cdEditorKind = CD_EDIT_GAUGE; }
        else cdSavePrefs();
        return true;
    }
    return true;  // edit-mode empty tap
}
