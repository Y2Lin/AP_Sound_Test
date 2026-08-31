// main/demo_sound_meter.c -- 周围音量检测页(本应用唯一页面,开机由 main.c 加载):
//   麦克风 RMS -> 伪 SPL 读数 + 五区彩色音量条(带 dB 刻度与峰值保持线)
//   + 阈值告警(整屏变红) + 自动息屏/唤醒。
//
// UI 设计(240x320,可用区 y≈48..284):
//   - 大读数(28 号字)按响度分区变色:蓝(静)/绿(正常)/深琥珀(偏响)/橙(很响)/红(极响);
//   - 音量条指示条同步分区变色,条下是五色区带(30..90dB 标尺)+ 30/60/90 刻度;
//   - 阈值刻度(橙)与峰值保持线(白)叠加在音量条上,一眼读出当前位置与峰值;
//   - 状态徽章:绿底 MONITOR / 白底红字 ALARM / 灰底 MIC FAIL;
//   - 统计面板四色标签:PEAK(橙)/AVG(蓝)/TIME(青)/ALARMS(品红),数值随分区变色;
//   - 告警态:屏幕深红、面板红、全部文字变白、徽章反白,与正常态对比强烈。
//
// 息屏/唤醒:
//   - 监视(绿)状态下无按键活动 3 分钟 -> 背光关闭;采样与统计继续在暗中运行;
//   - 触发告警(超过阈值)或音量剧变(偏离慢速 EMA >= 20dB) -> 自动亮屏;
//   - 任意按键 -> 立即亮屏。所有背光切换统一在采集任务里执行(LEDC
//     不属于 LVGL,但也不该被按键回调与任务并发修改)。
//
// 并发与生命周期约定(遵循硬件指南 §8 的音频退出契约):
//   - bsp_audio_read 会阻塞约一帧(16ms),只能在 worker 任务里调;
//     退出时不能直接 vTaskDelete 阻塞在 codec I/O 里的任务,
//     故任务与队列首次进入页面后常驻,用 s_active/s_idle 握手停止采集。
//   - 任务只碰音频、模型与队列,不碰任何 lv_* 对象(LVGL 非线程安全);
//     UI 由 lv_timer 在 LVGL 任务里消费快照,天然免锁。
//   - 按键回调运行于 button 组件任务(main.c 已持有 LVGL 锁):只做
//     阈值原子赋值(int32 对齐写)、置标志与改自己的 LVGL 对象,不阻塞。
//     NVS 落盘由常驻任务防抖执行,绝不在回调/exit 里写。
//   - 纯逻辑见 sound_meter_model.c;本文件只做 I/O、队列、UI 与持久化。
#include "demo.h"
#include "bsp_audio.h"
#include "bsp_display.h"   // bsp_display_backlight:息屏/亮屏
#include "ui_pixel.h"
#include "sound_meter_model.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "demo_sound";

#define SM_HZ            16000
#define SM_BITS          16
#define SM_CH            1
#define SM_FRAME_SAMPLES 256                 // 16ms/帧(模型常量 SOUND_METER_FRAME_MS)
#define SM_SNAPSHOT_EVERY 5                  // 每 5 帧(80ms)发布一次快照
#define SM_TASK_STACK    4096
#define SM_QUEUE_LEN     8
#define SM_NVS_NAMESPACE "soundmeter"
#define SM_NVS_KEY       "alarm_db"
#define SM_NVS_DEBOUNCE_MS 600               // 阈值停止变化多久后才落盘
#define SM_SCREEN_TIMEOUT_MS (3U * 60U * 1000U)   // 无活动息屏:3 分钟

// 告警态的屏幕配色:背景/面板整体变红,与五区彩条形成强对比。
#define SM_ALARM_BG    0xB3261E              // 告警:深红屏幕底
#define SM_STATE_ERR   0x9AA7B0              // MIC FAIL:灰
#define SM_HINT_COLOR  0x4A5A66              // 提示行:弱化墨色
#define SM_MUTED       0x78909C              // 刻度/单位:灰蓝
#define SM_BAR_TRACK   0x2E3A44              // 音量条底槽:深灰蓝
#define SM_BADGE_OK_BG 0x2E7D32              // MONITOR 徽章:绿底

