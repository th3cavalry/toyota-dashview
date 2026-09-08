/* lv_conf.h — LVGL 9.5.0 configuration for the 4.3B.
 * S1: 16-bit color (RGB565), modest Montserrat font set, no logging.
 * Put next to lvgl or define LV_CONF_PATH to reach it. */

#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH          16
#define LV_COLOR_16_SWAP        0   /* RGB565 byte order comes from esp_lcd */

#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN
#define LV_MEM_SIZE             (48 * 1024U)
#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN

#define LV_DEF_REFR_PERIOD      33

#define LV_USE_LOG              0

#define LV_FONT_MONTSERRAT_8   1
#define LV_FONT_MONTSERRAT_14  1
#define LV_FONT_MONTSERRAT_20  1
#define LV_FONT_MONTSERRAT_28  1

#define LV_USE_LODEPNG          1
#define LV_USE_PNG              1

#define LV_USE_GPU              0
#define LV_USE_OS                LV_OS_NONE

#endif /* LV_CONF_H */
