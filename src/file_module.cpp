// ============================================================
// file_module.cpp — 文件系统模块（参考 Python os.path / open）
//
// 提供统一的文件读 / 写 / 追加、存在性、删除、改名、大小、
// 目录判断与目录列举。解释器端直接返回原生 Value。
// ============================================================
#include "file_module.h"
#include "value.h"
#include "interpreter.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace vortex {
namespace fs = std::filesystem;

void register_file_module(std::unordered_map<std::string, ValuePtr>& std_modules) {
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
        u[n] = mk_fn("file", n, a0, a1, std::move(f));
    };

    add("read", 1, 1, [&](const ValueVec& a) {
        std::string p = a[0]->to_string();
        std::ifstream f(p, std::ios::binary);
        if (!f) throw RuntimeError("file.read: cannot open file");
        std::ostringstream oss; oss << f.rdbuf();
        return Value::make_str(oss.str());
    });

    // readlines(path)：把文件按换行拆成字符串列表
    add("readlines", 1, 1, [&](const ValueVec& a) {
        std::string p = a[0]->to_string();
        std::ifstream f(p, std::ios::binary);
        if (!f) throw RuntimeError("file.readlines: cannot open file");
        std::ostringstream oss; oss << f.rdbuf();
        std::string content = oss.str();
        auto r = Value::make_list();
        size_t pos = 0;
        while (true) {
            size_t nl = content.find('\n', pos);
            if (nl == std::string::npos) {
                r->list_rep->push_back(Value::make_str(content.substr(pos)));
                break;
            }
            std::string line = content.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();  // 兼容 CRLF
            r->list_rep->push_back(Value::make_str(line));
            pos = nl + 1;
        }
        return r;
    });

    add("write", 2, 2, [&](const ValueVec& a) {
        std::ofstream f(a[0]->to_string(), std::ios::binary | std::ios::trunc);
        if (!f) throw RuntimeError("file.write: cannot open file");
        f << a[1]->to_string();
        return Value::make_none();
    });

    add("append", 2, 2, [&](const ValueVec& a) {
        std::ofstream f(a[0]->to_string(), std::ios::binary | std::ios::app);
        if (!f) throw RuntimeError("file.append: cannot open file");
        f << a[1]->to_string();
        return Value::make_none();
    });

    add("exists", 1, 1, [&](const ValueVec& a) {
        std::error_code ec;
        return Value::make_bool(fs::exists(a[0]->to_string(), ec));
    });

    add("remove", 1, 1, [&](const ValueVec& a) {
        std::error_code ec;
        return Value::make_bool(fs::remove(a[0]->to_string(), ec));
    });

    add("rename", 2, 2, [&](const ValueVec& a) {
        std::error_code ec;
        fs::rename(a[0]->to_string(), a[1]->to_string(), ec);
        return Value::make_bool(!ec);
    });

    add("size", 1, 1, [&](const ValueVec& a) {
        std::error_code ec;
        auto sz = fs::file_size(a[0]->to_string(), ec);
        return Value::make_int(ec ? -1 : (long long)sz);
    });

    add("isdir", 1, 1, [&](const ValueVec& a) {
        std::error_code ec;
        return Value::make_bool(fs::is_directory(a[0]->to_string(), ec));
    });

    add("isfile", 1, 1, [&](const ValueVec& a) {
        std::error_code ec;
        return Value::make_bool(fs::is_regular_file(a[0]->to_string(), ec));
    });

    add("mkdir", 1, 1, [&](const ValueVec& a) {
        std::error_code ec;
        fs::create_directories(a[0]->to_string(), ec);
        return Value::make_bool(!ec);
    });

    add("rmdir", 1, 1, [&](const ValueVec& a) {
        std::error_code ec;
        return Value::make_bool(fs::remove_all(a[0]->to_string(), ec) > 0);
    });

    add("listdir", 1, 1, [&](const ValueVec& a) {
        auto r = Value::make_list();
        std::error_code ec;
        std::vector<std::string> names;
        for (fs::directory_iterator it(a[0]->to_string(), ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            names.push_back(it->path().filename().string());
        }
        std::sort(names.begin(), names.end());  // 排序保证确定性输出
        for (auto& n : names) r->list_rep->push_back(Value::make_str(n));
        return r;
    });

    std_modules["file"] = mod;
}

} // namespace vortex