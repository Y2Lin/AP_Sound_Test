#include "ui_pixel.h"
#include "ui_pixel_math.h"

static void start_blink(lv_obj_t *eye);

static lv_obj_t *block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    return obj;
}

lv_obj_t *ui_pixel_label(lv_obj_t *parent, const char *text,
                         const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static void add_cloud(lv_obj_t *parent, int x, int y)
{
    block(parent, x + 1, y + 7, 43, 10, UI_INK);
    block(parent, x + 5, y + 4, 35, 10, 0xFFFFFF);
    block(parent, x + 12, y, 10, 9, 0xFFFFFF);
    block(parent, x + 27, y + 1, 9, 8, 0xFFFFFF);
}

lv_obj_t *ui_pixel_screen_create(const char *title)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_SKY), 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    add_cloud(scr, 188, 8);
    /* 底部不再放大块草地装饰(用户反馈"大色块");改为 2px 细线收尾。 */

    block(scr, 9, 12, 151, 33, UI_INK);
    lv_obj_t *plate = block(scr, 5, 8, 151, 33, UI_PAPER);
    lv_obj_set_style_border_color(plate, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_border_width(plate, 3, 0);
    lv_obj_t *heading = ui_pixel_label(plate, title, &lv_font_montserrat_20, UI_INK);
    lv_obj_center(heading);
    return scr;
}

lv_obj_t *ui_pixel_panel_create(lv_obj_t *parent, int x, int y, int w, int h,
                                uint32_t color)
{
    block(parent, x + 5, y + 6, w, h, UI_INK);
    lv_obj_t *panel = block(parent, x, y, w, h, color);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_border_width(panel, 4, 0);
    lv_obj_set_style_pad_all(panel, 7, 0);
    return panel;
}

// 吉祥物需要动态换色的部件(分区表情用)。挂在容器 user_data 上;
// 应用同时只有一个吉祥物,用静态存储,不引入堆分配与释放问题。
typedef struct {
    lv_obj_t *antenna;   // 天线灯(顶部小色块)
    lv_obj_t *face;      // 脸部屏幕底
    lv_obj_t *eye_l;     // 左眼
    lv_obj_t *eye_r;     // 右眼
    lv_obj_t *mouth;     // 嘴(位置/大小按分区变化)
    int       base_y;    // 创建时的锚定 y:所有位移动画以它为基准
} mascot_parts_t;

static mascot_parts_t s_parts;

lv_obj_t *ui_pixel_mascot_create(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *m = lv_obj_create(parent);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(m, x, y);
    lv_obj_set_size(m, 38, 48);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);

    /* 原创“小电视机器人”：天线、发光屏幕脸、橙色围巾与履带脚。 */
    block(m, 18, 0, 3, 6, UI_INK);
    s_parts.antenna = block(m, 16, 0, 7, 3, UI_ORANGE);
    block(m, 3, 6, 32, 24, UI_INK);
    block(m, 0, 12, 5, 10, 0x7557D9);
    block(m, 33, 12, 5, 10, 0x7557D9);
    s_parts.face = block(m, 7, 10, 24, 16, 0xB9F3FF);
    s_parts.eye_l = block(m, 11, 14, 4, 6, 0x294B7A);
    s_parts.eye_r = block(m, 23, 14, 4, 6, 0x294B7A);
    s_parts.mouth = block(m, 16, 22, 7, 2, 0x7557D9);
    block(m, 10, 29, 18, 4, UI_ORANGE);
    block(m, 8, 33, 22, 11, 0x7557D9);
    block(m, 3, 35, 5, 7, 0xB9F3FF);
    block(m, 30, 35, 5, 7, 0xB9F3FF);
    block(m, 8, 44, 9, 4, UI_INK);
    block(m, 21, 44, 9, 4, UI_INK);
    start_blink(s_parts.eye_l);
    start_blink(s_parts.eye_r);
    s_parts.base_y = y;             // 位移基准:动画必须回到这里,不得漂移
    lv_obj_set_user_data(m, &s_parts);
    return m;
}

static void jump_y(void *obj, int32_t value)
{
    lv_obj_set_y((lv_obj_t *)obj, value);
}

