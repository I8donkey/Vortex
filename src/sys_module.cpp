// ============================================================
// sys_module.cpp — 系统/运行时模块（参考 Python sys）
// 全部返回标量，解释器与编译器语义一致。
// ============================================================
#include "sys_module.h"
#include "value.h"
#include "interpreter.h"
#include "os_module.h"
#include <cstdlib>
#include <chrono>
#include <thread>

namespace vortex {

static long long arg_code(const ValuePtr& v, const std::string& fn) {
    if (v->type == ValueType::Int) return v->int_val;
    if (v->type == ValueType::UInt) return (long long)v->uint_val;
    if (v->type == ValueType::Bool) return v->bool_val ? 1 : 0;
    if (v->type == ValueType::Float) return (long long)v->float_val;
    try { return std::stoll(v->to_string()); } catch (...) { throw RuntimeError("sys." + fn + ": bad argument"); }
}

void register_sys_module(std::unordered_map<std::string, ValuePtr>& std_modules) {
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
        u[n] = mk_fn("sys", n, a0, a1, std::move(f));
    };

    add("version", 0, 0, [&](const ValueVec&) { return Value::make_str("Vortex 1.0"); });
    add("platform", 0, 0, [&](const ValueVec&) {
#ifdef _WIN32
        return Value::make_str("windows");
#else
        return Value::make_str("linux");
#endif
    });
    // 单调时钟毫秒
    add("time_ms", 0, 0, [&](const ValueVec&) {
        auto n = std::chrono::steady_clock::now().time_since_epoch();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(n).count();
        return Value::make_int((long long)ms);
    });
    // 墙钟秒数（浮点）
    add("clock", 0, 0, [&](const ValueVec&) {
        double s = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return Value::make_float(s);
    });
    // 毫秒等待
    add("sleep", 1, 1, [&](const ValueVec& a) {
        long long ms = arg_code(a[0], "sleep");
        if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return Value::make_int(0);
    });
    // 退出进程
    add("exit", 0, 1, [&](const ValueVec& a) {
        long long code = a.empty() ? 0 : arg_code(a[0], "exit");
        std::exit((int)code);
        return Value::make_none();
    });

    std_modules["sys"] = mod;
}

} // namespace vortex