// ============================================================
// ModuleLoader.h — vortexcc 编译期模块解析
// 把主程序 import 的用户 .vt / .vtp 模块解析、改名并合并进单一编译单元。
// ============================================================
#ifndef VORTEX_MODULE_LOADER_H
#define VORTEX_MODULE_LOADER_H

#include "ast.h"
#include <string>
#include <vector>

namespace vortex {
namespace cg {

// 解析主（含用户模块）并合并成一个 Program；模块顶层符号改名为 "<mod>.<name>"。
// main_src: 主 .vt 源码；main_path: 主文件路径（用于定位模块搜索目录，可为空）。
// 成功返回 true 并填充 combined；失败返回 false 并填 err。
bool collect_modules(const std::string& main_src, const std::string& main_path,
                     Program& combined, std::string& err);

} // namespace cg
} // namespace vortex

#endif // VORTEX_MODULE_LOADER_H