// 分区色(指示条/区带用亮色,放深槽与纸色面板上)。
static const uint32_t ZONE_COLOR[SOUND_METER_ZONE_COUNT] = {
    0x4FC3F7,   // QUIET   <50dB   天蓝
    0x82BE2D,   // NORMAL  50-65    草绿
    0xFFD54F,   // MODERATE 65-75   明黄
    0xFF9838,   // LOUD    75-85    橙
    0xE43B2F,   // EXTREME >=85     红
};
// 分区文字色(深色,放纸色面板上保证对比度)。
static const uint32_t ZONE_TEXT[SOUND_METER_ZONE_COUNT] = {
    0x1565C0,   // 蓝
    0x2E7D32,   // 绿
    0xB07400,   // 深琥珀
    0xE65100,   // 深橙
    0xB3261E,   // 深红
};
// 统计标签色:PEAK/AVG/TIME/ALARMS 各一色,便于扫读。
#define SM_CAP_PEAK 0xE65100
#define SM_CAP_AVG  0x1565C0
#define SM_CAP_TIME 0x00838F
#define SM_CAP_ALM  0xAD1457

typedef struct {
    int32_t  db_x10;        // 最近读数(伪 SPL*10)
    int32_t  peak_db_x10;   // 会话峰值
    int32_t  mean_db_x10;   // 会话均值
    uint32_t frames;        // 会话帧数(时长换算用)
    uint32_t alarm_count;   // 告警次数
    bool     alarm;
    bool     mic_error;
} sm_snapshot_t;

static lv_obj_t   *s_scr, *s_panel_main, *s_panel_stats;
static lv_obj_t   *s_db, *s_db_unit, *s_thr;
static lv_obj_t   *s_badge, *s_badge_lbl;
static lv_obj_t   *s_bar, *s_thr_mark, *s_peak_mark, *s_strip[5], *s_tick[3];
static lv_obj_t   *s_cap_peak, *s_cap_avg, *s_cap_time, *s_cap_alm;
static lv_obj_t   *s_val_peak, *s_val_avg, *s_val_time, *s_val_alm;
static lv_obj_t   *s_hint, *s_mascot;
static lv_timer_t *s_timer;

static TaskHandle_t  s_task;
static QueueHandle_t s_queue;
static volatile bool s_active;          // 页面在前台(采集开关)
static volatile bool s_idle = true;     // 任务处于安全空转态(退出握手用)
static volatile bool s_reset_req;       // OK 短按:请求清零会话统计
static volatile bool s_thr_dirty;       // 阈值已变更,待防抖写 NVS
static volatile uint32_t s_thr_changed_ms;
static volatile bool s_screen_off;      // 当前背光关闭(息屏)
static volatile bool s_screen_wake_req; // 按键请求任务亮屏
static volatile uint32_t s_last_activity_ms;   // 最近一次按键/唤醒事件
static sound_meter_model_t s_model;     // 归采集任务所有(阈值可由按键原子改)
static sound_meter_wake_t  s_wake;      // 音量剧变检测(归采集任务所有)
static bool s_last_alarm;               // UI 当前呈现的告警态(换色判定)
static bool s_nvs_warned;               // NVS 打开失败只告警一次,避免刷屏

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ------------------------------------------------------------------ NVS ----
// NVS 分区由 app_main 统一初始化(失败不阻塞启动);这里只做打开与读写,
// 打不开就回退默认值/放弃写入,不擦除分区。

