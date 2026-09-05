// ============================================================
// str_module.h — 字符串工具模块（纯标量，解释器与编译端一致）
// upper/lower/trim/ltrim/rtrim/len/find/rfind/count/contains/
// starts_with/ends_with/replace/slice/char_at/repeat/pad_left/pad_right
// ============================================================
#ifndef VORTEX_STR_MODULE_H
#define VORTEX_STR_MODULE_H

#include <unordered_map>
#include <string>
#include <memory>

namespace vortex {
struct Value; using ValuePtr = std::shared_ptr<Value>;

void register_str_module(std::unordered_map<std::string, ValuePtr>& std_modules);

} // namespace vortex
#endif // VORTEX_STR_MODULE_H