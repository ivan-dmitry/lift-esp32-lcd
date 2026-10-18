#!/usr/bin/env bash
# Generate the LVGL fonts for the elevator display.
#   bash tools/build_fonts.sh
# Needs Node.js (npx downloads lv_font_conv on first run).
set -e
cd "$(dirname "$0")/.."
TTF=tools/ttf
OUT=src/fonts
mkdir -p "$OUT"

gen () { # name ttf size ranges
  echo ">> $1  ($3 px)"
  npx --yes lv_font_conv \
      --font "$TTF/$2" --size "$3" --bpp 4 --no-compress \
      --format lvgl --lv-include lvgl.h \
      -o "$OUT/$1.c" $4
}

# UI text: printable ASCII plus the few typographic marks the UI uses
#   0xB0 °   0xB7 ·   0x2013 –   0x2014 —   0x2022 •
TXT="-r 0x20-0x7E -r 0xB0 -r 0xB7 -r 0x2013 -r 0x2014 -r 0x2022"
gen ui12  BeVietnamPro-Regular.ttf  12 "$TXT"
gen ui14  BeVietnamPro-Regular.ttf  14 "$TXT"
gen ui14b BeVietnamPro-Bold.ttf     14 "$TXT"
gen ui22b BeVietnamPro-Bold.ttf     22 "$TXT"
# Splash title only: upper case, digits, basic punctuation
gen ui42b BeVietnamPro-Bold.ttf     42 "-r 0x20-0x5A"
# Clock on the main screen — digits as tall as the 28 px icons beside it
gen ui32b BeVietnamPro-Bold.ttf     32 "-r 0x20-0x5A -r 0xB0"

# ── Giant floor numbers ──
# Floor labels are digits, upper-case letters and '-' only ("B2", "12A", "G",
# "--" when the link is lost). Building just those 38 glyphs keeps each size
# small. Two sizes for the full-screen indicator: 220 px for 1–2 characters,
# 140 px for 3 characters and for the incident layout — three characters at
# 220 px are ~460 px wide and do not fit.
FLOOR="-r 0x20 -r 0x2D -r 0x30-0x39 -r 0x3F -r 0x41-0x5A"
# The two giant sizes are built COMPRESSED (RLE, LV_USE_FONT_COMPRESSED 1): they are
# mostly solid areas and shrink to about half — needed to fit two OTA slots in 4 MB.
genz () { echo ">> $1  ($3 px, compressed)"; npx --yes lv_font_conv --font "$TTF/$2" --size "$3" --bpp 4 --format lvgl --lv-include lvgl.h -o "$OUT/$1.c" $4; }
genz fl220 BeVietnamPro-Bold.ttf 220 "$FLOOR"
genz fl140 BeVietnamPro-Bold.ttf 140 "$FLOOR"

# ── Icons: Material Icons (Apache 2.0), only the glyphs actually used ──
# elevator lan tune layers search wifi home display_settings volume_up history
# info lock warning check_circle local_fire_department build monitor_weight
# block door_sliding arrow_upward arrow_downward edit delete close done cloud
# cloud_off wifi_off link link_off dialpad schedule power_settings_new
# arrow_back add touch_app menu
# weather: sunny nights_stay wb_cloudy foggy water_drop umbrella ac_unit thunderstorm
ICO="-r 0xF1A0 -r 0xEB2F -r 0xE429 -r 0xE53B -r 0xE8B6 -r 0xE63E -r 0xE88A \
     -r 0xEB97 -r 0xE050 -r 0xE889 -r 0xE88E -r 0xE897 -r 0xE002 -r 0xE86C \
     -r 0xEF55 -r 0xE869 -r 0xF039 -r 0xE14B -r 0xEFFE -r 0xE5D8 -r 0xE5DB \
     -r 0xE3C9 -r 0xE872 -r 0xE5CD -r 0xE876 -r 0xE2BD -r 0xE2C1 -r 0xE648 \
     -r 0xE157 -r 0xE16F -r 0xE0BC -r 0xE8B5 -r 0xE8AC -r 0xE5C4 -r 0xE145 \
     -r 0xE913 -r 0xE5D2 \
     -r 0xE81A -r 0xEA46 -r 0xE42D -r 0xE818 -r 0xE798 -r 0xF1AD -r 0xEB3B -r 0xEBDB"
# Large set: splash logo, incident banner icons, and the CALL / LOG / MENU
# buttons of the full-screen indicator (dialpad, history, menu)
ICOBIG="-r 0xF1A0 -r 0xEF55 -r 0xE002 -r 0xE869 -r 0xF039 -r 0xE14B \
        -r 0xEFFE -r 0xE16F -r 0xE86C -r 0xE0BC -r 0xE889 -r 0xE5D2 \
        -r 0xE5D8 -r 0xE5DB"

gen ico28 MaterialIcons-Regular.ttf 28 "$ICO"
gen ico48 MaterialIcons-Regular.ttf 48 "$ICOBIG"

echo "--- sizes ---"
ls -l "$OUT"