static int32_t threshold_load_nvs(void) {
    nvs_handle_t h;
    if (nvs_open(SM_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
        return SOUND_METER_THR_DEFAULT_DB;   // 首次使用等场景:读不到就用默认
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, SM_NVS_KEY, &v);
    nvs_close(h);
    if (err != ESP_OK) return SOUND_METER_THR_DEFAULT_DB;
    return sound_meter_threshold_load((int32_t)v);
}

// 只在常驻采集任务里调用;失败仅记日志,不擦除分区,内存中的阈值继续生效。
static void threshold_store_nvs(int32_t db) {
    nvs_handle_t h;
    if (nvs_open(SM_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        if (!s_nvs_warned) {
            s_nvs_warned = true;
            ESP_LOGW(TAG, "NVS 打开失败,阈值仅保留在内存");
        }
        return;
    }
    esp_err_t err = nvs_set_u8(h, SM_NVS_KEY, (uint8_t)db);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "NVS 写入失败: %s", esp_err_to_name(err));
}

// --------------------------------------------------------------- 采集任务 ----

// 发布一帧快照到 UI(队列满则丢:UI 每 100ms 只取最新,丢旧帧无损)。
static void publish_snapshot(bool mic_error) {
    if (!s_queue) return;
    sm_snapshot_t snap = {
        .db_x10       = s_model.last_db_x10,
        .peak_db_x10  = s_model.peak_db_x10,
        .mean_db_x10  = sound_meter_mean_db_x10(&s_model),
        .frames       = s_model.frames,
        .alarm_count  = s_model.alarm_count,
        .alarm        = s_model.alarm_active,
        .mic_error    = mic_error,
    };
    (void)xQueueSend(s_queue, &snap, 0);
}

// 亮屏(只在采集任务里调,统一持有背光的写入权)。
static void screen_on(uint32_t now) {
    s_last_activity_ms = now;
    if (s_screen_off) {
        s_screen_off = false;
        bsp_display_backlight(100);
        ESP_LOGI(TAG, "亮屏");
    }
}

static void sm_task(void *arg) {
    (void)arg;
    int16_t samples[SM_FRAME_SAMPLES];
    bool was_active = false;
    bool mic_error = false;
    uint32_t frames_since_publish = 0;

    for (;;) {
        uint32_t now = now_ms();

        // ---- 背光管理(统一在本任务执行)----
        if (s_screen_wake_req) {              // 按键请求亮屏
            s_screen_wake_req = false;
            screen_on(now);
        } else if (!s_screen_off && !s_model.alarm_active &&
                   (uint32_t)(now - s_last_activity_ms) >=
                       SM_SCREEN_TIMEOUT_MS) {
            // 监视(绿)状态 3 分钟无活动 -> 息屏;采样与统计继续。
            // 告警期间不灭屏:用户正需要看到告警。
            s_screen_off = true;
            bsp_display_backlight(0);
            ESP_LOGI(TAG, "息屏(3 分钟无活动)");
        }

        // NVS 防抖落盘:阈值停止变化 600ms 后写一次。放在常驻任务里,
        // 即使页面已退出(或麦克风失效)也能把最后一次变更写完。
        if (s_thr_dirty &&
            (uint32_t)(now - s_thr_changed_ms) >= SM_NVS_DEBOUNCE_MS) {
            s_thr_dirty = false;   // 先清标志再写:写失败也不无限重试
            threshold_store_nvs(s_model.threshold_db);
        }

        if (!s_active) {
            s_idle = true;
            was_active = false;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (!was_active) {
            // 每次重新激活都设置格式(BSP 对同格式重复调用是廉价的)。
            if (bsp_audio_set_format(SM_HZ, SM_BITS, SM_CH) != ESP_OK) {
                if (!mic_error) {
                    mic_error = true;
                    ESP_LOGE(TAG, "音频格式设置失败");
                    publish_snapshot(true);
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            was_active = true;
            mic_error = false;
            frames_since_publish = 0;
        }

        s_idle = false;
        if (bsp_audio_read(samples, sizeof(samples)) != ESP_OK) {
            if (!mic_error) {
                mic_error = true;
                ESP_LOGE(TAG, "麦克风读取失败");
                publish_snapshot(true);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        mic_error = false;
        if (!s_active) continue;   // 读完才发现退出请求:丢弃本帧

        // RMS:平方和 -> 整数平方根(全整数,C3 无 FPU,不引入软浮点)。
        uint64_t sum_sq = 0;
        for (int i = 0; i < SM_FRAME_SAMPLES; i++) {
            int32_t s = samples[i];
            sum_sq += (uint64_t)(s * s);
        }
        uint32_t rms = sound_meter_isqrt_u64(sum_sq / SM_FRAME_SAMPLES);

        bool alarm_edge  = sound_meter_model_frame(&s_model, rms);
        bool volume_jump = sound_meter_wake_frame(&s_wake, s_model.last_db_x10);
        if (s_reset_req) {                       // OK 短按:由本任务安全清零
            sound_meter_model_reset_session(&s_model);
            s_reset_req = false;
        }

        // 超过阈值(告警上升沿)或音量剧变 -> 自动亮屏并重置活动时间。
        if ((alarm_edge && s_model.alarm_active) || volume_jump) {
            screen_on(now);
            if (volume_jump)
                ESP_LOGI(TAG, "音量剧变唤醒: %d.%d dB",
                         (int)(s_model.last_db_x10 / 10),
                         (int)(s_model.last_db_x10 % 10));
        }

        if (++frames_since_publish >= SM_SNAPSHOT_EVERY) {
            frames_since_publish = 0;
            publish_snapshot(false);
        }
    }
}

// -------------------------------------------------------------------- UI ----

// 小工具:在父对象上放一个指定字体/颜色的文本。
static lv_obj_t *label_at(lv_obj_t *parent, int x, int y,
                          const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

// 小工具:放一个纯色块(区带/标记/徽章底)。
static lv_obj_t *block_at(lv_obj_t *parent, int x, int y, int w, int h,
                          uint32_t color) {
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    return b;
}

// 状态徽章:绿底白字 MONITOR / 白底红字 ALARM / 灰底白字 MIC FAIL。
static void badge_set(const char *text, uint32_t bg, uint32_t fg) {
    lv_label_set_text(s_badge_lbl, text);
    lv_obj_set_style_bg_color(s_badge, lv_color_hex(bg), 0);
    lv_obj_set_style_text_color(s_badge_lbl, lv_color_hex(fg), 0);
}

// 告警态整体换色(在 LVGL 上下文调用:enter 或 lv_timer)。
// 大读数与统计数值的颜色在 ui_tick 里按分区动态设置,不在此处。
static void apply_alarm_style(bool alarm) {
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(alarm ? SM_ALARM_BG : UI_SKY), 0);
    lv_obj_set_style_bg_color(s_panel_main, lv_color_hex(alarm ? UI_RED : UI_PAPER), 0);
    lv_obj_set_style_bg_color(s_panel_stats, lv_color_hex(alarm ? UI_RED : UI_PAPER), 0);

    uint32_t ink   = alarm ? 0xFFFFFF : UI_INK;
    uint32_t muted = alarm ? 0xFFFFFF : SM_MUTED;
    uint32_t thr_c = alarm ? 0xFFFFFF : SM_CAP_PEAK;   // 阈值行与 PEAK 同色系

    lv_obj_set_style_text_color(s_db_unit, lv_color_hex(muted), 0);
    lv_obj_set_style_text_color(s_thr, lv_color_hex(thr_c), 0);
    for (int i = 0; i < 3; i++)
        lv_obj_set_style_text_color(s_tick[i], lv_color_hex(muted), 0);

    lv_obj_set_style_text_color(s_cap_peak, lv_color_hex(alarm ? 0xFFFFFF : SM_CAP_PEAK), 0);
    lv_obj_set_style_text_color(s_cap_avg,  lv_color_hex(alarm ? 0xFFFFFF : SM_CAP_AVG), 0);
    lv_obj_set_style_text_color(s_cap_time, lv_color_hex(alarm ? 0xFFFFFF : SM_CAP_TIME), 0);
    lv_obj_set_style_text_color(s_cap_alm,  lv_color_hex(alarm ? 0xFFFFFF : SM_CAP_ALM), 0);
    lv_obj_set_style_text_color(s_val_time, lv_color_hex(ink), 0);
    lv_obj_set_style_text_color(s_val_alm,  lv_color_hex(ink), 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(alarm ? 0xFFFFFF : SM_HINT_COLOR), 0);

    lv_obj_set_style_bg_color(s_thr_mark, lv_color_hex(alarm ? 0xFFFFFF : UI_ORANGE), 0);
}

// 阈值数值 + 音量条上的阈值刻度位置(按键回调/enter 里即时刷新)。
static void refresh_threshold_ui(void) {
    lv_label_set_text_fmt(s_thr, "THR %d dB", (int)s_model.threshold_db);
    int pct = sound_meter_bar_pct(s_model.threshold_db * 10);
    int x = (182 * pct) / 100 - 1;              // bar 宽 182,标记宽 3
    if (x < 0) x = 0;
    if (x > 179) x = 179;
    lv_obj_set_pos(s_thr_mark, x, 52);
}

// lv_timer 回调:跑在 LVGL 任务里,操作对象免锁。只取最新快照。
// 息屏时只清队列不刷新(屏幕不可见,省去重绘与刷屏开销)。
static void ui_tick(lv_timer_t *t) {
    (void)t;
    sm_snapshot_t snap;
    bool have = false;
    while (xQueueReceive(s_queue, &snap, 0) == pdTRUE) have = true;
    if (!have || s_screen_off) return;

    bool alarm = snap.alarm && !snap.mic_error;

    if (snap.mic_error) {
        lv_label_set_text(s_db, "--.-");
        lv_obj_set_style_text_color(s_db, lv_color_hex(SM_STATE_ERR), 0);
        badge_set("MIC FAIL", SM_MUTED, 0xFFFFFF);
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
        lv_obj_add_flag(s_peak_mark, LV_OBJ_FLAG_HIDDEN);
    } else {
        int db = snap.db_x10;
        sound_meter_zone_t z = sound_meter_zone_of(db);
        lv_label_set_text_fmt(s_db, "%d.%d", db / 10, db % 10);
        lv_obj_set_style_text_color(s_db,
            lv_color_hex(alarm ? 0xFFFFFF : ZONE_TEXT[z]), 0);
        badge_set(alarm ? "ALARM" : "MONITOR",
                  alarm ? 0xFFFFFF : SM_BADGE_OK_BG,
                  alarm ? SM_ALARM_BG : 0xFFFFFF);
        // 指示条分区变色;80ms 平滑跟随。告警时反白以示紧急。
        lv_bar_set_value(s_bar, sound_meter_bar_pct(db), LV_ANIM_ON);
        lv_obj_set_style_bg_color(s_bar,
            lv_color_hex(alarm ? 0xFFFFFF : ZONE_COLOR[z]), LV_PART_INDICATOR);

        // 峰值保持线:白线停在会话峰值处(无数据时隐藏)。
        if (snap.peak_db_x10 > 0) {
            int px = (182 * sound_meter_bar_pct(snap.peak_db_x10)) / 100;
            if (px > 180) px = 180;
            lv_obj_remove_flag(s_peak_mark, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_peak_mark, px, 52);
        } else {
            lv_obj_add_flag(s_peak_mark, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (alarm != s_last_alarm) {
        s_last_alarm = alarm;
        apply_alarm_style(alarm);
        ui_pixel_mascot_jump(s_mascot);         // 告警边沿:小机器人跳一下
    }

    int peak = snap.peak_db_x10;
    int mean = snap.mean_db_x10;
    lv_label_set_text_fmt(s_val_peak, "%d.%d", peak / 10, peak % 10);
    lv_label_set_text_fmt(s_val_avg,  "%d.%d", mean / 10, mean % 10);
    // 峰值/均值数值按各自分区着色,告警时反白。
    lv_obj_set_style_text_color(s_val_peak,
        lv_color_hex(alarm ? 0xFFFFFF
                           : ZONE_TEXT[sound_meter_zone_of(peak)]), 0);
    lv_obj_set_style_text_color(s_val_avg,
        lv_color_hex(alarm ? 0xFFFFFF
                           : ZONE_TEXT[sound_meter_zone_of(mean)]), 0);

    uint32_t secs = sound_meter_session_seconds(snap.frames);
    lv_label_set_text_fmt(s_val_time, "%02u:%02u",
                          (unsigned)(secs / 60), (unsigned)(secs % 60));
    lv_label_set_text_fmt(s_val_alm, "%u", (unsigned)snap.alarm_count);
}

// ------------------------------------------------------------ 页面接口 ----

void demo_sound_meter_enter(void) {
    if (s_thr_dirty) {
        // 上一次的防抖写还没落盘:保留内存里的阈值,避免被 NVS 旧值覆盖回去。
        sound_meter_model_init(&s_model, s_model.threshold_db);
    } else {
        sound_meter_model_init(&s_model, threshold_load_nvs());
    }
    sound_meter_wake_init(&s_wake);
    s_reset_req = false;
    s_last_alarm = false;
    s_screen_off = false;
    s_screen_wake_req = false;
    s_last_activity_ms = now_ms();

    s_scr = ui_pixel_screen_create("SOUND");

    // ---- 主面板(18,48,204,122):大读数 + 徽章 + 阈值 + 音量条 + 区带标尺 ----
    s_panel_main = ui_pixel_panel_create(s_scr, 18, 48, 204, 122, UI_PAPER);

    s_db = label_at(s_panel_main, 2, 0, &lv_font_montserrat_28, UI_INK);
    lv_label_set_text(s_db, "--.-");
    s_db_unit = label_at(s_panel_main, 74, 12, &lv_font_montserrat_14, SM_MUTED);
    lv_label_set_text(s_db_unit, "dB");

    s_badge = block_at(s_panel_main, 106, 2, 76, 30, SM_BADGE_OK_BG);
    s_badge_lbl = label_at(s_badge, 0, 8, &lv_font_montserrat_14, 0xFFFFFF);
    lv_obj_set_width(s_badge_lbl, 76);
    lv_obj_set_style_text_align(s_badge_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_badge_lbl, "MONITOR");

    s_thr = label_at(s_panel_main, 2, 36, &lv_font_montserrat_14, SM_CAP_PEAK);
    lv_label_set_text(s_thr, "THR -- dB");

    s_bar = lv_bar_create(s_panel_main);
    lv_obj_set_pos(s_bar, 0, 54);
    lv_obj_set_size(s_bar, 182, 20);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(s_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(s_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(s_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_bar, lv_color_hex(UI_INK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(SM_BAR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(ZONE_COLOR[1]),
                              LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(s_bar, 80, LV_PART_MAIN);   // 平滑跟随

    // 阈值刻度(橙)与峰值保持线(白):在 bar 之后创建以盖在其上。
    s_thr_mark = block_at(s_panel_main, 0, 52, 3, 24, UI_ORANGE);
    s_peak_mark = block_at(s_panel_main, 0, 52, 2, 24, 0xFFFFFF);
    lv_obj_add_flag(s_peak_mark, LV_OBJ_FLAG_HIDDEN);

    // 五色区带(30..90 dB 标尺):与音量条同宽同映射,宽度按 20/15/10/10/5 dB 分配。
    {
        static const int STRIP_W[5] = {61, 45, 30, 30, 16};   // 合计 182
        int sx = 0;
        for (int i = 0; i < SOUND_METER_ZONE_COUNT; i++) {
            s_strip[i] = block_at(s_panel_main, sx, 76, STRIP_W[i], 6,
                                  ZONE_COLOR[i]);
            sx += STRIP_W[i];
        }
    }

    // dB 刻度数字:30/60/90 对齐区带两端与中点。
    {
        static const char *TICK_TXT[3] = {"30", "60", "90"};
        static const int   TICK_X[3]   = {0, 84, 167};
        for (int i = 0; i < 3; i++) {
            s_tick[i] = label_at(s_panel_main, TICK_X[i], 84,
                                 &lv_font_montserrat_14, SM_MUTED);
            lv_label_set_text(s_tick[i], TICK_TXT[i]);
        }
    }

    // ---- 统计面板(18,176,204,88):四色标签 + 数值 ----
    s_panel_stats = ui_pixel_panel_create(s_scr, 18, 176, 204, 88, UI_PAPER);

    s_cap_peak = label_at(s_panel_stats, 8, 0, &lv_font_montserrat_14, SM_CAP_PEAK);
    lv_label_set_text(s_cap_peak, "PEAK dB");
    s_cap_avg = label_at(s_panel_stats, 100, 0, &lv_font_montserrat_14, SM_CAP_AVG);
    lv_label_set_text(s_cap_avg, "AVG dB");

    s_val_peak = label_at(s_panel_stats, 8, 16, &lv_font_montserrat_14, UI_INK);
    lv_label_set_text(s_val_peak, "--.-");
    s_val_avg = label_at(s_panel_stats, 100, 16, &lv_font_montserrat_14, UI_INK);
    lv_label_set_text(s_val_avg, "--.-");

    s_cap_time = label_at(s_panel_stats, 8, 34, &lv_font_montserrat_14, SM_CAP_TIME);
    lv_label_set_text(s_cap_time, "TIME");
    s_cap_alm = label_at(s_panel_stats, 100, 34, &lv_font_montserrat_14, SM_CAP_ALM);
    lv_label_set_text(s_cap_alm, "ALARMS");

    s_val_time = label_at(s_panel_stats, 8, 50, &lv_font_montserrat_14, UI_INK);
    lv_label_set_text(s_val_time, "00:00");
    s_val_alm = label_at(s_panel_stats, 100, 50, &lv_font_montserrat_14, UI_INK);
    lv_label_set_text(s_val_alm, "0");

    // ---- 底部:按键提示 + 吉祥物 ----
    s_hint = label_at(s_scr, 10, 270, &lv_font_montserrat_14, SM_HINT_COLOR);
    lv_obj_set_width(s_hint, 170);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_hint, "UP/DN THR  OK RESET");

    s_mascot = ui_pixel_mascot_create(s_scr, 189, 238);

    apply_alarm_style(false);
    refresh_threshold_ui();

    if (!s_queue) s_queue = xQueueCreate(SM_QUEUE_LEN, sizeof(sm_snapshot_t));
    if (!s_task) {
        if (xTaskCreate(sm_task, "sound_meter", SM_TASK_STACK, NULL, 4,
                        &s_task) != pdPASS) {
            s_task = NULL;
            ESP_LOGE(TAG, "采集任务创建失败");
        }
    }

    if (s_queue) xQueueReset(s_queue);   // 丢弃上一次会话的残留快照
    s_active = true;                     // UI 就绪后再开采集
    s_timer = lv_timer_create(ui_tick, 100, NULL);
    lv_screen_load(s_scr);
}

void demo_sound_meter_exit(void) {
    // 停止采集:bsp_audio_read 最长阻塞一帧,握手等任务回到空转态。
    // 不 vTaskDelete(硬件指南 §8:不能删除阻塞在 codec I/O 里的任务)。
    s_active = false;
    for (int i = 0; i < 50 && !s_idle; i++) vTaskDelay(pdMS_TO_TICKS(1));
    if (!s_idle) ESP_LOGW(TAG, "采集任务停止超时(50ms)");

    // 若在息屏状态被卸载,恢复背光,避免下一界面黑屏。
    if (s_screen_off) {
        s_screen_off = false;
        bsp_display_backlight(100);
    }

    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_scr)   { lv_obj_delete(s_scr); s_scr = NULL; }
    s_panel_main = s_panel_stats = NULL;
    s_db = s_db_unit = s_thr = NULL;
    s_badge = s_badge_lbl = NULL;
    s_bar = s_thr_mark = s_peak_mark = NULL;
    for (int i = 0; i < SOUND_METER_ZONE_COUNT; i++) s_strip[i] = NULL;
    for (int i = 0; i < 3; i++) s_tick[i] = NULL;
    s_cap_peak = s_cap_avg = s_cap_time = s_cap_alm = NULL;
    s_val_peak = s_val_avg = s_val_time = s_val_alm = NULL;
    s_hint = NULL;
    s_mascot = NULL;
    // 任务与队列常驻:下次进入免重建;未落盘的阈值由任务在后台防抖写完。
}

void demo_sound_meter_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    // 运行于 button 组件任务,main.c 已持有 LVGL 锁:可安全改本页对象。
    // NVS 落盘不在回调里做(防抖交给常驻任务)。
    // 任意按键都视为活动:请求亮屏并重置 3 分钟息屏计时。
    s_last_activity_ms = now_ms();
    if (s_screen_off) s_screen_wake_req = true;

    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        int steps = 0;
        if (ev == BSP_BTN_CLICK)      steps = 1;   // 短按:±1 dB
        else if (ev == BSP_BTN_LONG)  steps = 5;   // 长按:±5 dB 快调
        if (steps == 0) return;
        int dir = (btn == BSP_BTN_UP) ? 1 : -1;
        // 对齐的 int32 写,采集任务读取是原子的。
        s_model.threshold_db =
            sound_meter_threshold_step(s_model.threshold_db, dir * steps);
        s_thr_changed_ms = now_ms();
        s_thr_dirty = true;
        refresh_threshold_ui();
        ui_pixel_mascot_jump(s_mascot);
    } else if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        s_reset_req = true;   // 由采集任务清零,避免与帧更新竞争
        ui_pixel_mascot_jump(s_mascot);
    }
    // OK 长按无操作;双击/按下沿忽略。
}
