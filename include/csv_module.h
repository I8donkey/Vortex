// ============================================================
// csv_module.h — CSV 模块
// 标量导向：to_line / count_fields / field_at / quote / parse_line。
// to_line 与 field_at/count 编译端可转发；parse_line 返回 str 视作
// 紧凑行（同 to_line 可再拆分），解释器端亦可。
// ============================================================
#ifndef VORTEX_CSV_MODULE_H
#define VORTEX_CSV_MODULE_H

#include <unordered_map>
#include <string>
#include <memory>

namespace vortex {
struct Value; using ValuePtr = std::shared_ptr<Value>;

void register_csv_module(std::unordered_map<std::string, ValuePtr>& std_modules);

} // namespace vortex
#endif // VORTEX_CSV_MODULE_H