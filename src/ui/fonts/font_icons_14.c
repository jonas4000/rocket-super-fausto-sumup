/*******************************************************************************
 * Size: 14 px
 * Bpp: 4
 * Opts: --bpp 4 --no-compress --format lvgl --lv-include lvgl.h --font assets/fonts/FontAwesome5-Solid+Brands+Regular.woff --size 14 --range 0xF2F2,0xF09B -o src/ui/fonts/font_icons_14.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef FONT_ICONS_14
#define FONT_ICONS_14 1
#endif

#if FONT_ICONS_14

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+F09B "" */
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x3, 0xae, 0xfe, 0xc6, 0x0, 0x0, 0x0, 0x9f,
    0xff, 0xff, 0xff, 0xd2, 0x0, 0x9, 0xfe, 0xff,
    0xff, 0xff, 0xee, 0x10, 0x3f, 0xf0, 0x37, 0x56,
    0x70, 0x6f, 0xb0, 0xaf, 0xf0, 0x0, 0x0, 0x0,
    0x7f, 0xf2, 0xef, 0xa0, 0x0, 0x0, 0x0, 0xf,
    0xf6, 0xff, 0x80, 0x0, 0x0, 0x0, 0xd, 0xf8,
    0xef, 0xb0, 0x0, 0x0, 0x0, 0xf, 0xf7, 0xbf,
    0xf5, 0x0, 0x0, 0x0, 0xaf, 0xf4, 0x6f, 0x6e,
    0xe4, 0x0, 0x9f, 0xff, 0xe0, 0xd, 0xd3, 0x60,
    0x0, 0x8f, 0xff, 0x60, 0x2, 0xee, 0xc1, 0x0,
    0x7f, 0xf9, 0x0, 0x0, 0x1a, 0xf1, 0x0, 0x7e,
    0x60, 0x0, 0x0, 0x0, 0x10, 0x0, 0x0, 0x0,
    0x0,

    /* U+F2F2 "" */
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xff, 0xff, 0x0, 0x0, 0x0, 0x0, 0xaf, 0xfa,
    0x0, 0x0, 0x0, 0x0, 0x3f, 0xf3, 0x0, 0x0,
    0x0, 0x3d, 0xff, 0xff, 0xd5, 0xd4, 0x4, 0xff,
    0xfd, 0xdf, 0xff, 0xf7, 0x1e, 0xff, 0xf1, 0x1f,
    0xff, 0xf0, 0x7f, 0xff, 0xf1, 0x1f, 0xff, 0xf6,
    0xaf, 0xff, 0xf1, 0x1f, 0xff, 0xfa, 0xbf, 0xff,
    0xf1, 0x1f, 0xff, 0xfb, 0x9f, 0xff, 0xf7, 0x7f,
    0xff, 0xf9, 0x4f, 0xff, 0xff, 0xff, 0xff, 0xf4,
    0xa, 0xff, 0xff, 0xff, 0xff, 0xa0, 0x0, 0xbf,
    0xff, 0xff, 0xfb, 0x0, 0x0, 0x5, 0xbe, 0xeb,
    0x50, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 217, .box_w = 14, .box_h = 15, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 105, .adv_w = 196, .box_w = 12, .box_h = 15, .ofs_x = 0, .ofs_y = -2}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_0[] = {
    0x0, 0x257
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 61595, .range_length = 600, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = 2, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LV_VERSION_CHECK(8, 0, 0)
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LV_VERSION_CHECK(8, 0, 0)
    .cache = &cache
#endif
};


/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LV_VERSION_CHECK(8, 0, 0)
const lv_font_t font_icons_14 = {
#else
lv_font_t font_icons_14 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 15,          /*The maximum line height required by the font*/
    .base_line = 2,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -5,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc           /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
};



#endif /*#if FONT_ICONS_14*/

