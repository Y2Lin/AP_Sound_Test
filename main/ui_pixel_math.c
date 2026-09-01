#include "ui_pixel_math.h"

int ui_pixel_blink_frame(uint32_t elapsed_ms)
{
    uint32_t phase = elapsed_ms % 2000U;
    return phase >= 1650U && phase < 1800U;
}

int ui_pixel_jump_offset(unsigned frame)
{
    static const int offsets[] = { 0, -3, -5, -3, 0 };
    return frame < sizeof(offsets) / sizeof(offsets[0]) ? offsets[frame] : 0;
}

int ui_pixel_battery_level(int soc)
{
    if (soc < 0)  return -1;   // 读取失败:未知
    if (soc < 20) return 0;    // 低:红
    if (soc <= 50) return 1;   // 中:黄
    return 2;                  // 高:绿
}

int ui_pixel_battery_fill_w(int soc, int max_w)
{
    if (soc < 0 || max_w <= 0) return 0;
    if (soc > 100) soc = 100;
    return (soc * max_w) / 100;
}

int ui_pixel_battery_display_soc(int soc, int mv)
{
    if (soc >= 99 && mv >= UI_BATT_FULL_MV) return 100;
    return soc;
}

// 分区表情表:下标对应 sound_meter_model.h 的 zone 枚举(0..4)。
// 设计:天线灯=分区亮色(蓝/绿/黄/橙/红),脸=同色系浅底,眼/嘴=同色系深色;
// 嘴随响度张大,浮动幅度(越响越闹)同步加码,安静/正常保持静止。
static const ui_pixel_mascot_style_t MASCOT_STYLE[5] = {
    {   // QUIET:瞌睡,浅蓝灰脸 + 小嘴,不动
        .antenna = 0x4FC3F7, .face = 0xCFE9F5, .eye = 0x37474F,
        .mouth   = 0x90A4AE, .mouth_w = 5, .mouth_h = 2,
        .bob_px  = 0, .bob_ms = 0,
    },
    {   // NORMAL:默认脸(青蓝),常规表情,不动
        .antenna = 0x82BE2D, .face = 0xB9F3FF, .eye = 0x294B7A,
        .mouth   = 0x7557D9, .mouth_w = 7, .mouth_h = 2,
        .bob_px  = 0, .bob_ms = 0,
    },
    {   // MODERATE:浅黄脸,嘴张开,轻微浮动
        .antenna = 0xFFD54F, .face = 0xFFF3B0, .eye = 0x8D6E00,
        .mouth   = 0xFF8F00, .mouth_w = 9, .mouth_h = 3,
        .bob_px  = 2, .bob_ms = 1100,
    },
    {   // LOUD:浅橙脸,大嘴,浮动明显
        .antenna = 0xFF9838, .face = 0xFFE0B2, .eye = 0xE65100,
        .mouth   = 0xE65100, .mouth_w = 11, .mouth_h = 4,
        .bob_px  = 3, .bob_ms = 600,
    },
    {   // EXTREME:浅红脸,最大嘴,剧烈浮动
        .antenna = 0xE43B2F, .face = 0xFFCDD2, .eye = 0xB3261E,
        .mouth   = 0xB3261E, .mouth_w = 13, .mouth_h = 5,
        .bob_px  = 5, .bob_ms = 350,
    },
};

// 告警覆盖样式:整屏已变红,机器人反白(白脸红眼)保持可见。
static const ui_pixel_mascot_style_t MASCOT_STYLE_ALARM = {
    .antenna = 0xE43B2F, .face = 0xFFFFFF, .eye = 0xB3261E,
    .mouth   = 0xB3261E, .mouth_w = 13, .mouth_h = 5,
    .bob_px  = 5, .bob_ms = 350,
};

const ui_pixel_mascot_style_t *ui_pixel_mascot_style(int zone, bool alarm)
{
    if (alarm) return &MASCOT_STYLE_ALARM;
    if (zone < 0) zone = 0;
    if (zone > 4) zone = 4;
    return &MASCOT_STYLE[zone];
}
