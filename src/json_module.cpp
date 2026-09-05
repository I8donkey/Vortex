// ============================================================
// json_module.cpp — JSON 模块（标量导向子集）
// 解析标量字段与字符串化，全部返回标量。
// ============================================================
#include "json_module.h"
#include "value.h"
#include "interpreter.h"
#include <cstdlib>
#include <string>
#include <vector>

namespace vortex {

static void skip_ws(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i]==' ' || s[i]=='\t' || s[i]=='\n' || s[i]=='\r')) ++i;
}

// 读取 JSON 字符串字面量（含转义展开），返回文本；找不到返回 false in ok
static bool parse_str_at(const std::string& s, size_t& i, std::string& out) {
    skip_ws(s, i);
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    out.clear();
    while (i < s.size()) {
        char c = s[i];
        if (c == '"') { ++i; return true; }
        if (c == '\\') {
            if (i + 1 >= s.size()) return false;
            char e = s[i+1];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'u': {
                    if (i + 6 > s.size()) return false;
                    unsigned code = 0;
                    for (int k = 1; k <= 4; ++k) {
                        char h = s[i+1+k];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= (unsigned)(h-'0');
                        else if (h >= 'a' && h <= 'f') code |= (unsigned)(h-'a'+10);
                        else if (h >= 'A' && h <= 'F') code |= (unsigned)(h-'A'+10);
                        else return false;
                    }
                    out += (char)code;
                    i += 4;
                    break;
                }
                default: return false;
            }
            i += 2;
        } else {
            out += c;
            ++i;
        }
    }
    return false;
}

// 查找根级 JSON string 值；否则失败
static bool json_get_str(const std::string& s, std::string& out) {
    size_t i = 0;
    return parse_str_at(s, i, out);
}
static bool json_get_bool(const std::string& s, bool& out) {
    size_t i = 0; skip_ws(s, i);
    if (s.compare(i, 4, "true") == 0) { out = true; return true; }
    if (s.compare(i, 5, "false") == 0) { out = false; return true; }
    return false;
}
static bool json_get_number(const std::string& s, double& out) {
    size_t i = 0; skip_ws(s, i);
    char* end = nullptr;
    double v = std::strtod(s.c_str() + i, &end);
    if (end == s.c_str() + i) return false;
    out = v;
    return true;
}
static bool json_valid(const std::string& s) {
    size_t i = 0;
    skip_ws(s, i);
    if (i >= s.size()) return false;
    char c = s[i];
    if (c == '"') { std::string tmp; return parse_str_at(s, i, tmp); }
    if (c == '{' || c == '[') {
        // 浅校验：仅要求存在闭合括号
        int depth = 0; bool in_str = false; char prev = 0;
        for (size_t j = i; j < s.size(); ++j) {
            char ch = s[j];
            if (in_str) {
                if (ch == '"' && prev != '\\') in_str = false;
            } else {
                if (ch == '"') in_str = true;
                else if (ch == '{' || ch == '[') ++depth;
                else if (ch == '}' || ch == ']') {
                    --depth;
                    if (depth < 0) return false;
                }
            }
            prev = ch;
        }
        return depth == 0;
    }
    double v; return json_get_number(s, v);
}

static std::string json_escape(const std::string& s) {
    std::string r = "\"";
    for (char ch : s) {
        switch (ch) {
            case '"': r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\t': r += "\\t"; break;
            case '\r': r += "\\r"; break;
            default: r += ch;
        }
    }
    r += "\"";
    return r;
}
static std::string json_stringify_str(const std::string& v) { return json_escape(v); }
static std::string json_stringify_bool(bool b) { return b ? "true" : "false"; }
static std::string json_stringify_int(long long v) { return std::to_string(v); }
static std::string json_stringify_double(double v) {
    char buf[40]; snprintf(buf, sizeof(buf), "%.15g", v); return buf;
}
static std::string json_stringify_none() { return "null"; }

// 跳过任意 JSON 值（含嵌套）
static void json_skip_value(std::string& s, size_t& i) {
    skip_ws(s, i);
    if (i >= s.size()) return;
    if (s[i] == '"') { std::string t; parse_str_at(s, i, t); return; }
    if (s[i] == '{') {
        ++i; int d = 0;
        while (i < s.size()) {
            if (s[i]=='{') ++d; else if (s[i]=='}'){ --d; if(d<=0){ ++i; break; } }
            ++i;
        }
        return;
    }
    if (s[i] == '[') {
        ++i; int d = 0;
        while (i < s.size()) {
            if (s[i]=='[') ++d; else if (s[i]==']'){ --d; if(d<=0){ ++i; break; } }
            ++i;
        }
        return;
    }
    while (i < s.size() && s[i]!=',' && s[i]!='}' && s[i]!=']'
           && s[i]!=' ' && s[i]!='\t' && s[i]!='\n' && s[i]!='\r') ++i;
}

// 解析 JSON 数组，元素转为字符串（字符串解码、其余取字面文本）
static bool json_parse_array_impl(const std::string& src, std::vector<std::string>& out) {
    std::string s = src;
    size_t i = 0; skip_ws(s, i);
    if (i >= s.size() || s[i] != '[') return false;
    ++i;
    skip_ws(s, i);
    if (i < s.size() && s[i] == ']') return true;  // 空数组
    while (i < s.size()) {
        skip_ws(s, i);
        if (src[i] == '"') {
            std::string str;
            if (!parse_str_at(s, i, str)) return false;
            out.push_back(str);
        } else if (src[i] == '[' || src[i] == '{') {
            json_skip_value(s, i);
        } else {
            size_t st = i;
            while (i < s.size() && s[i]!=',' && s[i]!=']'
                   && s[i]!=' ' && s[i]!='\t' && s[i]!='\n' && s[i]!='\r') ++i;
            out.push_back(s.substr(st, i-st));
        }
        skip_ws(s, i);
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == ']') return true;
        return false;
    }
    return false;
}

