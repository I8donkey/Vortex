// ============================================================
// os_module.h — 操作系统/环境模块（参考 Python os）
// 全部返回标量，解释器与编译器均可完整使用。
// ============================================================
#ifndef VORTEX_OS_MODULE_H
#define VORTEX_OS_MODULE_H

#include <unordered_map>
#include <string>
#include <memory>
#include <vector>

namespace vortex {
struct Value; using ValuePtr = std::shared_ptr<Value>;

void register_os_module(std::unordered_map<std::string, ValuePtr>& std_modules);

// 由宿主(main)注入命令行参数（跳过程序名），供 os.argc / os.arg 使用
void os_set_global_args(const std::vector<std::string>& args);

} // namespace vortex
#endif // VORTEX_OS_MODULE_H