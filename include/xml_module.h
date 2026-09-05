// ============================================================
// xml_module.h — XML 模块
// 解释器端：escape/unescape、parse 成 DOM 树（list 表示）。
// 编译端：runtime vor_xml_*（escape/unescape/parse_text 标量子集）。
// ============================================================
#ifndef VORTEX_XML_MODULE_H
#define VORTEX_XML_MODULE_H

#include <unordered_map>
#include <string>
#include <memory>

namespace vortex {
struct Value; using ValuePtr = std::shared_ptr<Value>;

void register_xml_module(std::unordered_map<std::string, ValuePtr>& std_modules);

} // namespace vortex
#endif // VORTEX_XML_MODULE_H