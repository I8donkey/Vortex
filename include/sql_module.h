// ============================================================
// sql_module.h — SQLite 模块
// 解释器端：open/close/execute/query/query_one/table_exists。
// 编译端：handle 以不透明 ptr 传递，转发 open/close/execute。
// ============================================================
#ifndef VORTEX_SQL_MODULE_H
#define VORTEX_SQL_MODULE_H

#include <unordered_map>
#include <string>
#include <memory>

namespace vortex {
struct Value; using ValuePtr = std::shared_ptr<Value>;

void register_sql_module(std::unordered_map<std::string, ValuePtr>& std_modules);

} // namespace vortex
#endif // VORTEX_SQL_MODULE_H