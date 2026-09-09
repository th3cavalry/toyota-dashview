/* LVGL 9 render core, 4.3B (LVGL-MIGRATION.md S1).

   Bounce-buffer-only scan-out — esp_lcd RGB "no_fb" mode. There is NO frame
   buffer anywhere, not even in PSRAM: two 3.2 KB internal-SRAM bounce buffers
   (one 800px RGB565 line each, 1600 B) ARE the scan-out source and the two LVGL
   draw buffers. The GDMA EOF ISR fires per line (~59 us at 14 MHz pclk): in
   no_fb mode the driver calls on_bounce_empty INSTEAD of the framebuffer->bounce
   memcpy — so the ISR path contains zero PSRAM reads, zero cache-sync tricks.
   The boot-loop panic "Cache disabled but cached memory region accessed" came
   from exactly that deleted memcpy reading cached PSRAM while SPI flash ops
   (WiFi/NVS/SD init — spi_flash_op_block_func on the backtrace) disabled the
   DCache on both cores. No framebuffer, no memcpy, no cached PSRAM in the ISR:
   unreachable by construction — and the PSRAM-bandwidth flicker it fed dies too.
   Layout: bounce buffer k feeds physical lines k, k+2, … (1600 B = one line, so
   bounce_pos_px steps 800 px per EOF); LVGL renders DIRECT into the idle buffer.
   GT911 -> LVGL indev (11a218d fix kept); CH422G -> brightness.
   Boot splash is plain canvas drawing (splash.cpp).
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
#include "draw/lv_draw_buf_private.h"   // full lv_draw_buf_handlers_t (lvgl.h only fwd-declares)
#include <Wire.h>

// from main.cpp (CH422G expander, defined in the display section below setup())
void ch422gSetPin(uint8_t bit, bool level);

LVGLCanvas canvas;   // drawing alias — same name the UI code already uses

static esp_lcd_panel_handle_t g_panel = nullptr;
static bool g_backlightOn = true;
static uint8_t *g_bb[2] = {nullptr, nullptr};  // bounce bufs = the two draw buffers

static bool IRAM_ATTR on_bounce_empty(esp_lcd_panel_handle_t panel,
                                      void *bounce_buf, int pos_px,
                                      int len_bytes, void *user_ctx) {
    // GDMA EOF ISR (~every 59 us): bounce buffer (pos_px/800)&1 just streamed out
    // its single line (line pos_px/800) to the panel. no_fb mode calls us INSTEAD
    // of the PSRAM memcpy — the buffer is already LVGL-current (flush_cb wrote the
    // dirty rect into it pre-ISR), so there is nothing to fill. Never yield here.
    (void)bounce_buf; (void)pos_px; (void)len_bytes; (void)panel; (void)user_ctx;
    return false;
}

static bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t panel,
                               const esp_lcd_rgb_panel_event_data_t *edata,
                               void *user_ctx) {
    // VSYNC: one frame scanned out; bounce_pos_px resets, buffer 0 leads again.
    return false;
}

static void IRAM_ATTR on_flush(lv_display_t *disp, const lv_area_t *area,
                               uint8_t *px_map) {
    // DIRECT mode: px_map is the whole-frame draw buffer = the bounce buffer LVGL
    // just rendered into (buf_act's data; g_bb[buf_act==buf_1]). That buffer
    // physically holds exactly one line: physical line L lives in buffer L&1 at
    // pixel offset (L>>1)*800 (bounce_pos_px steps one 800px line per EOF). So
    // copy the rect's rows there — internal-SRAM -> internal-SRAM memcpy through
    // the DCache, safe by construction even with the cache disabled.
    (void)disp;
    const int x0 = std::max<int32_t>(area->x1, 0), x1 = std::min<int32_t>(area->x2 + 1, 800);
    const int y0 = std::max<int32_t>(area->y1, 0), y1 = std::min<int32_t>(area->y2 + 1, 480);
    if (x0 >= x1 || y0 >= y1) return;
    const uint16_t *src = (const uint16_t *)px_map;   // strip rows start at 0
    for (int y = y0; y < y1; y++)
        memcpy(g_bb[y & 1] + (size_t)(y >> 1) * 1600 + (size_t)x0 * 2,
               src + (size_t)(y - y0) * 800 + x0, (size_t)(x1 - x0) * 2);
}

// ---- LVGL draw-buffer handlers: bounce buffers are 32B-aligned internal SRAM;
// nothing here sits behind the PSRAM cache, so the cache ops are no-ops and the
// LVGL_DRAW_BUF_*_ALIGN configs must match the 32-byte DMA alignment.
static void *bbbuf_malloc(size_t size, lv_color_format_t cf) {
    (void)cf;
    return heap_caps_aligned_calloc(32, 1, size + 31,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
}
static void bbbuf_free(void *p) { free(p); }
static void *bbbuf_align(void *p, lv_color_format_t cf) {
    (void)cf;
    return (void *)(((lv_uintptr_t)p + 31) & ~(lv_uintptr_t)31);
}
static void bbbuf_copy(lv_draw_buf_t *dst, const lv_area_t *dst_area,
                       const lv_draw_buf_t *src, const lv_area_t *src_area) {
    (void)dst_area; (void)src_area;
    memcpy(dst->data, src->data, src->data_size);
}
static uint32_t bbbuf_stride(uint32_t w, lv_color_format_t cf) {
    return LV_ROUND_UP(w * lv_color_format_get_bpp(cf) / 8, 32);
}

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
    uint32_t cp;
    int len;
    if ((c & 0x80) == 0) { cp = (uint8_t)c; len = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = (uint8_t)c & 0x1F; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = (uint8_t)c & 0x0F; len = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = (uint8_t)c & 0x07; len = 4; }
    else { cp = (uint8_t)c; len = 1; }
    for (int k = 1; k < len && i + k < (size_t)s.length(); k++) {
        cp = (cp << 6) | ((uint8_t)s[i + k] & 0x3F);
    }
    i += len;
    return cp;
}

bool LVGLCanvas::init(uint16_t w, uint16_t h, uint32_t pclk_hz) {
    _w = w; _h = h; _fbw = w; _fbh = h;

    static const int data_gpio[16] = {14, 38, 18, 17, 10, 39, 0, 45,
                                      48, 47, 21, 1,  2,  42, 41, 40};

    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src = LCD_CLK_SRC_PLL160M;
    cfg.data_width = 16;
    cfg.bounce_buffer_size_px = 800;   // one 565 line per bounce buffer: 800px*2B = 1600B
    cfg.flags.no_fb = 1;               // bounce-buffer-only mode: no FB, no PSRAM, no panic
    cfg.timings.pclk_hz = pclk_hz;
    cfg.timings.h_res = (uint32_t)w; cfg.timings.v_res = (uint32_t)h;
    cfg.timings.hsync_pulse_width = 4; cfg.timings.hsync_back_porch = 8; cfg.timings.hsync_front_porch = 8;
    cfg.timings.vsync_pulse_width = 4; cfg.timings.vsync_back_porch = 16; cfg.timings.vsync_front_porch = 16;
    cfg.timings.flags.hsync_idle_low = 1;
    cfg.timings.flags.vsync_idle_low = 1;
    cfg.timings.flags.de_idle_high = 0;
    cfg.timings.flags.pclk_active_neg = 1;
    cfg.timings.flags.pclk_idle_high = 1;
    for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = data_gpio[i];
    cfg.de_gpio_num = 5;   cfg.hsync_gpio_num = 46;
    cfg.vsync_gpio_num = 3; cfg.pclk_gpio_num = 7;
    cfg.disp_gpio_num = -1;

    esp_lcd_rgb_panel_event_callbacks_t cbs = {};
    cbs.on_bounce_empty = on_bounce_empty;
    cbs.on_vsync = on_vsync;

    if (esp_lcd_new_rgb_panel(&cfg, &g_panel) != ESP_OK) return false;
    // No esp_lcd_panel_init/reset here: the v5.3 RGB driver auto-starts
    // transmission inside esp_lcd_new_rgb_panel (its start_transmission pre-fills
    // both bounce buffers itself). An external reset/init would only restart that
    // GDMA chain pointlessly — and in fb_in_psram modes re-arm the PSRAM panic.
    esp_lcd_rgb_panel_register_event_callbacks(g_panel, &cbs, nullptr);

    // Publish the two bounce buffers to LVGL as the ONLY draw buffers. The driver
    // sized them 800px*2B, 32B-aligned internal SRAM, and already pre-filled both
    // (bounce_pos_px=0) — the first flush lands in the idle one immediately.
    void *bb0 = nullptr, *bb1 = nullptr;
    if (esp_lcd_rgb_panel_get_frame_buffer(g_panel, 2, &bb0, &bb1) != ESP_OK || !bb0 || !bb1)
        return false;
    g_bb[0] = (uint8_t *)bb0; g_bb[1] = (uint8_t *)bb1;

    lv_draw_buf_handlers_t *handlers = lv_draw_buf_get_handlers();
    handlers->buf_malloc_cb = bbbuf_malloc; handlers->buf_free_cb = bbbuf_free;
    handlers->buf_copy_cb = bbbuf_copy; handlers->align_pointer_cb = bbbuf_align;
    handlers->invalidate_cache_cb = nullptr; handlers->flush_cache_cb = nullptr;
    handlers->width_to_stride_cb = bbbuf_stride;

    static bool lv_inited = false;
    if (!lv_inited) {
        lv_init();
        lv_inited = true;
    }
    lv_display_t *disp = lv_display_create(w, h);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    // PARTIAL mode onto the bounce pair: each buffer holds exactly ONE physical
    // line — bounce buffer k feeds physical lines k, k+2, … (line L at row L>>1,
    // bounce_pos_px steps one 800px line per GDMA EOF). LVGL renders one line
    // into the idle buffer; the ISR flips which buffer feeds which line.
    lv_display_set_buffers(disp, g_bb[0], g_bb[1], (size_t)800 * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, on_flush);
    _disp = disp;

    memset(g_bb[0], 0, 1600);   // black until the splash paints
    memset(g_bb[1], 0, 1600);
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

// ---- compat shim: LGFX drawing API straight into the bounce-buffer pair ----
// Physical line L lives in buffer L&1, row L>>1 (pixel offset (L>>1)*800): the
// shim paints the buffer NOT being scanned — plain 16-bit stores into internal
// SRAM through the DCache. No PSRAM, no cache-sync games, nothing to panic on.

void LVGLCanvas::px(int x, int y, uint16_t c) {
    if (unsigned(x) > _fbw - 1 || unsigned(y) > _fbh - 1) return;
    int dx = _flip ? _fbw - 1 - x : x;
    int dy = _flip ? _fbh - 1 - y : y;
    uint8_t *bb = g_bb[dy & 1];
    if (!bb) return;                       // ISR hasn't published buffers yet
    ((uint16_t *)bb)[(size_t)(dy >> 1) * _fbw + dx] = c;
    _dirty = true;
}
void LVGLCanvas::hline(int x0, int x1, int y, uint16_t c) {
    for (int x = x0; x <= x1; x++) px(x, y, c);
}
void LVGLCanvas::rect(int x, int y, int w, int h, uint16_t c) {
    for (int yy = y; yy < y + h; yy++) hline(x, x + w - 1, yy, c);
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
