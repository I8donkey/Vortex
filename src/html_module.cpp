// ============================================================
// html_module.cpp — HTML 模块：转义/去转义/去标签/取文本
// 两端一致，均返回标量 str。
// ============================================================
#include "html_module.h"
#include "value.h"
#include "interpreter.h"
#include <string>

namespace vortex {

static std::string html_escape(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (char ch : s) {
        switch (ch) {
            case '&': r += "&amp;"; break;
            case '<': r += "&lt;"; break;
            case '>': r += "&gt;"; break;
            case '"': r += "&quot;"; break;
            case '\'': r += "&#39;"; break;
            default: r += ch;
        }
    }
    return r;
}

static std::string html_unescape(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            size_t semi = s.find(';', i);
            if (semi != std::string::npos && semi - i <= 12) {
                std::string ent = s.substr(i + 1, semi - i - 1);
                if (ent == "amp")   { r += '&'; i = semi; continue; }
                if (ent == "lt")    { r += '<'; i = semi; continue; }
                if (ent == "gt")    { r += '>'; i = semi; continue; }
                if (ent == "quot")  { r += '"'; i = semi; continue; }
                if (ent == "apos" || ent == "#39") { r += '\''; i = semi; continue; }
                if (ent == "nbsp")  { r += ' '; i = semi; continue; }
                if (!ent.empty() && ent[0] == '#') {
                    bool ok = true; unsigned long code = 0;
                    for (size_t j = 1; j < ent.size(); ++j) {
                        if (ent[j] < '0' || ent[j] > '9') { ok = false; break; }
                        code = code * 10 + (unsigned long)(ent[j] - '0');
                    }
                    if (ok && code > 0) { r += (char)code; i = semi; continue; }
                }
            }
            r += '&';
        } else {
            r += s[i];
        }
    }
    return r;
}

static std::string html_strip_tags(const std::string& s) {
    std::string r; r.reserve(s.size());
    bool in_tag = false;
    for (char ch : s) {
        if (ch == '<') { in_tag = true; continue; }
        if (ch == '>') { in_tag = false; continue; }
        if (!in_tag) r += ch;
    }
    return html_unescape(r);
}

void register_html_module(std::unordered_map<std::string, ValuePtr>& std_modules) {
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
        u[n] = mk_fn("html", n, a0, a1, std::move(f));
    };

    add("escape", 1, 1, [&](const ValueVec& a) {
        return Value::make_str(html_escape(a[0]->to_string()));
    });
    add("unescape", 1, 1, [&](const ValueVec& a) {
        return Value::make_str(html_unescape(a[0]->to_string()));
    });
    add("strip_tags", 1, 1, [&](const ValueVec& a) {
        return Value::make_str(html_strip_tags(a[0]->to_string()));
    });

    std_modules["html"] = mod;
}

} // namespace vortex