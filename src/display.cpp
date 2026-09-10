/* LVGL 9 render core, Waveshare ESP32-S3-Touch-LCD-4.3B (800x480 RGB LCD).
   Single PSRAM frame buffer with direct GDMA scan-out (bounce_buffer_size_px = 0).
   By avoiding the bounce buffer ISR, GDMA transfers directly via hardware DMA,
   completely eliminating the "Cache disabled but cached memory region accessed" panic.
*/

#include "display.h"

#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/gpio.h"
#include "esp32-hal-gpio.h"
#include <esp32-hal.h>
#include <cstring>
#include <cstdlib>
#include <algorithm>

#include <lvgl.h>
#include <Wire.h>

// from main.cpp (CH422G expander)
void ch422gSetPin(uint8_t bit, bool level);

LVGLCanvas canvas;   // drawing alias — used by all UI screens

static esp_lcd_panel_handle_t g_panel = nullptr;
static bool g_backlightOn = true;

// ---- static scratch buffer for lv_font_get_glyph_bitmap ----
static uint8_t glyph_bitmap_buf[4096];
static lv_draw_buf_t glyph_scratch = {};
static bool           glyph_scratch_ready = false;

static void ensure_glyph_scratch(void) {
    if (!glyph_scratch_ready) {
        lv_draw_buf_init(&glyph_scratch, 32, 32, LV_COLOR_FORMAT_A8, 32,
                         glyph_bitmap_buf, sizeof(glyph_bitmap_buf));
        glyph_scratch_ready = true;
    }
}

// ---- Arduino String is UTF-8; decode codepoints manually ----
static uint32_t _arduino_string_codepoint_at(const String &s, size_t &i) {
    if (i >= (size_t)s.length()) return 0;
    char c = s[i];
    uint32_t cp = (uint8_t)c;
    int len = 1;
    if ((c & 0x80) == 0) { cp = (uint8_t)c; len = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = (uint8_t)c & 0x1F; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = (uint8_t)c & 0x0F; len = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = (uint8_t)c & 0x07; len = 4; }
    for (int k = 1; k < len && i + k < (size_t)s.length(); k++) {
        cp = (cp << 6) | ((uint8_t)s[i + k] & 0x3F);
    }
    i += len;
    return cp;
}

bool LVGLCanvas::init(uint16_t w, uint16_t h, uint32_t pclk_hz) {
    _w = w; _h = h; _fbw = w; _fbh = h;

    Serial.printf("[DISPLAY] Initializing RGB panel %dx%d (pclk: %lu Hz)...\n", w, h, (unsigned long)pclk_hz);

    static const int data_gpio[16] = {14, 38, 18, 17, 10, 39, 0, 45,
                                      48, 47, 21, 1,  2,  42, 41, 40};

    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src = LCD_CLK_SRC_PLL160M;
    cfg.data_width = 16;
    cfg.num_fbs = 1;
    cfg.bounce_buffer_size_px = 0;   // Direct GDMA from PSRAM - NO bounce buffer ISR, zero cache panics!
    cfg.flags.fb_in_psram = 1;
    cfg.timings.pclk_hz = pclk_hz;
    cfg.timings.h_res = (uint32_t)w;
    cfg.timings.v_res = (uint32_t)h;
    cfg.timings.hsync_pulse_width = 4;
    cfg.timings.hsync_back_porch = 8;
    cfg.timings.hsync_front_porch = 8;
    cfg.timings.vsync_pulse_width = 4;
    cfg.timings.vsync_back_porch = 16;
    cfg.timings.vsync_front_porch = 16;
    cfg.timings.flags.hsync_idle_low = 1;
    cfg.timings.flags.vsync_idle_low = 1;
    cfg.timings.flags.de_idle_high = 0;
    cfg.timings.flags.pclk_active_neg = 1;
    cfg.timings.flags.pclk_idle_high = 1;
    for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = data_gpio[i];
    cfg.de_gpio_num = 5;
    cfg.hsync_gpio_num = 46;
    cfg.vsync_gpio_num = 3;
    cfg.pclk_gpio_num = 7;
    cfg.disp_gpio_num = -1;

    esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &g_panel);
    if (err != ESP_OK) {
        Serial.printf("[DISPLAY] esp_lcd_new_rgb_panel failed: 0x%x\n", err);
        return false;
    }

    err = esp_lcd_panel_reset(g_panel);
    if (err != ESP_OK) {
        Serial.printf("[DISPLAY] esp_lcd_panel_reset returned 0x%x\n", err);
    }
    err = esp_lcd_panel_init(g_panel);
    if (err != ESP_OK) {
        Serial.printf("[DISPLAY] esp_lcd_panel_init returned 0x%x\n", err);
    }

    void *fb0 = nullptr;
    err = esp_lcd_rgb_panel_get_frame_buffer(g_panel, 1, &fb0);
    if (err != ESP_OK || !fb0) {
        Serial.printf("[DISPLAY] esp_lcd_rgb_panel_get_frame_buffer failed: 0x%x, fb0=%p\n", err, fb0);
        return false;
    }
    _fb = (uint8_t *)fb0;
    Serial.printf("[DISPLAY] PSRAM frame buffer ready: %p (%u bytes)\n", _fb, (unsigned)(w * h * 2));
    memset(_fb, 0, (size_t)w * h * 2);

    static bool lv_inited = false;
    if (!lv_inited) {
        lv_init();
        lv_inited = true;
    }
    lv_display_t *disp = lv_display_create(w, h);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, _fb, nullptr, (size_t)w * h * 2, LV_DISPLAY_RENDER_MODE_DIRECT);
    _disp = disp;

    return true;
}

