// ============================================================
// html_module.h — HTML 模块
// 解释器端：escape/unescape/strip_tags/parse_text。
// 编译端：runtime vor_html_*（标量子集，模式同 xml）。
// ============================================================
#ifndef VORTEX_HTML_MODULE_H
#define VORTEX_HTML_MODULE_H

#include <unordered_map>
#include <string>
#include <memory>

namespace vortex {
struct Value; using ValuePtr = std::shared_ptr<Value>;

void register_html_module(std::unordered_map<std::string, ValuePtr>& std_modules);

} // namespace vortex
#endif // VORTEX_HTML_MODULE_H