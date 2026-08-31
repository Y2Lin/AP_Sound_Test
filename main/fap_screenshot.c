// main/fap_screenshot.c -- FAP_SCREENSHOT_V1 串口截屏协议(社区发布助手)。
//
// 协议(与发布助手 references/serial-screenshot.md 逐字对齐):
//   主机 -> 设备: ASCII 行 "FAP_SCREENSHOT_V1\n"(115200 波特;
//                 USB-Serial-JTAG 是原生 USB-CDC,波特率仅为名义值)。
//   设备 -> 主机: 头行 "FAP_SCREENSHOT_V1 <宽> <高> RGB565LE <字节数>\n"
//                 紧跟 <字节数> 字节小端 RGB565 行主序像素。
//
// 实现要点:
//   * 只读:整屏快照 + 回传,不重启/刷机/改设置/暴露凭据;
//   * 帧缓冲来自 lv_snapshot_take 的拷贝,锁只覆盖渲染瞬间,
//     回传期间 UI 照常刷新(USB-CDC 很快,但仍不阻塞动画);
//   * 回传期间临时关闭日志输出,避免日志字节混入二进制流
//     (主机按声明长度精确读取,任何插入字节都会错位)。
#include "fap_screenshot.h"
#include "bsp_display.h"       // bsp_lvgl_lock / bsp_lvgl_unlock
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "fap_shot";

#define FAP_CMD        "FAP_SCREENSHOT_V1"
#define FAP_CMD_LEN    (sizeof(FAP_CMD) - 1)
#define FAP_LINE_MAX   24    // 命令行缓冲:略大于命令长度,溢出即丢弃重来
#define FAP_TASK_STACK 8192  // lv_snapshot 在本任务内做整屏软件渲染,栈要余量
#define FAP_TASK_PRIO  3     // 低于 LVGL(4)/采集:截屏是偶发操作,不抢交互

// 循环写直到全部发出:usb_serial_jtag_write_bytes 可能分包返回部分长度。
static void write_all(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        int n = usb_serial_jtag_write_bytes(p, len, pdMS_TO_TICKS(2000));
        if (n <= 0) return;  // 主机掉线:放弃本次,任务继续等下一条命令
        p += n;
        len -= (size_t)n;
    }
}

// 渲染当前屏幕并以协议格式回传。快照失败(堆不足)只记日志不应答,
// 主机会以超时给出明确错误,不破坏流格式。
static void dump_screen(void) {
    lv_draw_buf_t *snap = NULL;
    if (bsp_lvgl_lock(2000)) {
        snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
        bsp_lvgl_unlock();
    }
    if (!snap) {
        ESP_LOGE(TAG, "快照失败:堆不足或 LVGL 忙,空闲堆 %lu 字节",
                 (unsigned long)esp_get_free_heap_size());
        return;
    }
    // LVGL 默认行距无填充(240*2=480B),防御性核对,不符则宁可不应答。
    uint32_t len = (uint32_t)snap->data_size;
    if (len != (uint32_t)snap->header.w * snap->header.h * 2) {
        ESP_LOGE(TAG, "行距含填充(%lu),不符合 RGB565LE 紧排约定", (unsigned long)len);
        lv_draw_buf_destroy(snap);
        return;
    }

    char header[48];
    int n = snprintf(header, sizeof(header), "%s %d %d RGB565LE %lu\n",
                     FAP_CMD, snap->header.w, snap->header.h, (unsigned long)len);
    int w = snap->header.w, h = snap->header.h;
    esp_log_level_set("*", ESP_LOG_NONE);  // 传输窗口内禁一切日志,见文件头注释
    write_all(header, (size_t)n);
    write_all(snap->data, len);
    esp_log_level_set("*", ESP_LOG_INFO);  // 恢复到 sdkconfig 的默认日志级别
    lv_draw_buf_destroy(snap);
    ESP_LOGI(TAG, "已回传截屏 %dx%d(%lu 字节)", w, h, (unsigned long)len);
}

// 串口应答任务:攒行匹配命令,其余输入(日志回显、换行等)一律忽略。
static void fap_task(void *arg) {
    (void)arg;
    char line[FAP_LINE_MAX];
    size_t used = 0;
    uint8_t buf[16];
    for (;;) {
        // 三重防空转:任何一条失败都必须让出 CPU——高优先级忙等会饿死
        // 空闲任务、触发任务看门狗,设备反复重启,表现为屏幕一直闪。
        if (!usb_serial_jtag_is_driver_installed()) {
            vTaskDelay(pdMS_TO_TICKS(500));  // 驱动未就绪:低频重试
            continue;
        }
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(500));
        if (n < 0) {
            vTaskDelay(pdMS_TO_TICKS(200));  // 读取出错:退避后再试
            continue;
        }
        if (n == 0) continue;  // 无输入:超时后继续等,任务常驻不退出
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\r') continue;
            if (c != '\n') {
                if (used < sizeof(line) - 1) {
                    line[used++] = c;
                } else {
                    used = 0;  // 超长行:丢弃,防止半截内容被误判成命令
                }
                continue;
            }
            if (used == FAP_CMD_LEN && memcmp(line, FAP_CMD, FAP_CMD_LEN) == 0) {
                dump_screen();
            }
            used = 0;
        }
    }
}

void fap_screenshot_start(void) {
    xTaskCreate(fap_task, "fap_shot", FAP_TASK_STACK, NULL, FAP_TASK_PRIO, NULL);
}