static void blSet(bool on) {
    g_backlightOn = on;
    extern void ch422gSetPin(uint8_t, bool);
    ch422gSetPin(2, on);
}
void displayBacklightOn()  { blSet(true); }
void displayBacklightOff() { blSet(false); }
bool displayBacklightEnabled() { return g_backlightOn; }

void LVGLCanvas::flush() {
    lv_tick_inc(1);
    lv_timer_handler();
}

void LVGLCanvas::setRotation(uint8_t r) {
    _flip = (r == 2);
    if (_disp) lv_display_set_rotation(_disp, _flip ? LV_DISPLAY_ROTATION_180
                                                    : LV_DISPLAY_ROTATION_0);
}

// ---- Drawing primitives straight into the PSRAM frame buffer ----

void LVGLCanvas::px(int x, int y, uint16_t c) {
    if (unsigned(x) > _fbw - 1 || unsigned(y) > _fbh - 1 || !_fb) return;
    int dx = _flip ? _fbw - 1 - x : x;
    int dy = _flip ? _fbh - 1 - y : y;
    ((uint16_t *)_fb)[(size_t)dy * _fbw + dx] = c;
    _dirty = true;
}
void LVGLCanvas::hline(int x0, int x1, int y, uint16_t c) {
    if (unsigned(y) > _fbh - 1 || !_fb) return;
    if (x0 > x1) std::swap(x0, x1);
    if (x1 < 0 || x0 >= (int)_fbw) return;
    x0 = std::max(0, x0);
    x1 = std::min((int)_fbw - 1, x1);
    int dy = _flip ? _fbh - 1 - y : y;
    uint16_t *row = (uint16_t *)_fb + (size_t)dy * _fbw;
    if (_flip) {
        for (int x = x0; x <= x1; x++) row[_fbw - 1 - x] = c;
    } else {
        for (int x = x0; x <= x1; x++) row[x] = c;
    }
    _dirty = true;
}
void LVGLCanvas::rect(int x, int y, int w, int h, uint16_t c) {
    if (!_fb) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)_fbw) w = _fbw - x;
    if (y + h > (int)_fbh) h = _fbh - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++) {
        hline(x, x + w - 1, yy, c);
    }
    _dirty = true;
}
void LVGLCanvas::invalidateRect(int32_t, int32_t, int32_t, int32_t) {
    _dirty = true;
}
uint16_t LVGLCanvas::color565(uint8_t r, uint8_t g, uint8_t b) const {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
void LVGLCanvas::setTextColor(uint16_t fg) { _fg = fg; }
void LVGLCanvas::setTextColor(uint16_t fg, uint16_t) { _fg = fg; }
void LVGLCanvas::setFont(const lv_font_t *f) { _font = f; }
void LVGLCanvas::setTextDatum(uint8_t d) { _datum = d; }
void LVGLCanvas::setTextPadding(uint16_t p) { _pad = p; }

void LVGLCanvas::fillScreen(uint16_t c) { rect(0, 0, _fbw, _fbh, c); }
void LVGLCanvas::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c) {
    rect(x, y, w, h, c);
}
void LVGLCanvas::drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c) {
    hline(x, x + w - 1, y, c); hline(x, x + w - 1, y + h - 1, c);
    for (int yy = y; yy < y + h; yy++) { px(x, yy, c); px(x + w - 1, yy, c); }
    _dirty = true;
}
void LVGLCanvas::drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        px(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
void LVGLCanvas::drawFastHLine(int32_t x, int32_t y, int32_t l, uint16_t c) {
    hline(x, x + l - 1, y, c);
}
void LVGLCanvas::fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h,
                               int32_t r, uint16_t c) {
    if (r < 2) { rect(x, y, w, h, c); return; }
    rect(x + r, y, w - 2 * r, h, c);
    rect(x, y + r, r, h - 2 * r, c);
    rect(x + w - r, y + r, r, h - 2 * r, c);
    rect(x + r, y, w - 2 * r, r, c);
    rect(x + r, y + h - r, w - 2 * r, r, c);
    px(x + r, y + r, c); px(x + w - r, y + r, c);
    px(x + r, y + h - r, c); px(x + w - r, y + h - r, c);
}
void LVGLCanvas::drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h,
                               int32_t r, uint16_t c) {
    hline(x + r, x + w - r, y, c); hline(x + r, x + w - r, y + h - 1, c);
    for (int yy = y + r; yy < y + h - r; yy++) { px(x, yy, c); px(x + w - 1, yy, c); }
    px(x + r, y + r, c); px(x + w - r, y + r, c);
    px(x + r, y + h - r, c); px(x + w - r, y + h - r, c);
    _dirty = true;
}
void LVGLCanvas::drawFastVLine(int32_t x, int32_t y, int32_t h, uint16_t c) {
    for (int32_t yy = y; yy < y + h; yy++) px(x, yy, c);
    _dirty = true;
}
void LVGLCanvas::drawCircle(int32_t x, int32_t y, int32_t r, uint16_t c) {
    int x0 = 0, y0 = r;
    int d = 3 - 2 * r;
    while (y0 >= x0) {
        px(x + x0, y + y0, c); px(x - x0, y + y0, c);
        px(x + y0, y + x0, c); px(x - y0, y + x0, c);
        px(x + x0, y - y0, c); px(x - x0, y - y0, c);
        px(x + y0, y - x0, c); px(x - y0, y - x0, c);
        if (d < 0) d += 4 * x0 + 6;
        else { d += 4 * (x0 - y0) + 10; y0--; }
        x0++;
    }
    _dirty = true;
}