// 读取 JSON 对象顶层指定键的值；字符串解码，其余返回字面文本
static bool json_get_member(const std::string& src, const std::string& key, std::string& out) {
    std::string s = src;
    size_t i = 0; skip_ws(s, i);
    if (i >= s.size() || s[i] != '{') return false;
    ++i;
    while (i < s.size()) {
        skip_ws(s, i);
        std::string member;
        if (!parse_str_at(s, i, member)) return false;
        skip_ws(s, i);
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        skip_ws(s, i);
        if (i >= s.size()) return false;
        if (member == key) {
            if (s[i] == '"') { std::string v; if (!parse_str_at(s, i, v)) return false; out = v; return true; }
            if (s[i] == '{' || s[i] == '[') { json_skip_value(s, i); return false; }
            size_t st = i;
            while (i < s.size() && s[i]!=',' && s[i]!='}'
                   && s[i]!=' ' && s[i]!='\t' && s[i]!='\n' && s[i]!='\r') ++i;
            out = s.substr(st, i-st);
            return true;
        }
        json_skip_value(s, i);
        skip_ws(s, i);
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        return false;
    }
    return false;
}

static long long val_as_int(const ValuePtr& v) {
    try { return std::stoll(v->to_string()); } catch (...) { return 0; }
}
static double val_as_float(const ValuePtr& v) {
    try { return std::stod(v->to_string()); } catch (...) { return 0.0; }
}

void register_json_module(std::unordered_map<std::string, ValuePtr>& std_modules) {
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
        u[n] = mk_fn("json", n, a0, a1, std::move(f));
    };

    add("valid", 1, 1, [&](const ValueVec& a) {
        return Value::make_bool(json_valid(a[0]->to_string()));
    });
    add("parse_str", 1, 1, [&](const ValueVec& a) {
        std::string o;
        if (!json_get_str(a[0]->to_string(), o))
            throw RuntimeError("json.parse_str: not a JSON string");
        return Value::make_str(o);
    });
    add("parse_int", 1, 1, [&](const ValueVec& a) {
        double d; if (!json_get_number(a[0]->to_string(), d))
            throw RuntimeError("json.parse_int: not a number");
        return Value::make_int((long long)d);
    });
    add("parse_float", 1, 1, [&](const ValueVec& a) {
        double d; if (!json_get_number(a[0]->to_string(), d))
            throw RuntimeError("json.parse_float: not a number");
        return Value::make_float(d);
    });
    add("parse_bool", 1, 1, [&](const ValueVec& a) {
        bool b; if (!json_get_bool(a[0]->to_string(), b))
            throw RuntimeError("json.parse_bool: not a bool");
        return Value::make_bool(b);
    });
    add("parse_array", 1, 1, [&](const ValueVec& a) {
        std::vector<std::string> items;
        if (!json_parse_array_impl(a[0]->to_string(), items))
            throw RuntimeError("json.parse_array: not a JSON array");
        auto r = Value::make_list();
        for (auto& it : items) r->list_rep->push_back(Value::make_str(it));
        return r;
    });
    add("get", 2, 2, [&](const ValueVec& a) {
        std::string v;
        if (!json_get_member(a[0]->to_string(), a[1]->to_string(), v))
            return Value::make_str("");
        return Value::make_str(v);
    });
    add("stringify_str", 1, 1, [&](const ValueVec& a) {
        return Value::make_str(json_stringify_str(a[0]->to_string()));
    });
    add("stringify_int", 1, 1, [&](const ValueVec& a) {
        return Value::make_str(json_stringify_int(val_as_int(a[0])));
    });
    add("stringify_float", 1, 1, [&](const ValueVec& a) {
        return Value::make_str(json_stringify_double(val_as_float(a[0])));
    });
    add("stringify_bool", 1, 1, [&](const ValueVec& a) {
        return Value::make_str(json_stringify_bool(a[0]->truthy()));
    });
    add("stringify", 1, 1, [&](const ValueVec& a) {
        const std::string tn = a[0]->type_name();
        // 依据解释器类型名区分：int/float/bool 输出裸标量，其余按字符串转义
        if (tn == "int" || tn == "long" || tn == "short" || tn == "uint" ||
            tn == "ulong" || tn == "ushort") {
            long long iv = val_as_int(a[0]);
            if (std::to_string(iv) == a[0]->to_string() || iv != 0)
                return Value::make_str(json_stringify_int(iv));
        }
        if (tn == "float" || tn == "double") {
            return Value::make_str(json_stringify_double(val_as_float(a[0])));
        }
        if (tn == "bool" || tn == "boolean") {
            return Value::make_str(json_stringify_bool(a[0]->truthy()));
        }
        if (tn == "none" || tn == "None" || tn == "null") {
            return Value::make_str("null");
        }
        return Value::make_str(json_stringify_str(a[0]->to_string()));
    });

    std_modules["json"] = mod;
}

} // namespace vortex