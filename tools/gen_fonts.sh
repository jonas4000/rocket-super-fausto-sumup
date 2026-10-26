#!/bin/sh
# Regenerates the custom LVGL fonts in src/ui/fonts/ from assets/fonts/.
# The built-in Montserrat fonts have no bold, "€" or "·", and only a fixed icon
# set, so the UI uses these alongside them. Needs Node (npx).
# Run: sh tools/gen_fonts.sh
set -e
cd "$(dirname "$0")/.."
OUT=src/ui/fonts
mkdir -p "$OUT"
# --no-compress: LVGL 9 has compressed-font support off by default (boxes instead
# of glyphs), and uncompressed glyphs are faster to draw on the ESP32.
conv() { npx --yes lv_font_conv@1.5.2 --bpp 4 --no-compress --format lvgl --lv-include lvgl.h "$@"; }
TEXT="0x20-0x7E,0x20AC,0xB7"  # ASCII, €, ·
BOLD=assets/fonts/Montserrat-Bold.ttf
MEDIUM=assets/fonts/Montserrat-Medium.ttf
ICONS=assets/fonts/FontAwesome5-Solid+Brands+Regular.woff
PIXEL=assets/fonts/PressStart2P-Regular.ttf  # OFL, see PressStart2P-OFL.txt

# Payment price "€0.90"
conv --font $BOLD --size 40 --symbols "€0123456789.,%" -o "$OUT/font_bold_40.c"
# Card names/prices, "Enjoy!", grind percentage
conv --font $BOLD --size 24 --range $TEXT -o "$OUT/font_bold_24.c"
# Titles, settings values, PIN keys
conv --font $BOLD --size 20 --range $TEXT -o "$OUT/font_bold_20.c"
# Settings stepper values, stats
conv --font $BOLD --size 16 --range $TEXT -o "$OUT/font_bold_16.c"
# Body text that needs "€" or "·" (e.g. "Paid €0.90 · Double shot")
conv --font $MEDIUM --size 14 --range $TEXT -o "$OUT/font_text_14.c"
# Icons (Font Awesome 5): coffee F0F4, info-circle F05A, users F0C0, chart-bar F080
conv --font $ICONS --size 20 --range 0xF0F4,0xF05A,0xF0C0,0xF080 -o "$OUT/font_icons_20.c"
# Stopwatch F2F2, github F09B (Brands)
conv --font $ICONS --size 14 --range 0xF2F2,0xF09B -o "$OUT/font_icons_14.c"
# Pixel font: screensaver buttons and Flappy Beans (uppercase only, which is
# all the game's keyboard types). 1 bpp at a multiple of its 8 px grid: crisp
# pixels, no anti-aliasing.
PIXEL_RANGE="0x20-0x5F,0x20AC"  # space, digits, punctuation, A-Z, €
for size in 8 16; do
  npx --yes lv_font_conv@1.5.2 --bpp 1 --no-compress --format lvgl --lv-include lvgl.h \
    --font $PIXEL --size $size --range $PIXEL_RANGE -o "$OUT/font_pixel_$size.c"
done
echo "fonts written to $OUT"
