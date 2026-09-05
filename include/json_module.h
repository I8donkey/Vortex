// ============================================================
// json_module.h — JSON 模块
// 标量导向：valid / parse_str / parse_int / parse_float / parse_bool /
// stringify。全部返回标量，解释器与编译端一致。
// ============================================================
#ifndef VORTEX_JSON_MODULE_H
#define VORTEX_JSON_MODULE_H

#include <unordered_map>
#include <string>
#include <memory>

namespace vortex {
struct Value; using ValuePtr = std::shared_ptr<Value>;

void register_json_module(std::unordered_map<std::string, ValuePtr>& std_modules);

} // namespace vortex
#endif // VORTEX_JSON_MODULE_H