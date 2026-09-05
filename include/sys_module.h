// ============================================================
// sys_module.h — 系统/运行时模块（参考 Python sys）
// 全部返回标量（str/int/float），解释器与编译器语义一致。
// ============================================================
#ifndef VORTEX_SYS_MODULE_H
#define VORTEX_SYS_MODULE_H

#include <unordered_map>
#include <string>
#include <memory>

namespace vortex {
struct Value; using ValuePtr = std::shared_ptr<Value>;

void register_sys_module(std::unordered_map<std::string, ValuePtr>& std_modules);

} // namespace vortex
#endif // VORTEX_SYS_MODULE_H