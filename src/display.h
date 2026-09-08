# pragma once
#include <lvgl.h>
#include <Arduino.h>
#include "fonts.h"

#define TFT_BLACK  0x0000
#define TFT_WHITE  0xFFFF

#define TFT_COLOR565(r, g, b) (((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)

class LVGLCanvas {
public:
    bool init(uint16_t w, uint16_t h, uint32_t pclk_hz);
    void flush();
    bool dirty() const { return _dirty; }
    void markDirty() { _dirty = true; }
    bool flip() const { return _flip; }

    int  width() const { return _w; }
    int  height() const { return _h; }
    uint16_t color565(uint8_t r, uint8_t g, uint8_t b) const;
    void setTextColor(uint16_t fg);
    void setTextColor(uint16_t fg, uint16_t);
    void setFont(const lv_font_t* f);
    void setTextDatum(uint8_t datum);
    void setTextPadding(uint16_t pad);
    void fillScreen(uint16_t c);
    void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c);
    void drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c);
    void drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c);
    void drawFastHLine(int32_t x, int32_t y, int32_t l, uint16_t c);
    void drawFastVLine(int32_t x, int32_t y, int32_t h, uint16_t c);
    void fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c);
    void drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c);
    void fillCircle(int32_t x, int32_t y, int32_t r, uint16_t c);
    void drawCircle(int32_t x, int32_t y, int32_t r, uint16_t c);
    void fillTriangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                      int32_t x2, int32_t y2, uint16_t c);
    void drawString(const String& s, int32_t x, int32_t y);
    void drawString(const String& s, int32_t x, int32_t y, uint16_t c);
    void drawCenterString(const String& s, int32_t x, int32_t y);
    void drawCenterString(const String& s, int32_t x, int32_t y, uint16_t c);
    void drawRightString(const String& s, int32_t x, int32_t y);
    void drawRightString(const String& s, int32_t x, int32_t y, uint16_t c);
    void setRotation(uint8_t r);
    bool drawPng(const uint8_t* png, uint32_t len, int32_t x, int32_t y);

    lv_display_t* display() const { return _disp; }
    uint16_t*     fb() const { return (uint16_t*)_fb; }

private:
    void px(int x, int y, uint16_t c);
    void hline(int x0, int x1, int y, uint16_t c);
    void rect(int x, int y, int w, int h, uint16_t c);
    void glyph(const lv_font_t* f, uint32_t cp, int32_t& x, int32_t y);
    int32_t  textW(const String& s) const;
    int32_t  textH() const;
    void     invalidateRect(int32_t x, int32_t y, int32_t w, int32_t h);
    uint8_t* _fb = nullptr;
    lv_display_t* _disp = nullptr;
    const lv_font_t* _font = nullptr;
    uint16_t _fg = 0xFFFF;
    uint8_t  _datum = 0;
    uint16_t _pad = 0;
    bool     _dirty = false;
    bool     _flip = false;
    uint16_t _w = 0, _h = 0;
    uint16_t _fbw = 0, _fbh = 0;
};

extern LVGLCanvas canvas;

void displayBacklightOn();
void displayBacklightOff();
bool displayBacklightEnabled();
void displayFlushNow();
void displayTouchInit();
bool displayTouchRead(int& x, int& y);
