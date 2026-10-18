// Shared colours and fonts — same tokens as esp32Fire (ui/index.html)
#pragma once
#include <lvgl.h>

LV_FONT_DECLARE(ui12);    // dim text, status bar
LV_FONT_DECLARE(ui14);    // body text
LV_FONT_DECLARE(ui14b);   // button labels, main list lines
LV_FONT_DECLARE(ui22b);   // headings, floor numbers on call buttons
LV_FONT_DECLARE(ui42b);   // splash title, incident banner, main-screen clock + temperature (upper case, digits, °)
LV_FONT_DECLARE(ui32b);   // upper case + digits + °
LV_FONT_DECLARE(icowx);   // weather icons, 36 px (main-screen top row)
LV_FONT_DECLARE(fl220);   // giant floor number, 1–2 character labels (168 px line)
LV_FONT_DECLARE(fl140);   // floor number: 3 character labels, incident layout (107 px line)
LV_FONT_DECLARE(ico28);   // Material icons, normal size
LV_FONT_DECLARE(ico48);   // Material icons, large

// The text fonts carry no symbol glyphs, so icons are a SEPARATE font — an icon
// and its text must be two separate labels. Written as UTF-8 escapes for review.
#define IC_ELEVATOR  "\xEF\x86\xA0"   // elevator
#define IC_CAN       "\xEE\xAC\xAF"   // lan            — CAN bus
#define IC_TUNE      "\xEE\x90\xA9"   // tune           — protocol map
#define IC_LAYERS    "\xEE\x94\xBB"   // layers         — floors
#define IC_SEARCH    "\xEE\xA2\xB6"   // search         — raw frames
#define IC_WIFI      "\xEE\x98\xBE"
#define IC_HOME      "\xEE\xA2\x8A"   // home           — Home Assistant
#define IC_DISPLAY   "\xEE\xAE\x97"
#define IC_VOLUME    "\xEE\x81\x90"
#define IC_HISTORY   "\xEE\xA2\x89"
#define IC_INFO      "\xEE\xA2\x8E"
#define IC_LOCK      "\xEE\xA2\x97"
#define IC_WARNING   "\xEE\x80\x82"
#define IC_CHECK     "\xEE\xA1\xAC"
#define IC_FIRE      "\xEE\xBD\x95"
#define IC_BUILD     "\xEE\xA1\xA9"   // inspection
#define IC_WEIGHT    "\xEF\x80\xB9"   // overload
#define IC_BLOCK     "\xEE\x85\x8B"   // out of service
#define IC_DOOR      "\xEE\xBF\xBE"   // door_sliding
#define IC_UP        "\xEE\x97\x98"
#define IC_DOWN      "\xEE\x97\x9B"
#define IC_EDIT      "\xEE\x8F\x89"
#define IC_DELETE    "\xEE\xA1\xB2"
#define IC_CLOSE     "\xEE\x97\x8D"
#define IC_DONE      "\xEE\xA1\xB6"
#define IC_CLOUD     "\xEE\x8A\xBD"
// Weather (ico28): sunny, nights_stay, wb_cloudy, foggy, water_drop, umbrella, ac_unit, thunderstorm
#define IC_WX_SUN     "\xEE\xA0\x9A"
#define IC_WX_NIGHT   "\xEE\xA9\x86"
#define IC_WX_PARTLY  "\xEE\x90\xAD"
#define IC_WX_FOG     "\xEE\xA0\x98"
#define IC_WX_DRIZZLE "\xEE\x9E\x98"
#define IC_WX_RAIN    "\xEF\x86\xAD"
#define IC_WX_SNOW    "\xEE\xAC\xBB"
#define IC_WX_STORM   "\xEE\xAF\x9B"
#define IC_CLOUD_OFF "\xEE\x8B\x81"
#define IC_WIFI_OFF  "\xEE\x99\x88"
#define IC_LINK      "\xEE\x85\x97"
#define IC_LINK_OFF  "\xEE\x85\xAF"
#define IC_DIALPAD   "\xEE\x82\xBC"
#define IC_CLOCK     "\xEE\xA2\xB5"
#define IC_POWER     "\xEE\xA2\xAC"
#define IC_BACK      "\xEE\x97\x84"
#define IC_ADD       "\xEE\x85\x85"
#define IC_TOUCH     "\xEE\xA4\x93"
#define IC_MENU      "\xEE\x97\x92"   // menu (hamburger)

#define C_BG        lv_color_hex(0x0A0D12)
#define C_SURF      lv_color_hex(0x141A23)
#define C_SURF2     lv_color_hex(0x1C2531)
#define C_LINE      lv_color_hex(0x293547)
#define C_TXT       lv_color_hex(0xE9EEF6)
#define C_DIM       lv_color_hex(0x8695AB)
#define C_ALARM     lv_color_hex(0xE11D22)
#define C_ALARM_LO  lv_color_hex(0x2E070A)
#define C_ALARM_HI  lv_color_hex(0xE5161D)
#define C_FAULT     lv_color_hex(0xF0A500)
#define C_FAULT_BG  lv_color_hex(0x2B1F03)
#define C_OK        lv_color_hex(0x16A34A)
#define C_OK_BG     lv_color_hex(0x07200F)
#define C_ISO       lv_color_hex(0x3B82F6)
#define C_ISO_BG    lv_color_hex(0x0A1A33)
#define C_ARROW     lv_color_hex(0x22D3EE)   // accents (simulation, announcements)
#define C_GOLD      lv_color_hex(0xE8A200)   // main screen: floor number, arrows, status text (deep gold)
#define C_WHITE     lv_color_hex(0xFFFFFF)
#define C_BLACK     lv_color_hex(0x000000)

// Fixed grid for every screen (same grid as esp32Fire)
#define H_STATUSBAR  28
#define H_BODY      236
#define H_FOOTER     56
#define TOUCH_MIN    44   // smallest touch target for resistive touch
