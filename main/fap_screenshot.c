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
#include "bsp_pins.h"          // BSP_LCD_W / BSP_LCD_H(快照缓冲尺寸)
#include "driver/usb_serial_jtag_vfs.h"  // 控制台 VFS 切到驱动通道
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

// 渲染当前屏幕并以协议格式回传。失败只记日志不应答,主机会以超时给出
// 明确错误,不破坏流格式。
//
// 整屏快照缓冲:静态预留。运行时堆虽然还有 ~220KB,却拿不出 153600
// 字节的连续块(碎片化),动态分配的快照必然失败;编译期预留则永不受
// 碎片影响。64 字节对齐满足软件渲染的行距/搬运要求。
#define FAP_SNAP_BYTES ((uint32_t)BSP_LCD_W * BSP_LCD_H * 2)
static lv_draw_buf_t s_snap_desc;
static uint8_t s_snap_buf[FAP_SNAP_BYTES] __attribute__((aligned(64)));

static void dump_screen(void) {
    if (!bsp_lvgl_lock(2000)) {
        ESP_LOGE(TAG, "拿不到 LVGL 锁,放弃本次截屏");
        return;
    }
    // 用官方 lv_snapshot_take_to_buf 的内部模式:外部缓冲 + 描述符,
    // 渲染直接写入 s_snap_buf,不经过 LVGL 堆分配。
    lv_draw_buf_init(&s_snap_desc, BSP_LCD_W, BSP_LCD_H, LV_COLOR_FORMAT_RGB565,
                     0, s_snap_buf, FAP_SNAP_BYTES);
    lv_result_t r = lv_snapshot_take_to_draw_buf(lv_screen_active(),
                                                 LV_COLOR_FORMAT_RGB565,
                                                 &s_snap_desc);
    bsp_lvgl_unlock();
    if (r != LV_RESULT_OK) {
        ESP_LOGE(TAG, "快照渲染失败:屏幕外延绘制超出静态缓冲");
        return;
    }
    // 防御性核对:必须是紧排 RGB565 整屏,不符宁可不应答。
    uint32_t len = (uint32_t)s_snap_desc.data_size;
    if (s_snap_desc.header.w != BSP_LCD_W || s_snap_desc.header.h != BSP_LCD_H
            || len != FAP_SNAP_BYTES) {
        ESP_LOGE(TAG, "快照 %ldx%ld(%lu) 不符紧排约定",
                 (long)s_snap_desc.header.w, (long)s_snap_desc.header.h,
                 (unsigned long)len);
        return;
    }

    char header[48];
    int n = snprintf(header, sizeof(header), "%s %d %d RGB565LE %lu\n",
                     FAP_CMD, (int)BSP_LCD_W, (int)BSP_LCD_H,
                     (unsigned long)len);
    esp_log_level_set("*", ESP_LOG_NONE);  // 传输窗口内禁一切日志,见文件头注释
    write_all(header, (size_t)n);
    write_all(s_snap_buf, len);
    esp_log_level_set("*", ESP_LOG_INFO);  // 恢复到 sdkconfig 的默认日志级别
    ESP_LOGI(TAG, "已回传截屏 %dx%d(%lu 字节)", (int)BSP_LCD_W, (int)BSP_LCD_H,
             (unsigned long)len);
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
            if (c == '\r' || c == '\n') {
                used = 0;  // 行结束:清空滑窗,等下一条
                continue;
            }
            if (used < sizeof(line) - 1) {
                line[used++] = c;
            } else {
                // 滑窗已满:丢弃最旧字符,保持"最近 FAP_CMD_LEN 字节"语义
                memmove(line, line + 1, sizeof(line) - 2);
                used = sizeof(line) - 2;
                line[used++] = c;
            }
            // 滑窗触发:只要输入流中出现命令子串即应答,不依赖换行符
            // (Windows miniterm 的 Enter 处理可能丢 \n,严格行匹配会漏命令)
            if (used == FAP_CMD_LEN && memcmp(line, FAP_CMD, FAP_CMD_LEN) == 0) {
                ESP_LOGI(TAG, "收到截屏命令");
                dump_screen();
                used = 0;  // 避免残留字符连续误触发
            }
        }
    }
}

void fap_screenshot_start(void) {
    // 本应用不启动 REPL,而控制台日志走的是 VFS 寄存器直写通道,
    // 全固件没有任何人安装过 USB 串口驱动——不装驱动,read_bytes 会
    // 解引用空指针(这正是早期"反复重启闪屏"的根因)。这里按官方 REPL
    // 的标准组合补装驱动,并把控制台 VFS 切到驱动通道,收发共用一条路。
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t cfg = {
            .rx_buffer_size = 256,   // 命令行最长 18 字节,256 足够
            .tx_buffer_size = 1024,  // 截屏载荷 150KB 分块流式发送,稍大更顺
        };
        esp_err_t err = usb_serial_jtag_driver_install(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "串口驱动安装失败(%d),截屏协议不可用", err);
            return;
        }
    }
    usb_serial_jtag_vfs_use_driver();

    BaseType_t ok = xTaskCreate(fap_task, "fap_shot", FAP_TASK_STACK, NULL,
                                FAP_TASK_PRIO, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "截屏任务创建失败(栈 %d 字节)", FAP_TASK_STACK);
        return;
    }
    ESP_LOGI(TAG, "截屏协议就绪:等待 FAP_SCREENSHOT_V1 命令");
}
