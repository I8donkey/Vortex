// ============================================================
// datetime_module.h — 日期时间模块
// 全部返回标量（str/int/float），解释器与编译端一致。
// ============================================================
#ifndef VORTEX_DATETIME_MODULE_H
#define VORTEX_DATETIME_MODULE_H

#include <unordered_map>
#include <string>
#include <memory>

namespace vortex {
struct Value; using ValuePtr = std::shared_ptr<Value>;

void register_datetime_module(std::unordered_map<std::string, ValuePtr>& std_modules);

} // namespace vortex
#endif // VORTEX_DATETIME_MODULE_H