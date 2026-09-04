// ============================================================
// vortex_modules.cpp — 扩展模块统一入口
// ============================================================
#include "vortex_modules.h"
#include "cuda_module.h"
#include "game2d_module.h"
#include "render3d_module.h"
#include "thread_module.h"
#include "log_module.h"
#include "gui_module.h"

namespace vortex {

void register_extension_modules(std::unordered_map<std::string, ValuePtr>& std_modules) {
    // 按依赖自底向上注册
    register_cuda_module(std_modules);
    register_game2d_module(std_modules);
    register_render3d_module(std_modules);
    register_thread_module(std_modules);
    register_log_module(std_modules);
    register_gui_module(std_modules);
}

} // namespace vortex
