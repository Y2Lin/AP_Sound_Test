#include <assert.h>
#include "ui_pixel_math.h"

// 电量级别边界:19 低 / 20-50 中 / 51 高;读不到为未知。
static void test_battery_level(void)
{
    assert(ui_pixel_battery_level(-1) == -1);
    assert(ui_pixel_battery_level(0) == 0);
    assert(ui_pixel_battery_level(19) == 0);
    assert(ui_pixel_battery_level(20) == 1);
    assert(ui_pixel_battery_level(50) == 1);
    assert(ui_pixel_battery_level(51) == 2);
    assert(ui_pixel_battery_level(100) == 2);
}

// 填充宽度:soc 钳到 0..100 后按比例换算;无效输入 0。
static void test_battery_fill_w(void)
{
    assert(ui_pixel_battery_fill_w(-1, 27) == 0);
    assert(ui_pixel_battery_fill_w(0, 27) == 0);
    assert(ui_pixel_battery_fill_w(50, 27) == 13);
    assert(ui_pixel_battery_fill_w(100, 27) == 27);
    assert(ui_pixel_battery_fill_w(120, 27) == 27);   // 钳位
    assert(ui_pixel_battery_fill_w(80, 0) == 0);      // 无画布
    assert(ui_pixel_battery_fill_w(80, -5) == 0);
}

// 满充显示校正:通用曲线满充停在 99%,电压足够高时按 100 显示;
// 电压不足、soc 不足、读取失败(-1)一律不校正。
static void test_battery_display_soc(void)
{
    assert(ui_pixel_battery_display_soc(99, 4200) == 100);
    assert(ui_pixel_battery_display_soc(99, UI_BATT_FULL_MV) == 100);        // 边界
    assert(ui_pixel_battery_display_soc(99, UI_BATT_FULL_MV - 1) == 99);     // 差 1mV
    assert(ui_pixel_battery_display_soc(99, 4100) == 99);                    // 电压不足
    assert(ui_pixel_battery_display_soc(100, 4100) == 100);                  // 本来就满
    assert(ui_pixel_battery_display_soc(98, 4200) == 98);                    // soc 不足
    assert(ui_pixel_battery_display_soc(50, 4200) == 50);
    assert(ui_pixel_battery_display_soc(99, -1) == 99);                      // 电压读失败
    assert(ui_pixel_battery_display_soc(-1, 4200) == -1);                    // 电量读失败
    assert(ui_pixel_battery_display_soc(-1, -1) == -1);
}

// 吉祥物样式:分区单调(嘴越来越大、浮动越来越闹),
// 安静/正常静止;告警覆盖为白脸红眼;zone 越界钳位。
static void test_mascot_style(void)
{
    const ui_pixel_mascot_style_t *s[5];
    for (int z = 0; z < 5; z++) s[z] = ui_pixel_mascot_style(z, false);

    for (int z = 1; z < 5; z++) {
        assert(s[z]->mouth_w >= s[z - 1]->mouth_w);
        assert(s[z]->bob_px >= s[z - 1]->bob_px);
    }
    assert(s[0]->bob_px == 0 && s[0]->bob_ms == 0);   // 安静:静止
    assert(s[1]->bob_px == 0 && s[1]->bob_ms == 0);   // 正常:静止
    assert(s[4]->bob_px > 0 && s[4]->bob_ms > 0);     // 极响:闹
    assert(s[4]->mouth_w > s[0]->mouth_w);
    // 各分区天线灯颜色互不相同(分区指示)。
    for (int a = 0; a < 5; a++)
        for (int b = a + 1; b < 5; b++)
            assert(s[a]->antenna != s[b]->antenna);

    // 告警覆盖:白脸红眼,与任何 zone 样式都不同。
    const ui_pixel_mascot_style_t *alm = ui_pixel_mascot_style(0, true);
    assert(alm->face == 0xFFFFFF);
    assert(alm->eye == 0xB3261E);
    assert(alm->bob_px > 0);
    assert(alm == ui_pixel_mascot_style(4, true));    // 告警与 zone 无关

    // zone 越界:负数按 0,超界按 4。
    assert(ui_pixel_mascot_style(-3, false) == s[0]);
    assert(ui_pixel_mascot_style(9, false) == s[4]);
}

int main(void)
{
    assert(ui_pixel_blink_frame(0) == 0);
    assert(ui_pixel_blink_frame(1700) == 1);
    assert(ui_pixel_blink_frame(1850) == 0);

    assert(ui_pixel_jump_offset(0) == 0);
    assert(ui_pixel_jump_offset(1) == -3);
    assert(ui_pixel_jump_offset(2) == -5);
    assert(ui_pixel_jump_offset(3) == -3);
    assert(ui_pixel_jump_offset(4) == 0);
    assert(ui_pixel_jump_offset(99) == 0);

    test_battery_level();
    test_battery_fill_w();
    test_battery_display_soc();
    test_mascot_style();
    return 0;
}
