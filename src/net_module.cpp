// ============================================================
// net_module.cpp — 网络模块（url_encode/decode + http_get/post）
// 核心逻辑共享自 net_core.h，解释器与编译端一致。
// ============================================================
#include "net_module.h"
#include "net_core.h"
#include "value.h"
#include "interpreter.h"
#include <string>

namespace vortex {

void register_net_module(std::unordered_map<std::string, ValuePtr>& std_modules) {
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
        u[n] = mk_fn("net", n, a0, a1, std::move(f));
    };

    add("url_encode", 1, 1, [&](const ValueVec& a) {
        return Value::make_str(net_url_encode(a[0]->to_string()));
    });
    add("url_decode", 1, 1, [&](const ValueVec& a) {
        return Value::make_str(net_url_decode(a[0]->to_string()));
    });
    add("http_get", 1, 1, [&](const ValueVec& a) {
        try {
            return Value::make_str(net_http(a[0]->to_string(), "GET", ""));
        } catch (const std::exception& e) {
            throw RuntimeError(std::string("net.http_get: ") + e.what());
        }
    });
    add("http_post", 2, 3, [&](const ValueVec& a) {
        std::string body = a[1]->to_string();
        try {
            return Value::make_str(net_http(a[0]->to_string(), "POST", body));
        } catch (const std::exception& e) {
            throw RuntimeError(std::string("net.http_post: ") + e.what());
        }
    });

    std_modules["net"] = mod;
}

} // namespace vortex