// ============================================================================
//  模态对话框框架（自绘，无资源脚本依赖）
//  ---------------------------------------------------------------------------
//    * ModalPage   —— 页面接口：内容绘制 / 布局 / 命令 / 按键
//    * RunModal    —— 通用模态宿主：圆角 + 暗色 + 键盘可达（Enter/Esc）
//    * ConfirmDialog / AboutDialog —— 两个具体页面
// ============================================================================
#pragma once

#include "common.h"
#include "ui/controls.h"

namespace mz {
namespace ui {

// 模态页面：内容区（不含标题栏与按钮行）内的一切自理
struct ModalPage {
    virtual ~ModalPage() = default;
    int result = 0;   // 关闭时带回的按钮 ID

    virtual void Draw(HDC dc, const RECT& contentRc) = 0;
    virtual void Layout(const RECT& contentRc) {}
    // 返回 true 表示关闭对话框；框架已把触发按钮 ID 写入 result
    virtual bool OnCommand(int id) { return false; }
    virtual void OnKey(WPARAM vk) {}   // VK_RETURN / VK_ESCAPE
    // 内容区点击（客户区坐标）；页面自行判断命中，例如卡片选择
    virtual void OnClick(int x, int y) {}
    // 悬停反馈（卡片高亮等）；返回 true 表示需要重绘内容区
    virtual bool OnMouseMove(int x, int y) { return false; }
    virtual bool OnMouseLeave() { return false; }
};

struct ModalOptions {
    std::wstring okText = L"确定";
    std::wstring cancelText = L"取消";
    bool showCancel = true;
    bool showExtra = false;    // 「访问官网」按钮（关于页用）
    bool dangerOk = false;     // 确定按钮用危险色（清理确认用）
};

// 按钮 ID
constexpr int kModalOk = 1;
constexpr int kModalCancel = 2;
constexpr int kModalExtra = 3;
constexpr int kModalClose = 4;   // 标题栏 ✕（等同取消）

// 运行模态对话框（阻塞宿主窗口）；返回按钮 ID
int RunModal(HWND host, const std::wstring& title, int width, int height,
             ModalPage* page, const ModalOptions& opt = {});

// 确认对话框：返回 true = 用户确认
bool ConfirmDialog(HWND host, const std::wstring& title, const std::wstring& body,
                   const std::wstring& okText, const std::wstring& cancelText,
                   bool danger = false);

// 关于对话框
void AboutDialog(HWND host, int moduleCount);

// 主题选择对话框（4 套主题卡片，点击即切换并持久化）
void ThemePickerDialog(HWND host);

} // namespace ui
} // namespace mz
