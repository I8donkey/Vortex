// ============================================================
// base64_module.h — Base64 编解码模块
// 全部返回 str，解释器与编译端一致。
// ============================================================
#ifndef VORTEX_BASE64_MODULE_H
#define VORTEX_BASE64_MODULE_H

#include <unordered_map>
#include <string>
#include <memory>

namespace vortex {
struct Value; using ValuePtr = std::shared_ptr<Value>;

void register_base64_module(std::unordered_map<std::string, ValuePtr>& std_modules);

} // namespace vortex
#endif // VORTEX_BASE64_MODULE_H