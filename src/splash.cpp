// TRD boot splash — decodes the embedded PNG into the PSRAM framebuffer at boot.
// Uses LVGL 9's image decoder (lodepng backend registered by lv_lodepng_init).
//
// Strategy: register the lodepng decoder, then use lv_image_decoder_open to
// decode the PNG into a drawable buffer, and blit the pixels straight into the
// FB at (0,0). The splash is 800x480 — 1:1 with the FB.

#include "display.h"
#include <lvgl.h>
#include "toyota_splash.h"

bool drawToyotaBootSplash(LVGLCanvas& canvas) {
    // Register the lodepng decoder with LVGL so image ops can decode the PNG.
    lv_lodepng_init();

    // Build an image descriptor from the embedded PNG bytes.
    lv_image_dsc_t splash_dsc = {};
    splash_dsc.data_size = TRD_SPLASH_PNG_LEN;
    splash_dsc.data      = (const uint8_t *)trd_splash_png;

    // lv_image_decoder_dsc_t is opaque in the public API; reconstruct the struct
    // layout from lv_image_decoder_private.h to access decoded buffer.
    struct _lv_image_decoder_args_t {
        bool stride_align;
        bool premultiply;
        bool no_cache;
        bool use_indexed;
        bool flush_cache;
    };
    struct _lv_image_decoder_dsc_t {
        lv_image_decoder_t *decoder;
        struct _lv_image_decoder_args_t args;
        const void *src;
        lv_image_src_t src_type;
        lv_fs_file_t file;
        lv_image_header_t header;
        const lv_draw_buf_t *decoded;
        const lv_color32_t *palette;
        uint32_t palette_size;
        uint32_t time_to_open;
        const char *error_msg;
        lv_cache_t *cache;
        lv_cache_entry_t *cache_entry;
        void *user_data;
    };
    struct _lv_image_decoder_dsc_t *dec = (struct _lv_image_decoder_dsc_t *)malloc(sizeof(*dec));
    if (!dec) { lv_lodepng_deinit(); return false; }
    memset(dec, 0, sizeof(*dec));

    lv_result_t res = lv_image_decoder_open((lv_image_decoder_dsc_t *)dec, &splash_dsc, NULL);
    if (res != LV_RESULT_OK || !dec->decoded) {
        free(dec);
        lv_lodepng_deinit();
        return false;
    }

    // The decoder has decoded the image into dec->decoded (const lv_draw_buf_t *).
    const lv_draw_buf_t *buf = dec->decoded;

    // Walk the draw buffer struct fields directly.
    const lv_image_header_t *hdr = &buf->header;
    uint32_t w = hdr->w;
    uint32_t h = hdr->h;
    uint32_t stride = hdr->stride;
    const uint8_t *pixels = (const uint8_t *)buf->data;

    // Determine the decoded color format.
    lv_color_format_t cf = (lv_color_format_t)hdr->cf;

    if (cf == LV_COLOR_FORMAT_RGB565 || cf == LV_COLOR_FORMAT_RGB565_SWAPPED) {
        // Decoded directly to RGB565 — 2 bytes/px, copy row by row.
        const uint8_t *src = pixels;
        uint16_t *dst = canvas.fb();
        for (uint32_t y = 0; y < h && y < 480; y++) {
            memcpy(dst + y * 800, src + y * stride, 800 * 2);
        }
    } else {
        // Decoded as RGB888 (lv_color_t rows, 3 bytes/px). Convert to RGB565.
        const uint8_t *src = pixels;
        uint16_t *dst = canvas.fb();
        for (uint32_t y = 0; y < h && y < 480; y++) {
            const uint8_t *row = src + y * stride;
            for (uint32_t x = 0; x < w && x < 800; x++) {
                uint8_t r = row[x * 3 + 0];
                uint8_t g = row[x * 3 + 1];
                uint8_t b = row[x * 3 + 2];
                dst[x] = ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
            }
        }
    }

    lv_image_decoder_close((lv_image_decoder_dsc_t *)dec);
    free(dec);
    lv_lodepng_deinit();
    canvas.markDirty();
    return true;
}
