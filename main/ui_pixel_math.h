#pragma once

#include <stdbool.h>
#include <stdint.h>

// 眨眼判定:2000ms 周期里 1650..1799ms 闭眼(约 7.5% 时间)。
int ui_pixel_blink_frame(uint32_t elapsed_ms);

// 跳跃动画的 y 偏移表(5 帧一个来回),越界返回 0。
int ui_pixel_jump_offset(unsigned frame);

// ---------------------------------------------------------------------------
// 电池图标(纯逻辑,C3 与主机共用的映射;配色由调用方按级别取)。
// ---------------------------------------------------------------------------
// 电量级别:0=低(红)、1=中(黄)、2=高(绿);soc 读失败(-1)返回 -1(灰,未知)。
// 边界:>50 高,20..50 中,<20 低。
int ui_pixel_battery_level(int soc);

// 填充宽度:soc% 映射到 0..max_w 像素;soc<0 或 max_w<=0 返回 0。
int ui_pixel_battery_fill_w(int soc, int max_w);

// ---------------------------------------------------------------------------
// 吉祥物表情样式:按响度分区(见 sound_meter_model.h 的 zone 枚举 0..4)
// 换脸/眼/嘴颜色与嘴型,并用上下浮动幅度表达"越响越闹"。alarm 态覆盖为
// 白脸红眼(在整屏变红的告警配色下保持可见)。
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t antenna;   // 天线灯颜色(分区亮色,起指示灯作用)
    uint32_t face;      // 脸部屏幕底色(分区浅色,深色眼/嘴放上面)
    uint32_t eye;       // 眼睛颜色(分区深色)
    uint32_t mouth;     // 嘴巴颜色
    int      mouth_w;   // 嘴宽 px(安静小嘴,极响大嘴),居中放置
    int      mouth_h;   // 嘴高 px
    int      bob_px;    // 上下浮动幅度 px;0=静止(安静/正常)
    int      bob_ms;    // 浮动单程时长 ms(幅度越大周期越短)
} ui_pixel_mascot_style_t;

// zone 越界按 0/4 钳位;alarm=true 时返回告警覆盖样式(与 zone 无关)。
const ui_pixel_mascot_style_t *ui_pixel_mascot_style(int zone, bool alarm);
