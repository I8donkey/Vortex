// ============================================================
// os_module.cpp — 操作系统/环境模块
// 全部返回标量（str/int/bool），解释器与编译器语义一致。
// ============================================================
#include "os_module.h"
#include "value.h"
#include "interpreter.h"
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#endif

namespace vortex {

static std::vector<std::string>& global_args() {
    static std::vector<std::string> a;
    return a;
}
void os_set_global_args(const std::vector<std::string>& args) { global_args() = args; }
static bool os_setenv(const std::string& name, const std::string& val) {
#ifdef _WIN32
    std::string kv = name + "=" + val;
    return _putenv(kv.c_str()) == 0;
#else
    return setenv(name.c_str(), val.c_str(), 1) == 0;
#endif
}

static bool os_unsetenv(const std::string& name) {
#ifdef _WIN32
    std::string kv = name + "=";
    return _putenv(kv.c_str()) == 0;
#else
    return unsetenv(name.c_str()) == 0;
#endif
}

void register_os_module(std::unordered_map<std::string, ValuePtr>& std_modules) {
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
        u[n] = mk_fn("os", n, a0, a1, std::move(f));
    };

    add("getenv", 1, 1, [&](const ValueVec& a) {
        const char* v = std::getenv(a[0]->to_string().c_str());
        return Value::make_str(v ? v : "");
    });
    add("hasenv", 1, 1, [&](const ValueVec& a) {
        return Value::make_bool(std::getenv(a[0]->to_string().c_str()) != nullptr);
    });
    add("setenv", 2, 2, [&](const ValueVec& a) {
        return Value::make_bool(os_setenv(a[0]->to_string(), a[1]->to_string()));
    });
    add("unsetenv", 1, 1, [&](const ValueVec& a) {
        return Value::make_bool(os_unsetenv(a[0]->to_string()));
    });
    add("cwd", 0, 0, [&](const ValueVec&) {
        std::error_code ec;
        return Value::make_str(std::filesystem::current_path(ec).string());
    });
    add("chdir", 1, 1, [&](const ValueVec& a) {
        std::error_code ec;
        std::filesystem::current_path(a[0]->to_string(), ec);
        return Value::make_bool(!ec);
    });
    add("pid", 0, 0, [&](const ValueVec&) {
#ifdef _WIN32
        return Value::make_int((long long)GetCurrentProcessId());
#else
        return Value::make_int((long long)getpid());
#endif
    });
    add("platform", 0, 0, [&](const ValueVec&) {
#ifdef _WIN32
        return Value::make_str("windows");
#else
        return Value::make_str("linux");
#endif
    });
    add("home", 0, 0, [&](const ValueVec&) {
#ifdef _WIN32
        const char* h = std::getenv("USERPROFILE");
#else
        const char* h = std::getenv("HOME");
#endif
        return Value::make_str(h ? h : "");
    });
    add("tempdir", 0, 0, [&](const ValueVec&) {
#ifdef _WIN32
        const char* t = std::getenv("TEMP");
        if (!t) t = std::getenv("TMP");
#else
        const char* t = std::getenv("TMPDIR");
        if (!t) t = "/tmp";
#endif
        return Value::make_str(t ? t : "");
    });
    add("path_join", 2, 10, [&](const ValueVec& a) {
        std::filesystem::path p(a[0]->to_string());
        for (size_t i = 1; i < a.size(); ++i) p /= a[i]->to_string();
        return Value::make_str(p.generic_string());
    });

    // 路径拆分：兼容 '/' 与 '\'
    add("basename", 1, 1, [&](const ValueVec& a) {
        const std::string p = a[0]->to_string();
        size_t s = p.find_last_of("/\\");
        return Value::make_str(s == std::string::npos ? p : p.substr(s + 1));
    });
    add("dirname", 1, 1, [&](const ValueVec& a) {
        const std::string p = a[0]->to_string();
        size_t s = p.find_last_of("/\\");
        if (s == std::string::npos) return Value::make_str("");
        if (s == 0) return Value::make_str("/");
        return Value::make_str(p.substr(0, s));
    });
    add("extname", 1, 1, [&](const ValueVec& a) {
        const std::string p = a[0]->to_string();
        size_t s = p.find_last_of("/\\");
        size_t d = p.find_last_of('.');
        if (d == std::string::npos || d == 0 || (s != std::string::npos && d < s))
            return Value::make_str("");
        return Value::make_str(p.substr(d));
    });

    // 命令行参数（运行时由宿主注入）
    add("listdir", 1, 1, [&](const ValueVec& a) {
        auto r = Value::make_list();
        std::error_code ec;
        std::vector<std::string> names;
        for (std::filesystem::directory_iterator it(a[0]->to_string(), ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            names.push_back(it->path().filename().string());
        }
        std::sort(names.begin(), names.end());  // 排序保证确定性输出
        for (auto& n : names) r->list_rep->push_back(Value::make_str(n));
        return r;
    });
    add("argc", 0, 0, [&](const ValueVec&) {
        return Value::make_int((long long)global_args().size());
    });
    add("arg", 1, 1, [&](const ValueVec& a) {
        long long i = 0;
        try { i = std::stoll(a[0]->to_string()); } catch (...) { throw RuntimeError("os.arg: bad index"); }
        auto& g = global_args();
        if (i < 0 || i >= (long long)g.size()) throw RuntimeError("os.arg: index out of range");
        return Value::make_str(g[(size_t)i]);
    });

    // 常量
    u["SEP"] = Value::make_str("/");
    u["LINE_END"] = Value::make_str("\n");

    std_modules["os"] = mod;
}

} // namespace vortex