void LVGLCanvas::fillCircle(int32_t x, int32_t y, int32_t r, uint16_t c) {
    int x0 = 0, y0 = r;
    int d = 3 - 2 * r;
    while (y0 >= x0) {
        for (int xx = x - x0; xx <= x + x0; xx++) {
            px(xx, y + y0, c); px(xx, y - y0, c);
        }
        for (int yy = y - y0 + 1; yy <= y + y0 - 1; yy++) {
            px(x - x0, yy, c); px(x + x0, yy, c);
        }
        if (d < 0) d += 4 * x0 + 6;
        else { d += 4 * (x0 - y0) + 10; y0--; }
        x0++;
    }
    _dirty = true;
}

void LVGLCanvas::fillTriangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                              int32_t x2, int32_t y2, uint16_t c) {
    int32_t minx = std::min({x0, x1, x2}), maxx = std::max({x0, x1, x2});
    int32_t miny = std::min({y0, y1, y2}), maxy = std::max({y0, y1, y2});
    auto side = [](int32_t ax, int32_t ay, int32_t bx, int32_t by,
                   int32_t px_, int32_t py_) {
        return (bx - ax) * (py_ - ay) - (by - ay) * (px_ - ax);
    };
    for (int y = miny; y <= maxy; y++)
        for (int x = minx; x <= maxx; x++) {
            int32_t w0 = side(x0, y0, x1, y1, x, y);
            int32_t w1 = side(x1, y1, x2, y2, x, y);
            int32_t w2 = side(x2, y2, x0, y0, x, y);
            if (w0 * w1 >= 0 && w1 * w2 >= 0) px(x, y, c);
        }
    _dirty = true;
}

