// ============================================================
// str_module.cpp — 字符串工具模块（纯标量/字符串返回，可编译转发）
// ============================================================
#include "str_module.h"
#include "value.h"
#include "interpreter.h"
#include <string>
#include <cctype>

namespace vortex {

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static long long arg_int(const ValuePtr& v, const std::string& fn) {
    if (v->type == ValueType::Int) return v->int_val;
    if (v->type == ValueType::UInt) return (long long)v->uint_val;
    if (v->type == ValueType::Bool) return v->bool_val ? 1 : 0;
    if (v->type == ValueType::Float) return (long long)v->float_val;
    try { return std::stoll(v->to_string()); } catch (...) { throw RuntimeError("text." + fn + ": bad argument"); }
}

void register_str_module(std::unordered_map<std::string, ValuePtr>& std_modules) {
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
        u[n] = mk_fn("text", n, a0, a1, std::move(f));
    };

    add("upper", 1, 1, [&](const ValueVec& a) {
        std::string s = a[0]->to_string();
        for (auto& c : s) c = (char)std::toupper((unsigned char)c);
        return Value::make_str(s);
    });
    add("lower", 1, 1, [&](const ValueVec& a) {
        std::string s = a[0]->to_string();
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return Value::make_str(s);
    });
    add("title", 1, 1, [&](const ValueVec& a) {
        std::string s = a[0]->to_string();
        bool start = true;
        for (auto& c : s) {
            if (std::isalpha((unsigned char)c)) { if (start) c = (char)std::toupper((unsigned char)c); start = false; }
            else start = true;
        }
        return Value::make_str(s);
    });
    add("swapcase", 1, 1, [&](const ValueVec& a) {
        std::string s = a[0]->to_string();
        for (auto& c : s) {
            if (std::isupper((unsigned char)c)) c = (char)std::tolower((unsigned char)c);
            else if (std::islower((unsigned char)c)) c = (char)std::toupper((unsigned char)c);
        }
        return Value::make_str(s);
    });
    add("capitalize", 1, 1, [&](const ValueVec& a) {
        std::string s = a[0]->to_string();
        if (!s.empty()) {
            if (std::isalpha((unsigned char)s[0])) s[0] = (char)std::toupper((unsigned char)s[0]);
            for (size_t i = 1; i < s.size(); ++i)
                if (std::isalpha((unsigned char)s[i])) s[i] = (char)std::tolower((unsigned char)s[i]);
        }
        return Value::make_str(s);
    });
    auto str_pred = [&](const char* n, int (*pred)(int), bool any_alpha_required) {
        add(n, 1, 1, [&, n, pred, any_alpha_required](const ValueVec& a) {
            std::string s = a[0]->to_string();
            if (s.empty()) return Value::make_int(0);
            bool has_alpha = false;
            for (auto c : s) {
                if (!pred((unsigned char)c)) return Value::make_int(0);
                if (std::isalpha((unsigned char)c)) has_alpha = true;
            }
            if (any_alpha_required && !has_alpha) return Value::make_int(0);
            return Value::make_int(1);
        });
    };
    str_pred("isalpha", &std::isalpha, false);
    str_pred("isdigit", &std::isdigit, false);
    str_pred("isalnum", &std::isalnum, false);
    str_pred("isspace", &std::isspace, false);
    str_pred("isupper", &std::isupper, true);
    str_pred("islower", &std::islower, true);
    auto trim_impl = [](const std::string& s, bool left, bool right) {
        size_t b = 0, e = s.size();
        if (left)  while (b < e && is_ws(s[b])) ++b;
        if (right) while (e > b && is_ws(s[e-1])) --e;
        return s.substr(b, e - b);
    };
    add("trim", 1, 1, [&](const ValueVec& a) { return Value::make_str(trim_impl(a[0]->to_string(), true, true)); });
    add("ltrim", 1, 1, [&](const ValueVec& a) { return Value::make_str(trim_impl(a[0]->to_string(), true, false)); });
    add("rtrim", 1, 1, [&](const ValueVec& a) { return Value::make_str(trim_impl(a[0]->to_string(), false, true)); });
    // Python 别名
    add("strip",  1, 1, [&](const ValueVec& a) { return Value::make_str(trim_impl(a[0]->to_string(), true, true)); });
    add("lstrip", 1, 1, [&](const ValueVec& a) { return Value::make_str(trim_impl(a[0]->to_string(), true, false)); });
    add("rstrip", 1, 1, [&](const ValueVec& a) { return Value::make_str(trim_impl(a[0]->to_string(), false, true)); });

    add("len", 1, 1, [&](const ValueVec& a) { return Value::make_int((long long)a[0]->to_string().size()); });

