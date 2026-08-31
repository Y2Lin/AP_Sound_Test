// main/sound_meter_model.h -- 周围音量检测的纯逻辑模型。
//
// 本模块只依赖 <stdint.h>/<stdbool.h>,不包含任何 ESP-IDF/LVGL 头,
// 因此可以在主机上用 gcc 直接编译并跑 assert 测试(见 tests/test_sound_meter_model.c)。
// 页面与采集任务(main/demo_sound_meter.c)只负责:I/O、队列、UI 与持久化。
//
// 计量约定:
//   - 输入 rms 是 16bit 单声道采样的 RMS(0..32767);
//   - dbfs_x10 = 200*log10(rms/32768),即 dBFS*10,0 表示满幅;
//   - display_db_x10 = dbfs_x10 + SOUND_METER_DB_OFFSET_X10,把 dBFS 平移成
//     "伪 SPL"。偏移取 94 dB(满幅≈94 dB SPL),未经声级计标定,只保证可比较;
//   - 全部为整数运算(查表 + 定点),ESP32-C3 无 FPU,不引入软浮点。
#pragma once

#include <stdint.h>
#include <stdbool.h>

// 伪 SPL 偏移:0 dBFS 视作 94 dB。需要真机标定时改这一个常数。
#define SOUND_METER_DB_OFFSET_X10 940

// 告警阈值范围与默认值(dB)。
#define SOUND_METER_THR_DEFAULT_DB 65
#define SOUND_METER_THR_MIN_DB     40
#define SOUND_METER_THR_MAX_DB     100

// 告警状态机:连续 N 帧超阈值才触发(去抖),连续 M 帧低于"阈值-滞回"才解除。
// 16ms/帧 -> 触发 48ms、解除 80ms,人耳对突发噪声的感知量级。
#define SOUND_METER_ALARM_ENTER_FRAMES 3
#define SOUND_METER_ALARM_EXIT_FRAMES  5
#define SOUND_METER_HYST_DB            4

// 每帧样本数 256 @ 16kHz -> 16ms。统计换算时长时用。
#define SOUND_METER_FRAME_MS 16

// 会话模型。除 threshold_db 可由按键回调原子改写(int32 对齐写)外,
// 其余字段只允许持有它的采集任务读写。
typedef struct {
    int32_t  threshold_db;   // 当前告警阈值(dB)
    int32_t  last_db_x10;    // 最近一帧的显示分贝
    int32_t  peak_db_x10;    // 会话峰值(0=尚无数据;显示值恒>0)
    int64_t  db_sum_x10;     // 会话分贝累计(供均值)
    uint32_t frames;         // 会话帧数(时长 = frames * 16ms)
    uint32_t alarm_count;    // 告警触发次数
    bool     alarm_active;   // 当前是否处于告警态
    uint8_t  over_frames;    // 连续超阈值帧数(状态机去抖)
    uint8_t  under_frames;   // 连续低于解除线帧数(状态机去抖)
} sound_meter_model_t;

// 整数平方根:返回 floor(sqrt(v))。v=0 返回 0。
uint32_t sound_meter_isqrt_u64(uint64_t v);

// dBFS*10 = 200*log10(rms/32768),rms=0 取下限 -903(1 LSB 对应值)。
// rms>32767 时按 32767 防御钳位。误差 <= 1(0.1 dB)。
int32_t sound_meter_dbfs_x10(uint32_t rms);

// 伪 SPL 显示值*10 = dbfs_x10 + 940,再钳到 [0,1200]。
int32_t sound_meter_display_db_x10(uint32_t rms);

// 音量条百分比:30..90 dB 线性映射到 0..100,区间外钳位。
int32_t sound_meter_bar_pct(int32_t db_x10);

// 会话初始化/重置。thr_db 越界时按 SOUND_METER_THR_DEFAULT_DB 回退。
void sound_meter_model_init(sound_meter_model_t *m, int32_t thr_db);

// 阈值步进并钳位到 [40,100];steps_db 为负则是下调。
int32_t sound_meter_threshold_step(int32_t thr_db, int steps_db);

// 从持久化值恢复阈值:越界(含 0/255 等"未写入"值)回退默认值。
int32_t sound_meter_threshold_load(int32_t stored_db);

// 喂入一帧 RMS:更新读数与统计,推进告警状态机。
// 返回 true 表示告警状态在本帧发生翻转(供 UI/动效响应边沿)。
bool sound_meter_model_frame(sound_meter_model_t *m, uint32_t rms);

// 清零会话统计(读数/峰值/均值/时长/告警计数)。
// threshold_db 与 alarm_active 保留:环境没变,阈值与告警态延续才有意义。
void sound_meter_model_reset_session(sound_meter_model_t *m);

// 会话平均分贝*10;frames==0 时返回 0。
int32_t sound_meter_mean_db_x10(const sound_meter_model_t *m);

// 帧数 -> 会话秒数(向下取整)。
uint32_t sound_meter_session_seconds(uint32_t frames);

// ---------------------------------------------------------------------------
// 响度分区(供 UI 配色区分):边界 50/65/75/85 dB。纯查表,主机可测。
// ---------------------------------------------------------------------------
typedef enum {
    SOUND_METER_ZONE_QUIET = 0,   // <  50 dB 安静
    SOUND_METER_ZONE_NORMAL,      // 50 - 65 dB 正常
    SOUND_METER_ZONE_MODERATE,    // 65 - 75 dB 偏响
    SOUND_METER_ZONE_LOUD,        // 75 - 85 dB 很响
    SOUND_METER_ZONE_EXTREME,     // >= 85  dB 极响
} sound_meter_zone_t;

// 响度分区数(供 UI 建配色表)。
#define SOUND_METER_ZONE_COUNT 5

sound_meter_zone_t sound_meter_zone_of(int32_t db_x10);

// ---------------------------------------------------------------------------
// 突变唤醒检测(息屏后自动亮屏的判定)。
//   慢速 EMA(权重 1/32)跟踪近期平均电平;瞬时电平偏离 EMA 超过
//   WAKE_JUMP_DB 且连续 WAKE_JUMP_FRAMES 帧 -> 判定"音量剧变"并置位。
//   触发后锁存,直到偏离回落到阈值的一半以下才重新武装(滞回,
//   避免持续高音量期间反复触发)。缓变(坡度小于阈值/31)不触发。
// ---------------------------------------------------------------------------
#define SOUND_METER_WAKE_JUMP_DB     20   // 判定剧变的偏离量(dB)
#define SOUND_METER_WAKE_JUMP_FRAMES  2   // 连续帧数(约 32ms)

typedef struct {
    int32_t ema_db_x10;   // 慢速 EMA(Q10)
    bool    ready;        // 已吃入首帧(EMA 有基线)
    bool    latched;      // 已触发,等待偏离回落后重新武装
    uint8_t over_frames;  // 连续超偏离帧数
} sound_meter_wake_t;

void sound_meter_wake_init(sound_meter_wake_t *w);

// 喂入一帧显示分贝。返回 true 表示本帧发生"音量剧变"(上升沿,每次
// 剧变事件只返回一次)。
bool sound_meter_wake_frame(sound_meter_wake_t *w, int32_t db_x10);
