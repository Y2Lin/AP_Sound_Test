// main/fap_screenshot.h -- FAP_SCREENSHOT_V1 串口截屏协议(社区发布助手)。
//
// 发布流程要求固件实现该协议作为"真机运行"凭证:主机(发布工具)经
// USB-CDC 发来一行 ASCII 命令,设备回一行头 + RGB565LE 帧缓冲。
// 命令严格只读:不重启、不刷机、不改设置、不暴露任何设备凭据。
#pragma once

// 启动截屏应答任务(内部只创建一次)。在 UI 就绪后调用。
void fap_screenshot_start(void);
