#include "fonts.h"

namespace fonts {
    const lv_font_t *Font0 = nullptr;
    const lv_font_t *Font2 = nullptr;
    const lv_font_t *Font4 = nullptr;
    const lv_font_t *Font7 = nullptr;
}

void fontsInit() {
    fonts::Font0 = &lv_font_montserrat_8;
    fonts::Font2 = &lv_font_montserrat_14;
    fonts::Font4 = &lv_font_montserrat_20;
    fonts::Font7 = &lv_font_montserrat_28;
}