static void blink_eye(void *obj, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void start_blink(lv_obj_t *eye)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, eye);
    lv_anim_set_exec_cb(&anim, blink_eye);
    lv_anim_set_values(&anim, LV_OPA_COVER, LV_OPA_20);
    lv_anim_set_duration(&anim, 70);
    lv_anim_set_playback_duration(&anim, 70);
    lv_anim_set_repeat_delay(&anim, 1700);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&anim, lv_anim_path_step);
    lv_anim_start(&anim);
}

void ui_pixel_mascot_jump(lv_obj_t *mascot)
{
    if (!mascot) return;
    /* 起点固定为创建时的锚定 y(而非"当前 y"):跳跃常与浮动动画并存,
     * 若以当前 y 为起点,动画在半途被打断重启会逐步累积位移。
     * 以 base_y 为基准则每次跳跃都回到同一位置,绝不漂移。 */
    mascot_parts_t *p = lv_obj_get_user_data(mascot);
    int y = p ? p->base_y : lv_obj_get_y(mascot);
    lv_anim_delete(mascot, jump_y);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, mascot);
    lv_anim_set_exec_cb(&anim, jump_y);
    lv_anim_set_values(&anim, y, y - 5);
    lv_anim_set_duration(&anim, 110);
    lv_anim_set_playback_duration(&anim, 140);
    lv_anim_set_path_cb(&anim, lv_anim_path_step);
    lv_anim_start(&anim);
}

// 上下浮动(“越响越闹”)。exec 回调与 jump_y 分开,lv_anim_delete 才能
// 只停浮动不停跳跃,两种动画互不干扰。
static void bob_y(void *obj, int32_t value)
{
    lv_obj_set_y((lv_obj_t *)obj, value);
}

void ui_pixel_mascot_set_zone(lv_obj_t *mascot, int zone, bool alarm)
{
    if (!mascot) return;
    const ui_pixel_mascot_style_t *st = ui_pixel_mascot_style(zone, alarm);
    mascot_parts_t *p = lv_obj_get_user_data(mascot);
    if (!p) return;

    lv_obj_set_style_bg_color(p->antenna, lv_color_hex(st->antenna), 0);
    lv_obj_set_style_bg_color(p->face, lv_color_hex(st->face), 0);
    lv_obj_set_style_bg_color(p->eye_l, lv_color_hex(st->eye), 0);
    lv_obj_set_style_bg_color(p->eye_r, lv_color_hex(st->eye), 0);
    lv_obj_set_style_bg_color(p->mouth, lv_color_hex(st->mouth), 0);

    // 嘴:宽度/高度随响度张大,水平居中在脸中心(x=19),纵向以 y=23 为底。
    lv_obj_set_size(p->mouth, st->mouth_w, st->mouth_h);
    lv_obj_set_pos(p->mouth, 19 - st->mouth_w / 2, 23 - (st->mouth_h + 1) / 2);

    // 浮动:停掉旧的,按新幅度重启;幅度 0(安静/正常)= 静止。
    // 区间固定为 [base_y-bob_px, base_y]:此前以"当前 y"为起点,音量在分区
    // 边界抖动会反复重建动画,起点被半途值顶替,区间不断上移--机器人
    // 最终漂出屏幕顶。锚定 base_y 后区间恒定,重建多少次都不漂移。
    lv_anim_delete(mascot, bob_y);
    if (st->bob_px > 0 && st->bob_ms > 0) {
        int y = p->base_y;
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, mascot);
        lv_anim_set_exec_cb(&anim, bob_y);
        lv_anim_set_values(&anim, y, y - st->bob_px);
        lv_anim_set_duration(&anim, st->bob_ms);
        lv_anim_set_playback_duration(&anim, st->bob_ms);
        lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
        lv_anim_start(&anim);
    }
}

void ui_pixel_set_selected(lv_obj_t *panel, bool selected, bool enabled)
{
    uint32_t color = !enabled ? 0x78909C : (selected ? UI_YELLOW : UI_PAPER);
    lv_obj_set_style_bg_color(panel, lv_color_hex(color), 0);
    lv_obj_set_style_border_color(panel,
        lv_color_hex(selected ? 0xFFFFFF : UI_INK), 0);
}
