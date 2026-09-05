// ============================================================
// regex_module.h — 正则表达式模块
// 全部返回标量（bool/str/int），解释器与编译端均可完整使用。
// ============================================================
#ifndef VORTEX_REGEX_MODULE_H
#define VORTEX_REGEX_MODULE_H

#include <unordered_map>
#include <string>
#include <memory>

namespace vortex {
struct Value; using ValuePtr = std::shared_ptr<Value>;

void register_regex_module(std::unordered_map<std::string, ValuePtr>& std_modules);

} // namespace vortex
#endif // VORTEX_REGEX_MODULE_H