    add("contains", 2, 2, [&](const ValueVec& a) {
        std::string s = a[0]->to_string(), sub = a[1]->to_string();
        return Value::make_int(s.find(sub) != std::string::npos ? 1 : 0);
    });
    add("starts_with", 2, 2, [&](const ValueVec& a) {
        std::string s = a[0]->to_string(), pre = a[1]->to_string();
        return Value::make_int(s.size() >= pre.size() && s.compare(0, pre.size(), pre) == 0 ? 1 : 0);
    });
    add("ends_with", 2, 2, [&](const ValueVec& a) {
        std::string s = a[0]->to_string(), suf = a[1]->to_string();
        return Value::make_int(s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0 ? 1 : 0);
    });
    // Python 名称别名（startswith / endswith）
    add("startswith", 2, 2, [&](const ValueVec& a) {
        std::string s = a[0]->to_string(), pre = a[1]->to_string();
        return Value::make_int(s.size() >= pre.size() && s.compare(0, pre.size(), pre) == 0 ? 1 : 0);
    });
    add("endswith", 2, 2, [&](const ValueVec& a) {
        std::string s = a[0]->to_string(), suf = a[1]->to_string();
        return Value::make_int(s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0 ? 1 : 0);
    });
    add("removeprefix", 2, 2, [&](const ValueVec& a) {
        std::string s = a[0]->to_string(), pre = a[1]->to_string();
        if (s.size() >= pre.size() && s.compare(0, pre.size(), pre) == 0)
            return Value::make_str(s.substr(pre.size()));
        return Value::make_str(s);
    });
    add("removesuffix", 2, 2, [&](const ValueVec& a) {
        std::string s = a[0]->to_string(), suf = a[1]->to_string();
        if (s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0)
            return Value::make_str(s.substr(0, s.size() - suf.size()));
        return Value::make_str(s);
    });
    add("find", 2, 2, [&](const ValueVec& a) {
        std::string s = a[0]->to_string(), sub = a[1]->to_string();
        size_t p = s.find(sub);
        return Value::make_int(p == std::string::npos ? -1 : (long long)p);
    });
    add("rfind", 2, 2, [&](const ValueVec& a) {
        std::string s = a[0]->to_string(), sub = a[1]->to_string();
        size_t p = s.rfind(sub);
        return Value::make_int(p == std::string::npos ? -1 : (long long)p);
    });
    add("count", 2, 2, [&](const ValueVec& a) {
        std::string s = a[0]->to_string(), sub = a[1]->to_string();
        if (sub.empty()) return Value::make_int(0);
        long long n = 0; size_t p = 0;
        while ((p = s.find(sub, p)) != std::string::npos) { ++n; p += sub.size(); }
        return Value::make_int(n);
    });
    add("replace", 3, 3, [&](const ValueVec& a) {
        std::string s = a[0]->to_string(), x = a[1]->to_string(), y = a[2]->to_string();
        if (!x.empty()) {
            size_t p = 0;
            while ((p = s.find(x, p)) != std::string::npos) { s.replace(p, x.size(), y); p += y.size(); }
        }
        return Value::make_str(s);
    });
    add("slice", 2, 3, [&](const ValueVec& a) {
        std::string s = a[0]->to_string();
        long long st = arg_int(a[1], "slice"), en = (long long)s.size();
        if (a.size() >= 3) en = arg_int(a[2], "slice");
        if (st < 0) st = 0;
        if ((long long)s.size() < en) en = (long long)s.size();
        if (st > en) return Value::make_str("");
        return Value::make_str(s.substr((size_t)st, (size_t)(en - st)));
    });
    add("char_at", 2, 2, [&](const ValueVec& a) {
        std::string s = a[0]->to_string();
        long long i = arg_int(a[1], "char_at");
        if (i < 0 || i >= (long long)s.size()) throw RuntimeError("text.char_at: index out of range");
        return Value::make_int((long long)(unsigned char)s[(size_t)i]);
    });
    add("repeat", 2, 2, [&](const ValueVec& a) {
        std::string s = a[0]->to_string();
        long long n = arg_int(a[1], "repeat");
        if (n <= 0) return Value::make_str("");
        std::string out; out.reserve(s.size() * (size_t)n);
        for (long long i = 0; i < n; ++i) out += s;
        return Value::make_str(out);
    });
    add("pad_left", 2, 3, [&](const ValueVec& a) {
        std::string s = a[0]->to_string();
        long long w = arg_int(a[1], "pad_left");
        char pad = ' ';
        if (a.size() >= 3) { std::string p = a[2]->to_string(); if (!p.empty()) pad = p[0]; }
        std::string out; out.reserve((std::max)((long long)s.size(), w));
        if (w > (long long)s.size()) out.assign((size_t)(w - s.size()), pad);
        out += s;
        return Value::make_str(out);
    });
    add("pad_right", 2, 3, [&](const ValueVec& a) {
        std::string s = a[0]->to_string();
        long long w = arg_int(a[1], "pad_right");
        char pad = ' ';
        if (a.size() >= 3) { std::string p = a[2]->to_string(); if (!p.empty()) pad = p[0]; }
        std::string out = s;
        if (w > (long long)s.size()) out.append((size_t)(w - s.size()), pad);
        return Value::make_str(out);
    });
    // Python 对齐别名
    add("ljust", 2, 3, [&](const ValueVec& a) {
        std::string s = a[0]->to_string();
        long long w = arg_int(a[1], "ljust");
        char pad = ' ';
        if (a.size() >= 3) { std::string p = a[2]->to_string(); if (!p.empty()) pad = p[0]; }
        if (w > (long long)s.size()) s.append((size_t)(w - s.size()), pad);
        return Value::make_str(s);
    });
    add("rjust", 2, 3, [&](const ValueVec& a) {
        std::string s = a[0]->to_string();
        long long w = arg_int(a[1], "rjust");
        char pad = ' ';
        if (a.size() >= 3) { std::string p = a[2]->to_string(); if (!p.empty()) pad = p[0]; }
        if (w > (long long)s.size()) s = std::string((size_t)(w - s.size()), pad) + s;
        return Value::make_str(s);
    });

