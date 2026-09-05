// ============================================================
// hash_module.h — 哈希模块
// md5 / sha1 / sha256：输入字符串，输出小写十六进制 str。
// ============================================================
#ifndef VORTEX_HASH_MODULE_H
#define VORTEX_HASH_MODULE_H

#include <unordered_map>
#include <string>
#include <memory>

namespace vortex {
struct Value; using ValuePtr = std::shared_ptr<Value>;

void register_hash_module(std::unordered_map<std::string, ValuePtr>& std_modules);

} // namespace vortex
#endif // VORTEX_HASH_MODULE_H