int32_t LVGLCanvas::textW(const String &s) const {
    if (!_font) return 0;
    int32_t w = 0;
    size_t i = 0;
    while (i < (size_t)s.length()) {
        uint32_t cp = _arduino_string_codepoint_at(s, i);
        lv_font_glyph_dsc_t dsc = {};
        if (lv_font_get_glyph_dsc(_font, &dsc, cp, 0) && (dsc.adv_w || cp == ' '))
            w += dsc.adv_w;
    }
    return w + _pad * std::max(1, (int)(s.length() ? s.length() : 1));
}

int32_t LVGLCanvas::textH() const {
    return _font ? lv_font_get_line_height(_font) : 14;
}

void LVGLCanvas::glyph(const lv_font_t *f, uint32_t cp, int32_t &x, int32_t y) {
    if (cp == ' ') { x += 4; return; }

    ensure_glyph_scratch();

    lv_font_glyph_dsc_t dsc = {};
    if (!lv_font_get_glyph_dsc(f, &dsc, cp, 0)) { x += dsc.adv_w ?: 4; return; }

    const void *raw = lv_font_get_glyph_bitmap(&dsc, &glyph_scratch);
    if (!raw) { x += dsc.adv_w ?: 4; return; }

    int32_t bw  = dsc.box_w;
    int32_t bh  = dsc.box_h;
    int32_t ox_ = dsc.ofs_x;
    int32_t oy_ = dsc.ofs_y;
    int32_t base_y = y;

    if (dsc.format == LV_FONT_GLYPH_FORMAT_A8 && bw > 0 && bh > 0) {
        for (int yy = 0; yy < bh; yy++)
            for (int xx = 0; xx < bw; xx++)
                px(ox_ + xx, base_y + oy_ + yy, _fg);
    }
    x += dsc.adv_w;
}

void LVGLCanvas::drawString(const String &s, int32_t x, int32_t y) {
    int32_t ox = x, oy = y;
    if (_datum & 2) { int32_t w = textW(s); ox = x - w / 2; }
    else if (_datum & 8) { int32_t w = textW(s); ox = x - w; }
    if (_font) {
        int32_t yy = oy - textH() / 2;
        if (_datum & 1) yy = oy;
        if (_datum & 16) yy = oy - textH();
        size_t i = 0;
        while (i < (size_t)s.length()) {
            uint32_t cp = _arduino_string_codepoint_at(s, i);
            glyph(_font, cp, ox, yy);
        }
    }
    _dirty = true;
}
void LVGLCanvas::drawString(const String &s, int32_t x, int32_t y, uint16_t c) {
    uint16_t save = _fg; _fg = c; drawString(s, x, y); _fg = save;
}
void LVGLCanvas::drawCenterString(const String &s, int32_t x, int32_t y) {
    uint8_t save = _datum; _datum = 2; drawString(s, x, y); _datum = save;
}
void LVGLCanvas::drawCenterString(const String &s, int32_t x, int32_t y, uint16_t c) {
    uint8_t save = _datum; _datum = 2; drawString(s, x, y, c); _datum = save;
}
void LVGLCanvas::drawRightString(const String &s, int32_t x, int32_t y) {
    uint8_t save = _datum; _datum = 8; drawString(s, x, y); _datum = save;
}
void LVGLCanvas::drawRightString(const String &s, int32_t x, int32_t y, uint16_t c) {
    uint8_t save = _datum; _datum = 8; drawString(s, x, y, c); _datum = save;
}

