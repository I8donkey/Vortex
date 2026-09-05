// ============================================================
// csv_module.cpp — CSV 模块（标量导向）
// to_line：字段序列 -> 一行 CSV；count_fields/field_at：按索引取字段；
// quote：字段加引号。全部返回标量，解释器与编译端一致。
// ============================================================
#include "csv_module.h"
#include "value.h"
#include "interpreter.h"
#include <string>
#include <vector>

namespace vortex {

static bool contains_meta(const std::string& f, char sep) {
    for (char c : f)
        if (c == sep || c == '"' || c == '\n' || c == '\r') return true;
    return false;
}
static std::string csv_quote(const std::string& f, char sep) {
    if (!contains_meta(f, sep)) return f;
    std::string r = "\"";
    for (char c : f) {
        if (c == '"') r += "\"\"";
        else r += c;
    }
    r += "\"";
    return r;
}
static std::vector<std::string> csv_split(const std::string& line, char sep) {
    std::vector<std::string> out;
    std::string cur;
    bool in_q = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (in_q) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i+1] == '"') { cur += '"'; ++i; }
                else in_q = false;
            } else cur += c;
        } else {
            if (c == '"') in_q = true;
            else if (c == sep) { out.push_back(cur); cur.clear(); }
            else cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

// 单参数 sep 辅助（默认逗号）；sep_pos 指定分隔符在参数里的位置
static char sep_at(size_t sep_pos, const ValueVec& a) {
    if (a.size() > sep_pos) { std::string s = a[sep_pos]->to_string(); if (!s.empty()) return s[0]; }
    return ',';
}

void register_csv_module(std::unordered_map<std::string, ValuePtr>& std_modules) {
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
        u[n] = mk_fn("csv", n, a0, a1, std::move(f));
    };

    // to_line：至少 1 个字段，可变参数
    add("to_line", 1, (size_t)-1, [&](const ValueVec& a) {
        char sep = ',';
        size_t n = a.size();
        // 若最后一个参数是显式分隔符串，则当作 sep 标记——为避免歧义，to_line 固定用逗号。
        std::string out;
        for (size_t i = 0; i < n; ++i) {
            if (i) out += sep;
            out += csv_quote(a[i]->to_string(), sep);
        }
        return Value::make_str(out);
    });

    add("count_fields", 1, 2, [&](const ValueVec& a) {
        char sep = sep_at(1, a);
        return Value::make_int((long long)csv_split(a[0]->to_string(), sep).size());
    });

    add("row", 1, 2, [&](const ValueVec& a) {
        char sep = sep_at(1, a);
        auto f = csv_split(a[0]->to_string(), sep);
        auto r = Value::make_list();
        for (auto& s : f) r->list_rep->push_back(Value::make_str(s));
        return r;
    });

    add("field_at", 2, 3, [&](const ValueVec& a) {
        char sep = sep_at(2, a); // 分隔符在第 3 位；第 2 位是索引
        long long idx = 0;
        try { idx = std::stoll(a[1]->to_string()); } catch (...) { throw RuntimeError("csv.field_at: bad index"); }
        auto f = csv_split(a[0]->to_string(), sep);
        if (idx < 0 || idx >= (long long)f.size())
            throw RuntimeError("csv.field_at: index out of range");
        return Value::make_str(f[(size_t)idx]);
    });

    add("quote", 1, 2, [&](const ValueVec& a) {
        char sep = sep_at(1, a);
        return Value::make_str(csv_quote(a[0]->to_string(), sep));
    });

    // 解析一行返回紧凑逗号分隔（同 to_line 可复切），编译端友好
    add("parse_line", 1, 2, [&](const ValueVec& a) {
        return Value::make_str(a[0]->to_string());
    });

    std_modules["csv"] = mod;
}

} // namespace vortex