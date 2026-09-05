// ============================================================
// regex_module.cpp — 正则表达式模块（std::regex）
// 全部返回标量，解释器与编译端一致。
// ============================================================
#include "regex_module.h"
#include "value.h"
#include "interpreter.h"
#include <regex>
#include <string>

namespace vortex {

static bool re_valid(const std::string& pat) {
    try { std::regex r(pat); return true; }
    catch (...) { return false; }
}
static bool re_match(const std::string& pat, const std::string& s) {
    try { return std::regex_match(s, std::regex(pat)); }
    catch (...) { return false; }
}
static bool re_search(const std::string& pat, const std::string& s) {
    try { return std::regex_search(s, std::regex(pat)); }
    catch (...) { return false; }
}
static std::string re_find(const std::string& pat, const std::string& s) {
    try {
        std::smatch m;
        if (std::regex_search(s, m, std::regex(pat))) return m.str();
    } catch (...) {}
    return "";
}
static std::string re_find_all_join(const std::string& pat, const std::string& s) {
    // 提取所有匹配，用逗号连成一个字符串（避免 list 编译限制）
    std::string out;
    try {
        std::regex re(pat);
        auto begin = std::sregex_iterator(s.begin(), s.end(), re);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            if (!out.empty()) out += ", ";
            out += it->str();
        }
    } catch (...) {}
    return out;
}
static const char* re_meta = R"(\.^$|?*+()[]{})";
static int re_is_meta(char c) {
    const char* p = re_meta;
    while (*p) { if (*p == c) return 1; ++p; }
    return 0;
}
// split(pat, s)：按正则匹配切分，返回字符串列表
static ValuePtr re_split(const std::string& pat, const std::string& s) {
    auto r = Value::make_list();
    try {
        std::regex re(pat);
        auto begin = std::sregex_token_iterator(s.begin(), s.end(), re, -1);
        auto end = std::sregex_token_iterator();
        for (auto it = begin; it != end; ++it)
            r->list_rep->push_back(Value::make_str(it->str()));
    } catch (...) {
        r->list_rep->push_back(Value::make_str(s));
    }
    return r;
}
static std::string re_escape(const std::string& s) {
    // 参考 Python re.escape：给正则元字符加反斜杠转义
    std::string out;
    out.reserve(s.size() * 2);
    for (char c : s) {
        if (re_is_meta(c)) out += '\\';
        out += c;
    }
    return out;
}
static std::string re_replace(const std::string& pat, const std::string& s, const std::string& repl) {
    try { return std::regex_replace(s, std::regex(pat), repl); }
    catch (...) { return s; }
}

void register_regex_module(std::unordered_map<std::string, ValuePtr>& std_modules) {
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
        u[n] = mk_fn("regex", n, a0, a1, std::move(f));
    };

    add("valid", 1, 1, [&](const ValueVec& a) {
        return Value::make_bool(re_valid(a[0]->to_string()));
    });
    add("match", 2, 2, [&](const ValueVec& a) {
        return Value::make_bool(re_match(a[0]->to_string(), a[1]->to_string()));
    });
    add("search", 2, 2, [&](const ValueVec& a) {
        return Value::make_bool(re_search(a[0]->to_string(), a[1]->to_string()));
    });
    add("escape", 1, 1, [&](const ValueVec& a) {
        return Value::make_str(re_escape(a[0]->to_string()));
    });
    add("find", 2, 2, [&](const ValueVec& a) {
        return Value::make_str(re_find(a[0]->to_string(), a[1]->to_string()));
    });
    add("split", 2, 2, [&](const ValueVec& a) {
        return re_split(a[0]->to_string(), a[1]->to_string());
    });
    add("find_all", 2, 2, [&](const ValueVec& a) {
        return Value::make_str(re_find_all_join(a[0]->to_string(), a[1]->to_string()));
    });
    add("replace", 3, 3, [&](const ValueVec& a) {
        return Value::make_str(re_replace(a[0]->to_string(), a[1]->to_string(), a[2]->to_string()));
    });
    add("count", 2, 2, [&](const ValueVec& a) {
        // 返回匹配个数（用逗号分隔串来数，稳妥）
        std::string all = re_find_all_join(a[0]->to_string(), a[1]->to_string());
        if (all.empty()) return Value::make_int(0);
        long long n = 1;
        size_t p = 0;
        while ((p = all.find(", ", p)) != std::string::npos) { ++n; p += 2; }
        return Value::make_int(n);
    });

    std_modules["regex"] = mod;
}

} // namespace vortex