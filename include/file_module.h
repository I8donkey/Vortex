// ============================================================
// file_module.h — 文件系统模块（参考 Python os.path / open）
// 解释器端：返回原生 Value（str / list / bool / int）。
// 编译端：runtime 中 vor_file_* 转发（标量/str 子集）。
// ============================================================
#ifndef VORTEX_FILE_MODULE_H
#define VORTEX_FILE_MODULE_H

#include <unordered_map>
#include <string>
#include <memory>

namespace vortex {
struct Value; using ValuePtr = std::shared_ptr<Value>;

void register_file_module(std::unordered_map<std::string, ValuePtr>& std_modules);

} // namespace vortex
#endif // VORTEX_FILE_MODULE_H