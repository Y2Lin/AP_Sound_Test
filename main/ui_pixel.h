#pragma once

#include "lvgl.h"

#define UI_SKY        0x1689E8
#define UI_SKY_DARK   0x0872C9
#define UI_INK        0x17202A
#define UI_PAPER      0xF4F4EA
#define UI_GRASS      0x82BE2D
#define UI_GRASS_DARK 0x55951D
#define UI_YELLOW     0xFFD928
#define UI_ORANGE     0xFFB23E
#define UI_RED        0xE43B2F
#define UI_MUTED      0xD9E7EC

lv_obj_t *ui_pixel_screen_create(const char *title);
// 像素风面板:墨色投影 + 纸色面板体(两个平级对象)。
// 常驻面板用 ui_pixel_panel_create;弹出式面板(要整体隐藏/显示的)
// 用 _ex 版本拿回阴影指针,隐藏时必须连阴影一起藏,否则留墨色残影。
lv_obj_t *ui_pixel_panel_create(lv_obj_t *parent, int x, int y, int w, int h,
                                uint32_t color);
lv_obj_t *ui_pixel_panel_create_ex(lv_obj_t *parent, int x, int y, int w, int h,
                                   uint32_t color, lv_obj_t **shadow_out);
lv_obj_t *ui_pixel_label(lv_obj_t *parent, const char *text,
                         const lv_font_t *font, uint32_t color);
lv_obj_t *ui_pixel_mascot_create(lv_obj_t *parent, int x, int y);
void ui_pixel_mascot_jump(lv_obj_t *mascot);
// 响度分区表情(0..4,对应 sound_meter_model.h 的 zone 枚举):
// 天线灯/脸/眼/嘴按分区换色,嘴随响度张大,偏响及以上上下浮动,告警时反白。
// 只在 zone/alarm 变化时调用(内部会管理浮动动画的启停)。
void ui_pixel_mascot_set_zone(lv_obj_t *mascot, int zone, bool alarm);
void ui_pixel_set_selected(lv_obj_t *panel, bool selected, bool enabled);
