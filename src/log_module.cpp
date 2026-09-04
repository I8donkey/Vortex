// ============================================================
// log_module.cpp — 日志模块实现（参考 Python logging）
// ============================================================
#include "log_module.h"
#include "interpreter.h"
#include <chrono>
#include <iomanip>
#include <ctime>
#include <sstream>
#include <iostream>

namespace vortex {

static LoggerState& logger_state() {
    static LoggerState s;
    return s;
}

static const char* level_name(int lv) {
    switch (lv) {
        case 10: return "DEBUG";
        case 20: return "INFO";
        case 30: return "WARN";
        case 40: return "ERROR";
        case 50: return "FATAL";
        default: return "???";
    }
}

static int level_from_str(const std::string& s) {
    if (s == "DEBUG" || s == "debug") return 10;
    if (s == "INFO"  || s == "info")  return 20;
    if (s == "WARN"  || s == "warn")  return 30;
    if (s == "ERROR" || s == "error") return 40;
    if (s == "FATAL" || s == "fatal") return 50;
    return 20;
}

static std::string format_time() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S")
        << "." << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

static std::string format_msg(int lv, const std::string& msg) {
    auto& st = logger_state();
    std::string fmt = st.format;
    auto replace = [&](const std::string& token, const std::string& val) {
        size_t pos = 0;
        while ((pos = fmt.find(token, pos)) != std::string::npos) {
            fmt.replace(pos, token.size(), val);
            pos += val.size();
        }
    };
    replace("%(time)", format_time());
    replace("%(level)", level_name(lv));
    replace("%(name)", "vortex");
    replace("%(message)", msg);
    return fmt;
}

static void emit_log(int lv, const std::string& msg) {
    auto& st = logger_state();
    if (lv < st.min_level) return;
    std::lock_guard<std::mutex> lk(st.mtx);
    std::string line = format_msg(lv, msg) + "\n";
    if (st.console_on) {
        if (lv >= 40) std::cerr << line;
        else std::cout << line;
    }
    for (auto& fs : st.file_streams) {
        if (fs && fs->is_open()) *fs << line;
    }
}

// 拼接可变参数为单个消息字符串（namespace 级，避免功能 lambda 捕获局部导致悬空引用）
static std::string join_args(const ValueVec& a) {
    std::ostringstream os;
    for (size_t i = 0; i < a.size(); ++i) {
        if (i) os << " ";
        os << a[i]->to_string();
    }
    return os.str();
}

void register_log_module(std::unordered_map<std::string, ValuePtr>& std_modules) {
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
        u[n] = mk_fn("log", n, a0, a1, std::move(f));
    };

    // 级别常量
    u["DEBUG"] = Value::make_int(10);
    u["INFO"]  = Value::make_int(20);
    u["WARN"]  = Value::make_int(30);
    u["ERROR"] = Value::make_int(40);
    u["FATAL"] = Value::make_int(50);

    // 日志输出函数（可变参数，空格拼接后记录）
    add("debug", 1, (size_t)-1, [&](const ValueVec& a) { emit_log(10, join_args(a)); return Value::make_none(); });
    add("info",  1, (size_t)-1, [&](const ValueVec& a) { emit_log(20, join_args(a)); return Value::make_none(); });
    add("warn",  1, (size_t)-1, [&](const ValueVec& a) { emit_log(30, join_args(a)); return Value::make_none(); });
    add("error", 1, (size_t)-1, [&](const ValueVec& a) { emit_log(40, join_args(a)); return Value::make_none(); });
    add("fatal", 1, (size_t)-1, [&](const ValueVec& a) { emit_log(50, join_args(a)); return Value::make_none(); });

    // 配置函数
    add("level", 1, 1, [&](const ValueVec& a) {
        std::string lv = a[0]->to_string();
        logger_state().min_level = level_from_str(lv);
        return Value::make_none();
    });
    add("get_level", 0, 0, [&](const ValueVec&) {
        return Value::make_str(level_name(logger_state().min_level));
    });
    add("format", 1, 1, [&](const ValueVec& a) {
        logger_state().format = a[0]->to_string();
        return Value::make_none();
    });
    add("file", 1, 1, [&](const ValueVec& a) {
        auto& st = logger_state();
        std::string path = a[0]->to_string();
        auto fs = std::make_shared<std::ofstream>(path, std::ios::app);
        st.file_streams.push_back(fs);
        st.file_paths.push_back(path);
        return Value::make_none();
    });
    add("console", 1, 1, [&](const ValueVec& a) {
        logger_state().console_on = a[0]->truthy();
        return Value::make_none();
    });

    std_modules["log"] = mod;
}

} // namespace vortex
