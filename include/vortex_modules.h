// ============================================================
// vortex_modules.h — 扩展模块统一注册入口
// 各子模块（game2d / render3d）通过此头文件向
// 解释器注册自己的 std_module 表项。
// 每个模块均独立编译，可通过 CMake 开关整体启用或禁用。
// ============================================================
#ifndef VORTEX_MODULES_H
#define VORTEX_MODULES_H

#include "value.h"
#include <unordered_map>
#include <string>

namespace vortex {

class Interpreter;

// 注册所有已启用的扩展模块到 Interpreter::std_modules_ 表中。
// 当依赖（SDL2 / OpenGL）缺失时，对应模块依然会被注册，
// 但运行时调用会抛出友好错误或退化为 CPU 实现，以保证兼容性。
void register_extension_modules(std::unordered_map<std::string, ValuePtr>& std_modules);

// 子模块独立注册函数（供 register_extension_modules 内部调用）
void register_game2d_module (std::unordered_map<std::string, ValuePtr>& std_modules);
void register_render3d_module(std::unordered_map<std::string, ValuePtr>& std_modules);
void register_thread_module (std::unordered_map<std::string, ValuePtr>& std_modules);
void register_log_module    (std::unordered_map<std::string, ValuePtr>& std_modules);
void register_gui_module    (std::unordered_map<std::string, ValuePtr>& std_modules);

} // namespace vortex
#endif // VORTEX_MODULES_H
