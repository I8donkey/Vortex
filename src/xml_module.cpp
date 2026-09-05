// ============================================================
// xml_module.cpp — XML 模块（转义/取文本）
// 两端一致：escape / unescape / parse_text 均返回标量 str。
// ============================================================
#include "xml_module.h"
#include "value.h"
#include "interpreter.h"
#include <string>

namespace vortex {

static std::string xml_escape(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (char ch : s) {
        switch (ch) {
            case '&': r += "&amp;"; break;
            case '<': r += "&lt;"; break;
            case '>': r += "&gt;"; break;
            case '"': r += "&quot;"; break;
            case '\'': r += "&apos;"; break;
            default: r += ch;
        }
    }
    return r;
}

static std::string xml_unescape(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            size_t semi = s.find(';', i);
            if (semi != std::string::npos && semi - i <= 8) {
                std::string ent = s.substr(i + 1, semi - i - 1);
                if (ent == "amp") { r += '&'; i = semi; continue; }
                if (ent == "lt")  { r += '<'; i = semi; continue; }
                if (ent == "gt")  { r += '>'; i = semi; continue; }
                if (ent == "quot"){ r += '"'; i = semi; continue; }
                if (ent == "apos"){ r += '\''; i = semi; continue; }
                if (ent.size() > 1 && ent[0] == '#') {
                    unsigned long code = 0;
                    bool ok = true;
                    for (size_t j = 1; j < ent.size(); ++j) {
                        if (ent[j] < '0' || ent[j] > '9') { ok = false; break; }
                        code = code * 10 + (unsigned long)(ent[j] - '0');
                    }
                    if (ok && ent.size() > 1 && ent[1] != '#') {
                        // ent[0]=='#' numeric, ent[1] is first digit
                    }
                }
            }
            r += '&';
        } else {
            r += s[i];
        }
    }
    return r;
}

// 提取第一个 <tag>...</tag> 的文本内容；找不到返回空串
static std::string xml_parse_text(const std::string& xml, const std::string& tag) {
    std::string open = "<" + tag;
    size_t pos = xml.find(open);
    if (pos == std::string::npos) return "";
    size_t gt = xml.find('>', pos);
    if (gt == std::string::npos) return "";
    size_t close = xml.find("</" + tag + ">", gt + 1);
    if (close == std::string::npos) return "";
    return xml_unescape(xml.substr(gt + 1, close - gt - 1));
}

void register_xml_module(std::unordered_map<std::string, ValuePtr>& std_modules) {
    auto mod = Value::make_module();
    auto& u = *mod->module_rep;

    auto mk_fn = [](const std::string& mname, const std::string& name, size_t min_a, size_t max_a,
                    std::function<ValuePtr(const ValueVec&)> fn) {
        auto fv = std::make_shared<FunctionValue>();
        fv->name = name; fv->is_builtin = true;
        fv->builtin_fn = [mname, name, min_a, max_a, fn](const ValueVec& args, Environment&) -> ValuePtr {
            if (args.size() < min_a || (max_a != (size_t)-1 && args.size() > max_a))
                throw RuntimeError(mname + "." + name + " expects " +
                    std::to_string(min_a) + "~" + std::to_string(max_a) + " args, got " +
                    std::to_string(args.size()));
            return fn(args);
        };
        auto v = Value::make_none(); v->type = ValueType::Function; v->fn_rep = fv;
        return v;
    };
    auto add = [&](const std::string& n, size_t a0, size_t a1,
                   std::function<ValuePtr(const ValueVec&)> f) {
        u[n] = mk_fn("xml", n, a0, a1, std::move(f));
    };

    add("escape", 1, 1, [&](const ValueVec& a) {
        return Value::make_str(xml_escape(a[0]->to_string()));
    });
    add("unescape", 1, 1, [&](const ValueVec& a) {
        return Value::make_str(xml_unescape(a[0]->to_string()));
    });
    add("parse_text", 2, 2, [&](const ValueVec& a) {
        return Value::make_str(xml_parse_text(a[0]->to_string(), a[1]->to_string()));
    });

    std_modules["xml"] = mod;
}

} // namespace vortex