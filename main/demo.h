// main/demo.h -- 应用页面实现的统一接口。
//
// 本分支是独立应用:开机由 main.c 直接加载音量检测页,
// 不再有组件演示菜单。页面文件实现 enter/exit/key 三个函数;
// exit 在当前启动流程中不会被调用,保留它是页面生命周期契约的一部分
// (页面必须能被干净地卸载,便于未来加入导航/休眠前的收尾)。
#pragma once

#include "bsp_button.h"

// 音量检测页(定义在 demo_sound_meter.c)
void demo_sound_meter_enter(void); void demo_sound_meter_exit(void);
void demo_sound_meter_key(bsp_btn_t btn, bsp_btn_ev_t ev);
