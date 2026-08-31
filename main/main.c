// main/main.c -- FoloToy AI Passport 周围音量检测应用:外设初始化 + 页面加载。
//
// 本固件是独立应用:开机直接进入音量检测页,不提供组件演示菜单
// (基线仓库的演示菜单见 main 分支,本分支按应用分支惯例将其替换)。
//
// 按键语义(全局统一,全部交给页面处理):
//   上/下 短按   告警阈值 -/+1 dB
//   上/下 长按   告警阈值 -/+5 dB(快调)
//   确定  短按   清零会话统计
//   确定  长按   无操作(页面忽略)
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "demo.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_sleep.h"

static const char *TAG = "main";

// 按键回调运行在 button 组件的任务里,操作 LVGL 必须加锁。
// 独立应用没有菜单导航:所有按键事件直接转发给音量检测页。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!bsp_lvgl_lock(500)) return;
    demo_sound_meter_key(btn, ev);
    bsp_lvgl_unlock();
}

void app_main(void) {
    ESP_LOGI(TAG, "FoloToy AI Passport 周围音量检测应用启动");
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", wakeup);
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是应用的 UI 载体,失败就没有可用界面 -- 打清楚日志后退出,
    // 不做"串口界面"降级(独立应用只有一个用途,降级没有意义)。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,应用无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    // 阈值持久化依赖 NVS:初始化失败不阻塞启动(页面回退默认阈值),
    // 也不擦除分区 -- 与基线仓库"不自动擦除 NVS"的约定一致。
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s;阈值将只在内存中生效",
                 esp_err_to_name(nvs_err));
    }

    // 按键与音频失败都不阻塞:页面自身有对应的降级表现
    // (无按键=阈值固定为持久化值;无音频=状态行显示 MIC FAIL)。
    if (bsp_button_init(on_key, NULL) != ESP_OK)
        ESP_LOGE(TAG, "按键初始化失败:无法调节阈值");
    if (bsp_audio_init() != ESP_OK)
        ESP_LOGE(TAG, "音频初始化失败:页面将显示 MIC FAIL");

    if (bsp_lvgl_lock(1000)) {
        demo_sound_meter_enter();
        bsp_lvgl_unlock();
    }

    ESP_LOGI(TAG, "就绪:Sound Meter 应用已启动");
}
