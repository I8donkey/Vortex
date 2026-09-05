// ============================================================
// zip_module.h — zip 归档模块（存储/解压，zlib）
// 解释器端：add / extract / count / has / names。
// 编译端：runtime vor_zip_*（add/extract/count/has）。
// ============================================================
#ifndef VORTEX_ZIP_MODULE_H
#define VORTEX_ZIP_MODULE_H

#include <unordered_map>
#include <string>
#include <memory>

namespace vortex {
struct Value; using ValuePtr = std::shared_ptr<Value>;

void register_zip_module(std::unordered_map<std::string, ValuePtr>& std_modules);

} // namespace vortex
#endif // VORTEX_ZIP_MODULE_H