    // zfill / center：零填充 / 居中对齐
    add("zfill", 2, 2, [&](const ValueVec& a) {
        std::string s = a[0]->to_string();
        long long w = arg_int(a[1], "zfill");
        if (w > (long long)s.size())
            return Value::make_str(std::string((size_t)(w - s.size()), '0') + s);
        return Value::make_str(s);
    });
    add("center", 2, 3, [&](const ValueVec& a) {
        std::string s = a[0]->to_string();
        long long w = arg_int(a[1], "center");
        char pad = ' ';
        if (a.size() >= 3) { std::string p = a[2]->to_string(); if (!p.empty()) pad = p[0]; }
        if (w <= (long long)s.size()) return Value::make_str(s);
        long long total = w - (long long)s.size();
        long long left = total / 2, right = total - left;
        return Value::make_str(std::string((size_t)left, pad) + s + std::string((size_t)right, pad));
    });

    // ord / chr：字符码 <-> 单字符字符串
    add("ord", 1, 1, [&](const ValueVec& a) {
        std::string s = a[0]->to_string();
        if (s.empty()) return Value::make_int(-1);
        return Value::make_int((long long)(unsigned char)s[0]);
    });
    add("chr", 1, 1, [&](const ValueVec& a) {
        long long c = arg_int(a[0], "chr");
        if (c < 0 || c > 255) c = 0;
        std::string s(1, (char)c);
        return Value::make_str(s);
    });

    // split(s, sep)：按分隔符拆分为字符串列表
    add("split", 2, 2, [&](const ValueVec& a) {
        const std::string s = a[0]->to_string();
        const std::string sep = a[1]->to_string();
        auto r = Value::make_list();
        size_t pos = 0, found;
        if (sep.empty()) {
            for (char c : s) r->list_rep->push_back(Value::make_str(std::string(1, c)));
        } else {
            while ((found = s.find(sep, pos)) != std::string::npos) {
                r->list_rep->push_back(Value::make_str(s.substr(pos, found - pos)));
                pos = found + sep.size();
            }
            r->list_rep->push_back(Value::make_str(s.substr(pos)));
        }
        return r;
    });

    // join(sep, parts...)：用分隔符拼接若干字符串
    add("join", 1, (size_t)-1, [&](const ValueVec& a) {
        const std::string sep = a[0]->to_string();
        std::string out;
        for (size_t i = 1; i < a.size(); ++i) {
            if (i > 1) out += sep;
            out += a[i]->to_string();
        }
        return Value::make_str(out);
    });

    // format(fmt, args...)：以顺序参数替换模板中的连续 "{}"
    add("format", 1, (size_t)-1, [&](const ValueVec& a) {
        const std::string fmt = a[0]->to_string();
        std::string out;
        out.reserve(fmt.size());
        size_t argi = 1;
        for (size_t i = 0; i < fmt.size();) {
            if (fmt[i] == '{' && i + 1 < fmt.size() && fmt[i + 1] == '}') {
                if (argi < a.size()) out += a[argi]->to_string();
                ++argi;
                i += 2;
            } else {
                out += fmt[i];
                ++i;
            }
        }
        return Value::make_str(out);
    });

    std_modules["text"] = mod;
}

} // namespace vortex
