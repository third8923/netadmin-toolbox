// ============================================================================
//  主窗口 —— AppWindow
//  ---------------------------------------------------------------------------
//  旧版 gui.cpp（835 行、全局指针 G、直绘屏幕）的彻底重构：
//    * 单一职责类，零全局可变状态；句柄/线程/字体全部 RAII 管理
//    * 侧栏 / 搜索 / 结果列表 / 日志 / 状态栏各自独立，经消息路由协作
//    * 与 ModuleRegistry 解耦：界面只认识 IModule，不认识任何具体功能
//    * Win11 沉浸式暗色标题栏 + 圆角 + 暗色滚动条 + 双缓冲零闪烁
//    * PerMonitorV2 DPI：跨屏拖动自动重排
// ============================================================================
#pragma once

#include "common.h"
#include "module.h"
#include "ui/controls.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace mz {

int RunAppWindow(HINSTANCE hInst, int nCmdShow);

} // namespace mz