// ---- GT911 -> LVGL indev (11a218d: track reg is 0x814F, not 0x814E) ----

#define GT911_REG_POINT_STAT 0x814F
static uint8_t gt911Addr = 0;
static lv_indev_t *g_indev = nullptr;

static void gt911_read(lv_indev_t *indev, lv_indev_data_t *data);

void displayTouchInit() {
    Wire.beginTransmission(0x5D);
    Wire.write(GT911_REG_POINT_STAT >> 8); Wire.write(GT911_REG_POINT_STAT & 0xFF);
    if (Wire.endTransmission(false) == 0 && Wire.requestFrom((uint8_t)0x5D, (uint8_t)1) == 1) {
        Wire.read(); gt911Addr = 0x5D; Wire.read();
        Serial.printf("[TOUCH] GT911 online at 0x%02X (INT: GPIO4)\n", gt911Addr);
    } else {
        Serial.println("[TOUCH] No GT911 found — touch disabled.");
    }
}

static void gt911_read(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    if (!gt911Addr) { data->state = LV_INDEV_STATE_RELEASED; return; }

    static int sx = 0, sy = 0;
    static bool down = false;

    Wire.beginTransmission(gt911Addr);
    Wire.write(GT911_REG_POINT_STAT >> 8); Wire.write(GT911_REG_POINT_STAT & 0xFF);
    bool ok = Wire.endTransmission(false) == 0 && Wire.requestFrom(gt911Addr, (uint8_t)1) == 1;
    if (!ok) { data->state = LV_INDEV_STATE_RELEASED; return; }

    uint8_t status = Wire.read();
    if (!(status & 0x80)) {
        data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        data->point.x = sx; data->point.y = sy;
        return;
    }

    uint8_t count = status & 0x0F;
    if (count == 0 || count > 5) {
        Wire.beginTransmission(gt911Addr);
        Wire.write(GT911_REG_POINT_STAT >> 8); Wire.write(GT911_REG_POINT_STAT & 0xFF);
        Wire.write(0); Wire.endTransmission();
        data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        return;
    }

    uint8_t pt[5] = {0};
    Wire.beginTransmission(gt911Addr);
    Wire.write(0x8150 >> 8); Wire.write(0x8150 & 0xFF);
    ok = Wire.endTransmission(false) == 0 && Wire.requestFrom(gt911Addr, (uint8_t)5) == 5;
    if (ok) for (int i = 0; i < 5; i++) pt[i] = Wire.read();

    Wire.beginTransmission(gt911Addr);
    Wire.write(GT911_REG_POINT_STAT >> 8); Wire.write(GT911_REG_POINT_STAT & 0xFF);
    Wire.write(0); Wire.endTransmission();
    if (!ok) { data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED; return; }

    int rx = pt[1] | (pt[2] << 8);
    int ry = pt[3] | (pt[4] << 8);
    if (rx > 799) rx = 799;
    if (ry > 479) ry = 479;

    int fx = rx, fy = ry;
    if (canvas.flip()) { fx = 799 - rx; fy = 479 - ry; }
    sx = fx; sy = fy; down = true;
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = sx;
    data->point.y = sy;
}

bool displayTouchRead(int &x, int &y) {
    x = 0; y = 0;
    if (!gt911Addr) return false;
    if (!g_indev) {
        g_indev = lv_indev_create();
        lv_indev_set_type(g_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(g_indev, gt911_read);
        lv_indev_set_group(g_indev, lv_group_create());
    }
    lv_indev_read(g_indev);
    static lv_point_t last_pt = {};
    lv_indev_get_point(g_indev, &last_pt);
    x = (int)last_pt.x; y = (int)last_pt.y;
    return lv_indev_get_state(g_indev) == LV_INDEV_STATE_PRESSED;
}
