// ============================================================
// base64_module.cpp — Base64 编解码模块
// encode(s)->str / decode(s)->str，全部返回标量。
// ============================================================
#include "base64_module.h"
#include "value.h"
#include "interpreter.h"
#include <string>

namespace vortex {

static const char B64_ENC[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string b64_encode(const std::string& in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= in.size()) {
        unsigned v = ((unsigned)(unsigned char)in[i] << 16)
                   | ((unsigned)(unsigned char)in[i+1] << 8)
                   | ((unsigned)(unsigned char)in[i+2]);
        out += B64_ENC[(v >> 18) & 63];
        out += B64_ENC[(v >> 12) & 63];
        out += B64_ENC[(v >> 6) & 63];
        out += B64_ENC[v & 63];
        i += 3;
    }
    size_t rem = in.size() - i;
    if (rem == 1) {
        unsigned v = (unsigned)(unsigned char)in[i] << 16;
        out += B64_ENC[(v >> 18) & 63];
        out += B64_ENC[(v >> 12) & 63];
        out += "==";
    } else if (rem == 2) {
        unsigned v = ((unsigned)(unsigned char)in[i] << 16)
                   | ((unsigned)(unsigned char)in[i+1] << 8);
        out += B64_ENC[(v >> 18) & 63];
        out += B64_ENC[(v >> 12) & 63];
        out += B64_ENC[(v >> 6) & 63];
        out += "=";
    }
    return out;
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static std::string b64_decode(const std::string& in) {
    std::string out;
    out.reserve((in.size() / 4) * 3);
    int buf = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=' || c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        int v = b64_val(c);
        if (v < 0) return out; // 非法字符：返回已解码部分
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((buf >> bits) & 0xFF);
        }
    }
    return out;
}

void register_base64_module(std::unordered_map<std::string, ValuePtr>& std_modules) {
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
        u[n] = mk_fn("base64", n, a0, a1, std::move(f));
    };

    add("encode", 1, 1, [&](const ValueVec& a) {
        return Value::make_str(b64_encode(a[0]->to_string()));
    });
    add("decode", 1, 1, [&](const ValueVec& a) {
        return Value::make_str(b64_decode(a[0]->to_string()));
    });

    std_modules["base64"] = mod;
}

} // namespace vortex