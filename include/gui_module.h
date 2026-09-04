// ============================================================
// gui_module.h — GUI 模块（基于 Qt 的脚本层 GUI 控件）
//
// API（import gui）：
//   gui.window(title, w, h)        — 创建主窗口
//   gui.label(win, text)           — 创建标签
//   gui.button(win, text)          — 创建按钮，返回控件句柄
//   gui.input(win, text)           — 创建输入框
//   gui.layout_v(win)              — 设置垂直布局
//   gui.layout_h(win)              — 设置水平布局
//   gui.set_text(ctrl, text)       — 设置控件文本
//   gui.get_text(ctrl)             — 获取控件文本
//   gui.show(win)                   — 显示窗口
//   gui.msgbox(title, text)        — 弹出消息框
//   gui.exec()                      — 进入事件循环
//   gui.quit()                      — 退出事件循环
//
//   gui.on_click(button, fn)       — 绑定点击回调
// ============================================================
#ifndef VORTEX_GUI_MODULE_H
#define VORTEX_GUI_MODULE_H

#include "value.h"
#include <memory>

// 全局前向声明：避免 vortex_core（非 Qt 库）依赖 Qt 头文件
// 实际 Qt 类型细节仅在 gui_module.cpp 内部使用
class QWidget;

namespace vortex {

// GUI 控件包装（存储 QWidget 指针）
struct GuiWidget {
    QWidget* widget = nullptr;
};

void register_gui_module(std::unordered_map<std::string, ValuePtr>& std_modules);

} // namespace vortex

#endif // VORTEX_GUI_MODULE_H
