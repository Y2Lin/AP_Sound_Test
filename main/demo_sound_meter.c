// main/demo_sound_meter.c -- 周围音量检测页:麦克风 RMS -> 伪 SPL 读数 + 音量条 + 阈值告警。
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
#include "demo_radio.h"     // demo_radio_nvs_prepare():幂等的 NVS 初始化
#include "bsp_audio.h"
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

// 告警态的屏幕配色:背景/面板/读数整体变红,一眼可辨。
#define SM_ALARM_BG    0xB3261E
#define SM_STATE_OK    0x2E7D32              // MONITOR:绿色
#define SM_STATE_ERR   0x9AA7B0              // MIC FAIL:灰
#define SM_HINT_COLOR  0x4A5A66              // 提示行:弱化墨色

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
static lv_obj_t   *s_db, *s_thr, *s_state, *s_bar, *s_thr_mark;
static lv_obj_t   *s_peak_line, *s_time_line, *s_hint, *s_mascot;
static lv_timer_t *s_timer;

static TaskHandle_t  s_task;
static QueueHandle_t s_queue;
static volatile bool s_active;          // 页面在前台(采集开关)
static volatile bool s_idle = true;     // 任务处于安全空转态(退出握手用)
static volatile bool s_reset_req;       // OK 短按:请求清零会话统计
static volatile bool s_thr_dirty;       // 阈值已变更,待防抖写 NVS
static volatile uint32_t s_thr_changed_ms;
static sound_meter_model_t s_model;     // 归采集任务所有(阈值可由按键原子改)
static bool s_last_alarm;               // UI 当前呈现的告警态(换色判定)

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ------------------------------------------------------------------ NVS ----

static int32_t threshold_load_nvs(void) {
    if (demo_radio_nvs_prepare() != ESP_OK) return SOUND_METER_THR_DEFAULT_DB;
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
    if (demo_radio_nvs_prepare() != ESP_OK) return;
    nvs_handle_t h;
    if (nvs_open(SM_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS 打开失败,阈值仅保留在内存");
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

static void sm_task(void *arg) {
    (void)arg;
    int16_t samples[SM_FRAME_SAMPLES];
    bool was_active = false;
    bool mic_error = false;
    uint32_t frames_since_publish = 0;

    for (;;) {
        uint32_t now = now_ms();

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
            // 每次重新激活都设置格式(BSP 对同格式重复调用是廉价的);
            // 与 Audio 页交替进出时,BSP 内部会做 close/open 切换。
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

        (void)sound_meter_model_frame(&s_model, rms);
        if (s_reset_req) {                       // OK 短按:由本任务安全清零
            sound_meter_model_reset_session(&s_model);
            s_reset_req = false;
        }

        if (++frames_since_publish >= SM_SNAPSHOT_EVERY) {
            frames_since_publish = 0;
            publish_snapshot(false);
        }
    }
}

// -------------------------------------------------------------------- UI ----

// 告警态整体换色(在 LVGL 上下文调用:enter 或 lv_timer)。
static void apply_alarm_style(bool alarm) {
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(alarm ? SM_ALARM_BG : UI_SKY), 0);
    lv_obj_set_style_bg_color(s_panel_main, lv_color_hex(alarm ? UI_RED : UI_PAPER), 0);
    lv_obj_set_style_bg_color(s_panel_stats, lv_color_hex(alarm ? UI_RED : UI_PAPER), 0);
    uint32_t text = alarm ? 0xFFFFFF : UI_INK;
    lv_obj_set_style_text_color(s_db, lv_color_hex(text), 0);
    lv_obj_set_style_text_color(s_thr, lv_color_hex(text), 0);
    lv_obj_set_style_text_color(s_peak_line, lv_color_hex(text), 0);
    lv_obj_set_style_text_color(s_time_line, lv_color_hex(text), 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(text), 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x2E3A44), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(alarm ? 0xFFFFFF : UI_GRASS),
                              LV_PART_INDICATOR);
}

// 阈值数值 + 音量条上的阈值刻度位置(按键回调/enter 里即时刷新)。
static void refresh_threshold_ui(void) {
    lv_label_set_text_fmt(s_thr, "THR %d dB", (int)s_model.threshold_db);
    int pct = sound_meter_bar_pct(s_model.threshold_db * 10);
    int x = (182 * pct) / 100 - 1;              // bar 宽 182,标记宽 3
    if (x < 0) x = 0;
    if (x > 179) x = 179;
    lv_obj_set_pos(s_thr_mark, x, 45);
}

// lv_timer 回调:跑在 LVGL 任务里,操作对象免锁。只取最新快照。
static void ui_tick(lv_timer_t *t) {
    (void)t;
    sm_snapshot_t snap;
    bool have = false;
    while (xQueueReceive(s_queue, &snap, 0) == pdTRUE) have = true;
    if (!have) return;

    if (snap.mic_error) {
        lv_label_set_text(s_db, "--.- dB");
        lv_label_set_text(s_state, "MIC FAIL");
        lv_obj_set_style_text_color(s_state, lv_color_hex(SM_STATE_ERR), 0);
    } else {
        int db = snap.db_x10;
        lv_label_set_text_fmt(s_db, "%d.%d dB", db / 10, db % 10);
        lv_label_set_text(s_state, snap.alarm ? "ALARM" : "MONITOR");
        lv_obj_set_style_text_color(s_state,
            lv_color_hex(snap.alarm ? 0xFFFFFF : SM_STATE_OK), 0);
        lv_bar_set_value(s_bar, sound_meter_bar_pct(db), LV_ANIM_OFF);
    }

    if (snap.alarm != s_last_alarm) {
        s_last_alarm = snap.alarm;
        apply_alarm_style(snap.alarm);
        ui_pixel_mascot_jump(s_mascot);         // 告警边沿:小机器人跳一下
    }

    int peak = snap.peak_db_x10;
    int mean = snap.mean_db_x10;
    lv_label_set_text_fmt(s_peak_line, "PEAK %d.%d   AVG %d.%d",
                          peak / 10, peak % 10, mean / 10, mean % 10);
    uint32_t secs = sound_meter_session_seconds(snap.frames);
    lv_label_set_text_fmt(s_time_line, "TIME %02u:%02u   ALM %u",
                          (unsigned)(secs / 60), (unsigned)(secs % 60),
                          (unsigned)snap.alarm_count);
}

// ------------------------------------------------------------ 页面接口 ----

void demo_sound_meter_enter(void) {
    if (s_thr_dirty) {
        // 上一次的防抖写还没落盘:保留内存里的阈值,避免被 NVS 旧值覆盖回去。
        sound_meter_model_init(&s_model, s_model.threshold_db);
    } else {
        sound_meter_model_init(&s_model, threshold_load_nvs());
    }
    s_reset_req = false;
    s_last_alarm = false;

    s_scr = ui_pixel_screen_create("SOUND");

    // 主面板:大读数 + 阈值/状态 + 音量条(带阈值刻度)。
    s_panel_main = ui_pixel_panel_create(s_scr, 18, 52, 204, 96, UI_PAPER);
    s_db = ui_pixel_label(s_panel_main, "--.- dB",
                          &lv_font_montserrat_20, UI_INK);
    lv_obj_set_width(s_db, 182);
    lv_obj_set_style_text_align(s_db, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_db, 0, 1);

    s_thr = ui_pixel_label(s_panel_main, "THR -- dB",
                           &lv_font_montserrat_14, UI_INK);
    lv_obj_set_pos(s_thr, 0, 29);

    s_state = ui_pixel_label(s_panel_main, "MONITOR",
                             &lv_font_montserrat_14, SM_STATE_OK);
    lv_obj_set_width(s_state, 182);
    lv_obj_set_style_text_align(s_state, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_state, 0, 29);

    s_bar = lv_bar_create(s_panel_main);
    lv_obj_set_pos(s_bar, 0, 48);
    lv_obj_set_size(s_bar, 182, 22);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(s_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(s_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(s_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_bar, lv_color_hex(UI_INK), LV_PART_MAIN);

    // 阈值刻度:bar 上方的 3px 竖线,位置随阈值走(在 bar 之后创建,盖在其上)。
    s_thr_mark = lv_obj_create(s_panel_main);
    lv_obj_remove_flag(s_thr_mark, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_thr_mark, 3, 28);
    lv_obj_set_style_radius(s_thr_mark, 0, 0);
    lv_obj_set_style_border_width(s_thr_mark, 0, 0);
    lv_obj_set_style_pad_all(s_thr_mark, 0, 0);
    lv_obj_set_style_bg_color(s_thr_mark, lv_color_hex(UI_ORANGE), 0);

    // 统计面板:峰值/均值 + 时长/告警数 + 按键提示。
    s_panel_stats = ui_pixel_panel_create(s_scr, 18, 156, 204, 78, UI_PAPER);
    s_peak_line = ui_pixel_label(s_panel_stats, "PEAK --.-   AVG --.-",
                                 &lv_font_montserrat_14, UI_INK);
    lv_obj_set_width(s_peak_line, 182);
    lv_obj_set_style_text_align(s_peak_line, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_peak_line, 0, 1);

    s_time_line = ui_pixel_label(s_panel_stats, "TIME 00:00   ALM 0",
                                 &lv_font_montserrat_14, UI_INK);
    lv_obj_set_width(s_time_line, 182);
    lv_obj_set_style_text_align(s_time_line, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_time_line, 0, 19);

    s_hint = ui_pixel_label(s_panel_stats, "UP/DN:THRESH  OK:RESET",
                            &lv_font_montserrat_14, SM_HINT_COLOR);
    lv_obj_set_width(s_hint, 182);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_hint, 0, 38);

    s_mascot = ui_pixel_mascot_create(s_scr, 101, 238);

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

    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_scr)   { lv_obj_delete(s_scr); s_scr = NULL; }
    s_panel_main = s_panel_stats = NULL;
    s_db = s_thr = s_state = s_bar = s_thr_mark = NULL;
    s_peak_line = s_time_line = s_hint = NULL;
    s_mascot = NULL;
    // 任务与队列常驻:下次进入免重建;未落盘的阈值由任务在后台防抖写完。
}

void demo_sound_meter_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    // 运行于 button 组件任务,main.c 已持有 LVGL 锁:可安全改本页对象。
    // NVS 落盘不在回调里做(防抖交给常驻任务)。
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
    // OK 长按已被 main.c 统一拦截用于返回菜单;双击/按下沿忽略。
}
