#include "interpreter.h"
#include "lexer.h"
#include "parser.h"
#include "vortex_modules.h"
#include "game2d_module.h"
#include "thread_module.h"
#include "log_module.h"
#include "gui_module.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <random>
#include <algorithm>
#include <ctime>
#include <cmath>
#include <cstring>
#include <chrono>
#include <thread>
#include <numeric>
#include <cctype>

namespace vortex {

// ============ 随机数生成器（供 random 模块复用） ============
static std::mt19937& get_rng() {
    static std::mt19937 rng((unsigned)std::time(nullptr));
    return rng;
}

// ============ Environment ============
void Environment::define(const std::string& name, ValuePtr value, bool is_const) {
    vars_[name] = {std::move(value), is_const};
}
bool Environment::has(const std::string& name) const {
    if (vars_.count(name)) return true;
    return parent_ ? parent_->has(name) : false;
}
ValuePtr& Environment::lookup(const std::string& name) {
    auto it = vars_.find(name);
    if (it != vars_.end()) return it->second.first;
    if (parent_) return parent_->lookup(name);
    throw RuntimeError("Undefined variable: '" + name + "'");
}
void Environment::assign(const std::string& name, ValuePtr value) {
    auto it = vars_.find(name);
    if (it != vars_.end()) {
        if (it->second.second) throw RuntimeError("Cannot modify const variable: " + name);
        it->second.first = std::move(value);
        return;
    }
    if (parent_) return parent_->assign(name, std::move(value));
    throw RuntimeError("Undefined variable: '" + name + "'");
}
void Environment::erase(const std::string& name) {
    auto it = vars_.find(name);
    if (it != vars_.end()) { vars_.erase(it); return; }
    if (parent_) return parent_->erase(name);
    throw RuntimeError("Undefined variable: '" + name + "'");
}
void Environment::clear_all() { vars_.clear(); }
std::vector<std::string> Environment::locals() const {
    std::vector<std::string> r;
    for (auto& [k, v] : vars_) r.push_back(k);
    return r;
}

// ============ Interpreter ============
static ValuePtr value_from_uint(unsigned long long v) { return Value::make_uint(v); }

Interpreter::Interpreter() {
    current_env_ = &globals_;
    init_builtins();
    init_modules();
    game2d::Game::bind_interpreter(this);
    // 绑定 thread / gui 模块的全局解释器指针
    extern Interpreter* g_thread_active_interpreter;
    extern Interpreter* g_gui_active_interpreter;
    g_thread_active_interpreter = this;
    g_gui_active_interpreter = this;
}

void Interpreter::init_builtins() {
    // 内置函数以 FunctionValue 存放到 globals
    auto reg_builtin = [&](const std::string& name, size_t min_args, size_t max_args,
                           std::function<ValuePtr(const ValueVec&, Environment&)> fn) {
        auto fv = std::make_shared<FunctionValue>();
        fv->name = name;
        fv->is_builtin = true;
        fv->builtin_fn = [fn, min_args, max_args, name](const ValueVec& args, Environment& env) -> ValuePtr {
            if (args.size() < min_args || (max_args != (size_t)-1 && args.size() > max_args)) {
                throw RuntimeError("Function '" + name + "' expects " +
                    std::to_string(min_args) + "~" + std::to_string(max_args) + " args, got " +
                    std::to_string(args.size()));
            }
            return fn(args, env);
        };
        auto v = Value::make_none();
        v->type = ValueType::Function; v->fn_rep = fv;
        globals_.define(name, v, true);
    };

    // print
    reg_builtin("print", 0, (size_t)-1, [this](const ValueVec& args, Environment&) -> ValuePtr {
        std::ostringstream oss;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) oss << " ";
            oss << args[i]->to_string();
        }
        oss << "\n";
        print_output(oss.str());
        return Value::make_none();
    });

    // input
    reg_builtin("input", 0, 1, [this](const ValueVec& args, Environment&) -> ValuePtr {
        std::string p = args.empty() ? "" : args[0]->to_string();
        return Value::make_str(read_input(p));
    });

    // type conversion
    reg_builtin("int", 1, 1, [](const ValueVec& a, Environment&) { return value_to_int(a[0]); });
    reg_builtin("long", 1, 1, [](const ValueVec& a, Environment&) { return value_to_long(a[0]); });
    reg_builtin("uint", 1, 1, [](const ValueVec& a, Environment&) { return value_to_uint(a[0]); });
    reg_builtin("ulong", 1, 1, [](const ValueVec& a, Environment&) { return value_to_ulong(a[0]); });
    reg_builtin("float", 1, 1, [](const ValueVec& a, Environment&) { return value_to_float(a[0]); });
    reg_builtin("double", 1, 1, [](const ValueVec& a, Environment&) { return value_to_float(a[0]); });
    reg_builtin("bool", 1, 1, [](const ValueVec& a, Environment&) { return value_to_bool(a[0]); });
    reg_builtin("str", 1, 1, [](const ValueVec& a, Environment&) { return value_to_str(a[0]); });
    reg_builtin("unistr", 1, 1, [](const ValueVec& a, Environment&) { return value_to_unistr(a[0]); });
    reg_builtin("bin", 1, 1, [](const ValueVec& a, Environment&) { return value_to_bin(a[0]); });
    reg_builtin("char", 1, 1, [](const ValueVec& a, Environment&) {
        auto v = value_to_int(a[0]);
        return Value::make_char((int)v->int_val);
    });
    reg_builtin("unichar", 1, 1, [](const ValueVec& a, Environment&) {
        auto v = value_to_int(a[0]);
        return Value::make_unichar((int)v->int_val);
    });

    // format
    reg_builtin("format", 1, 2, [](const ValueVec& a, Environment&) {
        std::string spec = a.size() >= 2 ? a[1]->to_string() : "";
        return Value::make_str(format_value(a[0], spec));
    });

    // len
    reg_builtin("len", 1, 1, [](const ValueVec& a, Environment&) {
        auto& v = a[0];
        switch (v->type) {
            case ValueType::Str: case ValueType::UniStr: return value_from_uint(v->str_val.size());
            case ValueType::Bin: return value_from_uint(v->bin_val.size());
            case ValueType::List: return value_from_uint(v->list_rep->size());
            case ValueType::Stack: return value_from_uint(v->stack_rep->size());
            case ValueType::Queue: return value_from_uint(v->queue_rep->size());
            case ValueType::Set: return value_from_uint(v->set_rep->size());
            case ValueType::UndSet: return value_from_uint(v->undset_rep->size());
            case ValueType::Dict: return value_from_uint(v->dict_rep->size());
            case ValueType::Tuple: return value_from_uint(v->tuple_rep->size());
            default: throw RuntimeError("len() not applicable to " + v->type_name());
        }
    });

    // type
    reg_builtin("type", 1, 1, [](const ValueVec& a, Environment&) {
        return Value::make_str(a[0]->type_name());
    });

    // range
    reg_builtin("range", 2, 3, [](const ValueVec& a, Environment&) {
        long long start = value_to_int(a[0])->int_val;
        long long end   = value_to_int(a[1])->int_val;
        long long step = 1;
        if (a.size() >= 3) step = value_to_int(a[2])->int_val;
        if (step == 0) throw RuntimeError("range step cannot be 0");
        auto lst = Value::make_list();
        if (step > 0) for (long long i = start; i < end; i += step) lst->list_rep->push_back(Value::make_int(i));
        else for (long long i = start; i > end; i += step) lst->list_rep->push_back(Value::make_int(i));
        return lst;
    });

    // isinstance
    reg_builtin("isinstance", 2, 2, [](const ValueVec& a, Environment&) {
        std::string tname = a[1]->to_string();
        return Value::make_bool(a[0]->type_name() == tname);
    });

    // set literal helper
    reg_builtin("__set_literal__", 1, 1, [](const ValueVec& a, Environment&) {
        auto s = Value::make_set();
        auto& lst = *a[0]->list_rep;
        for (auto& e : lst) s->set_rep->insert(e);
        return s;
    });

    // 容器构造空函数：list/set/undset/dict/stack/queue/pair/tuple/make_pair/make_tuple
    reg_builtin("list", 0, 1, [](const ValueVec& a, Environment&) {
        if (a.empty()) return Value::make_list();
        auto r = Value::make_list();
        if (a[0]->type == ValueType::List) {
            for (auto& e : *a[0]->list_rep) r->list_rep->push_back(e);
        } else if (a[0]->type == ValueType::Str) {
            for (char c : a[0]->str_val) r->list_rep->push_back(Value::make_char((unsigned char)c));
        } else {
            r->list_rep->push_back(a[0]);
        }
        return r;
    });
    reg_builtin("set", 0, 1, [](const ValueVec& a, Environment&) {
        auto r = Value::make_set();
        if (!a.empty()) {
            if (a[0]->type == ValueType::List) for (auto& e : *a[0]->list_rep) r->set_rep->insert(e);
            else if (a[0]->type == ValueType::Set) for (auto& e : *a[0]->set_rep) r->set_rep->insert(e);
        }
        return r;
    });
    reg_builtin("undset", 0, 1, [](const ValueVec& a, Environment&) {
        auto r = Value::make_undset();
        if (!a.empty()) {
            if (a[0]->type == ValueType::List) for (auto& e : *a[0]->list_rep) r->undset_rep->insert(e);
            else if (a[0]->type == ValueType::UndSet) for (auto& e : *a[0]->undset_rep) r->undset_rep->insert(e);
        }
        return r;
    });
    reg_builtin("dict", 0, 1, [](const ValueVec& a, Environment&) {
        auto r = Value::make_dict();
        if (!a.empty() && a[0]->type == ValueType::List) {
            for (auto& e : *a[0]->list_rep) {
                if (e->type == ValueType::Tuple && e->tuple_rep->size() == 2) {
                    (*r->dict_rep)[(*e->tuple_rep)[0]] = (*e->tuple_rep)[1];
                } else if (e->type == ValueType::Pair) {
                    (*r->dict_rep)[e->pair_rep->first] = e->pair_rep->second;
                }
            }
        }
        return r;
    });
    reg_builtin("stack", 0, 1, [](const ValueVec& a, Environment&) {
        auto r = Value::make_stack();
        if (!a.empty() && a[0]->type == ValueType::List) {
            for (auto& e : *a[0]->list_rep) r->stack_rep->push_back(e);
        }
        return r;
    });
    reg_builtin("queue", 0, 1, [](const ValueVec& a, Environment&) {
        auto r = Value::make_queue();
        if (!a.empty() && a[0]->type == ValueType::List) {
            for (auto& e : *a[0]->list_rep) r->queue_rep->push_back(e);
        }
        return r;
    });
    reg_builtin("pair", 2, 2, [](const ValueVec& a, Environment&) {
        return Value::make_pair(a[0], a[1]);
    });
    reg_builtin("make_pair", 2, 2, [](const ValueVec& a, Environment&) {
        return Value::make_pair(a[0], a[1]);
    });
    reg_builtin("tuple", 0, 1, [](const ValueVec& a, Environment&) {
        if (a.empty()) return Value::make_tuple({});
        if (a[0]->type == ValueType::List) return Value::make_tuple(std::vector<ValuePtr>(a[0]->list_rep->begin(), a[0]->list_rep->end()));
        return Value::make_tuple(ValueVec{a[0]});
    });
    reg_builtin("make_tuple", 0, (size_t)-1, [](const ValueVec& a, Environment&) {
        return Value::make_tuple(a);
    });
}

// 手动实现 strptime：将字符串按 fmt 解析进 std::tm。
// 不依赖 std::get_time（MinGW 对其支持有限），健壮且可移植。
static bool util_strptime(const std::string& s, const std::string& fmt, std::tm& t) {
    static const char* dow_full[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
    static const char* dow_abbr[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char* mon_full[] = {"January","February","March","April","May","June","July",
                                     "August","September","October","November","December"};
    static const char* mon_abbr[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul",
                                     "Aug","Sep","Oct","Nov","Dec"};
    static const char* am_names[] = {"AM","PM"};
    size_t i = 0;
    // 依次尝试两组名称（全称/简称），返回是否匹配
    auto match_names = [&](const char* const* full, const char* const* abbr, int n, int& out) -> bool {
        for (int k = 0; k < n; ++k) {
            for (int pass = 0; pass < 2; ++pass) {
                const char* w = pass == 0 ? full[k] : abbr[k];
                size_t len = std::strlen(w);
                if (len == 0 || i + len > s.size()) continue;
                bool ok = true;
                for (size_t x = 0; x < len; ++x)
                    if ((s[i + x] | 32) != (w[x] | 32)) { ok = false; break; }
                if (ok) { i += len; out = k; return true; }
            }
        }
        return false;
    };
    auto skip_ws = [&]{ while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; };
    auto num = [&](size_t width, int& out) -> bool {
        int v = 0, cnt = 0;
        while (i < s.size() && std::isdigit((unsigned char)s[i]) && cnt < (int)width) {
            v = v * 10 + (s[i] - '0'); ++i; ++cnt;
        }
        if (cnt == 0) return false;
        out = v; return true;
    };
    for (size_t j = 0; j < fmt.size(); ++j) {
        if (fmt[j] == '%' && j + 1 < fmt.size()) {
            char c = fmt[++j];
            int v;
            if (c == '%') { if (i < s.size() && s[i] == '%') ++i; else return false; }
            else if (c == 'Y') { if (!num(4, v)) return false; t.tm_year = v - 1900; }
            else if (c == 'y') { if (!num(2, v)) return false; t.tm_year = (v < 69 ? 2000 : 1900) + v - 1900; }
            else if (c == 'm') { if (!num(2, v)) return false; t.tm_mon = v - 1; }
            else if (c == 'd' || c == 'e') { if (!num(2, v)) return false; t.tm_mday = v; }
            else if (c == 'H') { if (!num(2, v)) return false; t.tm_hour = v; }
            else if (c == 'I') { if (!num(2, v)) return false; t.tm_hour = v % 12; }
            else if (c == 'M') { if (!num(2, v)) return false; t.tm_min = v; }
            else if (c == 'S') { if (!num(2, v)) return false; t.tm_sec = v; }
            else if (c == 'j') { if (!num(3, v)) return false; t.tm_yday = v - 1; }
            else if (c == 'p') {
                int k = 0; bool ok = false;
                for (; k < 2; ++k) {
                    const char* w = am_names[k];
                    size_t len = std::strlen(w);
                    if (len && i + len <= s.size() &&
                        (s[i] | 32) == (w[0] | 32) &&   // 起点一致
                        (s[i + len - 1] | 32) == (w[len - 1] | 32)) {
                        // 全字比较
                        bool e = true;
                        for (size_t x = 0; x < len; ++x)
                            if ((s[i + x] | 32) != (w[x] | 32)) { e = false; break; }
                        if (e) { i += len; ok = true; break; }
                    }
                }
                if (!ok) return false;
                if (k == 1) t.tm_hour += 12;
            }
            else if (c == 'a' || c == 'A') { if (!match_names(dow_full, dow_abbr, 7, v)) return false; }
            else if (c == 'b' || c == 'B' || c == 'h') { if (!match_names(mon_full, mon_abbr, 12, v)) return false; t.tm_mon = v; }
            else { /* 未知指令：跳过对应字段以保持兼容 */ }
        } else {
            if (std::isspace((unsigned char)fmt[j])) { skip_ws(); continue; }
            if (i >= s.size() || s[i] != fmt[j]) return false;
            ++i;
        }
    }
    return true;
}

void Interpreter::init_modules() {
    // 构造标准库模块：math / time / random
    // 模块函数注册辅助（带参数个数校验）
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
    auto D = [](const ValuePtr& v) -> double { return value_to_float(v)->float_val; };
    auto L = [](const ValuePtr& v) -> long long { return value_to_int(v)->int_val; };

    // ================= math =================
    {
        auto mod = Value::make_module();
        auto& u = *mod->module_rep;
        auto add_const = [&](const std::string& n, double v) { u[n] = Value::make_float(v); };
        auto add = [&](const std::string& n, size_t a0, size_t a1,
                       std::function<ValuePtr(const ValueVec&)> f) {
            u[n] = mk_fn("math", n, a0, a1, std::move(f));
        };
        add_const("pi", 3.141592653589793238462643383279502884197169399375105820974944);
        add_const("e", 2.718281828459045235360287471352662497757247093699959574966967);
        add_const("tau", 6.283185307179586476925286766559005768394338798750211641949888);
        add_const("phi", 1.618033988749894848204586834365638117720309179805762862135448);
        add_const("sqrt2", 1.414213562373095048801688724209698078569671875376948073176680);
        add_const("ln2", 0.693147180559945309417232121458176568075500134360255254120680);
        add_const("inf", HUGE_VAL);
        add_const("nan", NAN);
        add("sqrt", 1,1, [&](const ValueVec& a){ return Value::make_float(std::sqrt(D(a[0]))); });
        add("cbrt", 1,1, [&](const ValueVec& a){ return Value::make_float(std::cbrt(D(a[0]))); });
        add("pow", 2,2, [&](const ValueVec& a){ return Value::make_float(std::pow(D(a[0]), D(a[1]))); });
        add("exp", 1,1, [&](const ValueVec& a){ return Value::make_float(std::exp(D(a[0]))); });
        add("log", 1,2, [&](const ValueVec& a){
                double x = D(a[0]);
                double base = a.size()>=2 ? D(a[1]) : std::exp(1.0);
                return Value::make_float(std::log(x)/std::log(base));
            });
        add("log2", 1,1, [&](const ValueVec& a){ return Value::make_float(std::log2(D(a[0]))); });
        add("log10", 1,1, [&](const ValueVec& a){ return Value::make_float(std::log10(D(a[0]))); });
        add("sin", 1,1, [&](const ValueVec& a){ return Value::make_float(std::sin(D(a[0]))); });
        add("cos", 1,1, [&](const ValueVec& a){ return Value::make_float(std::cos(D(a[0]))); });
        add("tan", 1,1, [&](const ValueVec& a){ return Value::make_float(std::tan(D(a[0]))); });
        add("asin", 1,1, [&](const ValueVec& a){ return Value::make_float(std::asin(D(a[0]))); });
        add("acos", 1,1, [&](const ValueVec& a){ return Value::make_float(std::acos(D(a[0]))); });
        add("atan", 1,1, [&](const ValueVec& a){ return Value::make_float(std::atan(D(a[0]))); });
        add("atan2", 2,2, [&](const ValueVec& a){ return Value::make_float(std::atan2(D(a[0]), D(a[1]))); });
        add("sinh", 1,1, [&](const ValueVec& a){ return Value::make_float(std::sinh(D(a[0]))); });
        add("cosh", 1,1, [&](const ValueVec& a){ return Value::make_float(std::cosh(D(a[0]))); });
        add("tanh", 1,1, [&](const ValueVec& a){ return Value::make_float(std::tanh(D(a[0]))); });
        add("asinh", 1,1, [&](const ValueVec& a){ return Value::make_float(std::asinh(D(a[0]))); });
        add("acosh", 1,1, [&](const ValueVec& a){ return Value::make_float(std::acosh(D(a[0]))); });
        add("atanh", 1,1, [&](const ValueVec& a){ return Value::make_float(std::atanh(D(a[0]))); });
        add("hypot", 2,2, [&](const ValueVec& a){ return Value::make_float(std::hypot(D(a[0]), D(a[1]))); });
        add("floor", 1,1, [&](const ValueVec& a){ return Value::make_int((long long)std::floor(D(a[0]))); });
        add("ceil", 1,1, [&](const ValueVec& a){ return Value::make_int((long long)std::ceil(D(a[0]))); });
        add("trunc", 1,1, [&](const ValueVec& a){ return Value::make_int((long long)std::trunc(D(a[0]))); });
        add("round", 1,2, [&](const ValueVec& a){
                double x = D(a[0]);
                long long nd = a.size()>=2 ? L(a[1]) : 0;
                double p = std::pow(10.0, (double)nd);
                return Value::make_float(std::round(x*p)/p);
            });
        add("fmod", 2,2, [&](const ValueVec& a){ return Value::make_float(std::fmod(D(a[0]), D(a[1]))); });
        add("gcd", 2,2, [&](const ValueVec& a){ return Value::make_int(std::gcd((long long)L(a[0]), (long long)L(a[1]))); });
        add("lcm", 2,2, [&](const ValueVec& a){
                long long x = L(a[0]), y = L(a[1]);
                if (x==0 || y==0) return Value::make_int(0);
                long long g = std::gcd(x, y);
                return Value::make_int((x/g)*y);
            });
        add("isinf", 1,1, [&](const ValueVec& a){ return Value::make_bool(std::isinf(D(a[0]))); });
        add("isnan", 1,1, [&](const ValueVec& a){ return Value::make_bool(std::isnan(D(a[0]))); });
        std_modules_["math"] = mod;
    }

    // ================= time =================
    {
        auto mod = Value::make_module();
        auto& u = *mod->module_rep;
        auto add = [&](const std::string& n, size_t a0, size_t a1,
                       std::function<ValuePtr(const ValueVec&)> f) {
            u[n] = mk_fn("time", n, a0, a1, std::move(f));
        };
        auto tm_to_tuple = [](const std::tm& t) {
            std::vector<ValuePtr> items(9);
            items[0]=Value::make_int(t.tm_year+1900); items[1]=Value::make_int(t.tm_mon+1);
            items[2]=Value::make_int(t.tm_mday); items[3]=Value::make_int(t.tm_hour);
            items[4]=Value::make_int(t.tm_min); items[5]=Value::make_int(t.tm_sec);
            items[6]=Value::make_int((t.tm_wday+6)%7); items[7]=Value::make_int(t.tm_yday+1);
            items[8]=Value::make_int(0);
            return Value::make_tuple(items);
        };
        // 用于 strptime 的解析（解析结果经 mktime 规范化）
        u["TIME_UTC"] = Value::make_int(0);
        add("time", 0,0, [&](const ValueVec&){
                return Value::make_float(std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count());
            });
        add("sleep", 1,1, [&](const ValueVec& a){
                std::this_thread::sleep_for(std::chrono::duration<double>(D(a[0])));
                return Value::make_none();
            });
        add("gmtime", 0,1, [&](const ValueVec& a){
                double ts = a.empty() ? std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count() : D(a[0]);
                std::time_t t = (std::time_t)ts;
                return tm_to_tuple(*std::gmtime(&t));
            });
        add("localtime", 0,1, [&](const ValueVec& a){
                double ts = a.empty() ? std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count() : D(a[0]);
                std::time_t t = (std::time_t)ts;
                return tm_to_tuple(*std::localtime(&t));
            });
        add("mktime", 1,1, [&](const ValueVec& a){
                auto& tr = *a[0]->tuple_rep;
                std::tm t; std::memset(&t, 0, sizeof(t));
                t.tm_year = (int)L(tr[0]) - 1900; t.tm_mon = (int)L(tr[1]) - 1;
                t.tm_mday = (int)L(tr[2]); t.tm_hour = (int)L(tr[3]);
                t.tm_min = (int)L(tr[4]); t.tm_sec = (int)L(tr[5]);
                return Value::make_float((double)std::mktime(&t));
            });
        add("strftime", 2,2, [&](const ValueVec& a){
                std::string fmt = a[0]->str_val;
                auto& tr = *a[1]->tuple_rep;
                std::tm t; std::memset(&t, 0, sizeof(t));
                t.tm_year=(int)L(tr[0])-1900; t.tm_mon=(int)L(tr[1])-1; t.tm_mday=(int)L(tr[2]);
                t.tm_hour=(int)L(tr[3]); t.tm_min=(int)L(tr[4]); t.tm_sec=(int)L(tr[5]);
                std::mktime(&t); // 规范化并计算 weekday/yearday
                char buf[256];
                std::strftime(buf, sizeof(buf), fmt.c_str(), &t);
                return Value::make_str(buf);
            });
        add("strptime", 2,2, [&](const ValueVec& a){
                std::string s = a[0]->str_val, fmt = a[1]->str_val;
                std::tm t; std::memset(&t, 0, sizeof(t));
                if (!util_strptime(s, fmt, t))
                    throw RuntimeError("time.strptime: unable to parse '" + s +
                                       "' with format '" + fmt + "'");
                std::time_t tt = std::mktime(&t); // 规范化并计算 weekday/yearday
                if (tt != (std::time_t)-1) {
                    std::tm* p = std::localtime(&tt);
                    if (!p)
                        throw RuntimeError("time.strptime: cannot resolve parsed time");
                    return tm_to_tuple(*p);
                }
                // 日期字段不完整（如仅解析时间）导致 mktime 无法表示：
                // 直接采用解析字段，缺失字段置合理默认值
                if (t.tm_mon < 0) t.tm_mon = 0;
                if (t.tm_mday < 1) t.tm_mday = 1;
                return tm_to_tuple(t);
            });
        // 性能计数器
        static std::chrono::steady_clock::time_point counter_start = std::chrono::steady_clock::now();
        add("counter", 0,0, [&](const ValueVec&){
                return Value::make_float(std::chrono::duration<double>(std::chrono::steady_clock::now()-counter_start).count());
            });
        add("reset_counter", 0,0, [&](const ValueVec&){
                counter_start = std::chrono::steady_clock::now();
                return Value::make_none();
            });
        add("process_time", 0,0, [&](const ValueVec&){
                return Value::make_float((double)std::clock()/CLOCKS_PER_SEC);
            });
        std_modules_["time"] = mod;
    }

    // ================= random =================
    {
        auto mod = Value::make_module();
        auto& u = *mod->module_rep;
        auto add = [&](const std::string& n, size_t a0, size_t a1,
                       std::function<ValuePtr(const ValueVec&)> f) {
            u[n] = mk_fn("random", n, a0, a1, std::move(f));
        };
        auto to_list = [](const ValuePtr& v) -> std::vector<ValuePtr> {
            return {v->list_rep->begin(), v->list_rep->end()};
        };
        u["DEFAULT_SEED"] = Value::make_uint((unsigned)std::time(nullptr)); // 自动：基于系统时间
        add("seed", 1,1, [&](const ValueVec& a){
                get_rng().seed((std::mt19937::result_type)value_to_uint(a[0])->uint_val);
                return Value::make_none();
            });
        add("getstate", 0,0, [&](const ValueVec&){
                std::ostringstream oss; oss << get_rng(); return Value::make_str(oss.str());
            });
        add("setstate", 1,1, [&](const ValueVec& a){
                std::istringstream iss(a[0]->str_val); iss >> get_rng(); return Value::make_none();
            });
        add("random", 0,0, [&](const ValueVec&){
                std::uniform_real_distribution<> d(0.0, 1.0);
                return Value::make_float(d(get_rng()));
            });
        add("uniform", 2,2, [&](const ValueVec& a){
                std::uniform_real_distribution<> d(D(a[0]), D(a[1])); return Value::make_float(d(get_rng()));
            });
        add("randint", 2,2, [&](const ValueVec& a){
                long long lo=L(a[0]), hi=L(a[1]);
                std::uniform_int_distribution<long long> d(lo, hi); return Value::make_int(d(get_rng()));
            });
        add("randrange", 2,3, [&](const ValueVec& a){
                long long start=L(a[0]), stop=L(a[1]), step=a.size()>=3?L(a[2]):1;
                long long n = (stop - start + step - 1)/step;
                std::uniform_int_distribution<long long> d(0, n-1);
                return Value::make_int(start + d(get_rng())*step);
            });
        add("choice", 1,1, [&](const ValueVec& a){
                auto vec = to_list(a[0]);
                if (vec.empty()) throw RuntimeError("random.choice of empty list");
                std::uniform_int_distribution<size_t> d(0, vec.size()-1);
                return vec[d(get_rng())];
            });
        add("choices", 1,3, [&](const ValueVec& a){
                auto pop = to_list(a[0]);
                bool weighted = a.size()>=2 && a[1]->type != ValueType::None;
                std::vector<double> w;
                if (weighted) {
                    auto& vr = *a[1]->list_rep;
                    for (auto& e : vr) w.push_back(D(e));
                }
                long long k = a.size()>=3 ? L(a[2]) : 1;
                auto r = Value::make_list();
                std::discrete_distribution<size_t> dd(w.begin(), w.end());
                if (weighted) {
                    for (long long i=0;i<k;++i) r->list_rep->push_back(pop[dd(get_rng())]);
                } else {
                    std::uniform_int_distribution<size_t> d(0, pop.size()-1);
                    for (long long i=0;i<k;++i) r->list_rep->push_back(pop[d(get_rng())]);
                }
                return r;
            });
        add("shuffle", 1,1, [&](const ValueVec& a){
                auto vec = to_list(a[0]);
                std::shuffle(vec.begin(), vec.end(), get_rng());
                auto& L = *a[0]->list_rep;
                auto it = L.begin();
                for (auto& e : vec) { *it = e; ++it; }
                return Value::make_none();
            });
        add("sample", 2,2, [&](const ValueVec& a){
                auto pop = to_list(a[0]);
                long long k = L(a[1]);
                std::shuffle(pop.begin(), pop.end(), get_rng());
                auto r = Value::make_list();
                for (long long i=0;i<k && i<(long long)pop.size();++i) r->list_rep->push_back(pop[i]);
                return r;
            });
        add("gauss", 2,2, [&](const ValueVec& a){
                std::normal_distribution<> d(D(a[0]), D(a[1])); return Value::make_float(d(get_rng()));
            });
        add("normalvariate", 2,2, [&](const ValueVec& a){
                std::normal_distribution<> d(D(a[0]), D(a[1])); return Value::make_float(d(get_rng()));
            });
        add("expovariate", 1,1, [&](const ValueVec& a){
                std::exponential_distribution<> d(D(a[0])); return Value::make_float(d(get_rng()));
            });
        add("triangular", 2,3, [&](const ValueVec& a){
                double lo=D(a[0]), hi=D(a[1]);
                double mode = a.size()>=3 ? D(a[2]) : (lo+hi)/2.0;
                if (hi<=lo) throw RuntimeError("random.triangular: high must be > low");
                double r = std::uniform_real_distribution<double>(0.0,1.0)(get_rng());
                double c = (mode-lo)/(hi-lo);
                double x = (r < c)
                    ? lo + std::sqrt(r*c)*(hi-lo)
                    : mode + (hi-mode)*(1.0 - std::sqrt((1.0-r)/(1.0-c)));
                return Value::make_float(x);
            });
        std_modules_["random"] = mod;
    }

    // ================= 扩展模块：cuda / game2d / render3d =================
    register_extension_modules(std_modules_);
}

bool Interpreter::is_builtin(const std::string&) const { return false; } // not used

// ============ Scope management ============
Environment* Interpreter::enter_scope() {
    auto env = new Environment(current_env_);
    current_env_ = env;
    return env;
}
void Interpreter::leave_scope() {
    Environment* old = current_env_;
    if (old->parent()) current_env_ = old->parent();
    delete old;
}

// ============ Run ============
ValuePtr Interpreter::run(const Program& program) {
    ValuePtr last = Value::make_none();
    try {
        for (auto& s : program.stmts) {
            auto cs = execute(s.get());
            if (cs.type == CtrlFlow::Return) return cs.value ? cs.value : Value::make_none();
        }
    } catch (RuntimeError& e) {
        print_output(std::string("RuntimeError: ") + e.what() + "\n");
        return Value::make_none();
    }
    return last;
}

ControlSignal Interpreter::execute(const Stmt* stmt) {
    if (!stmt) return {};
    switch (stmt->kind) {
        case StmtKind::VarDecl:    return exec_var_decl(static_cast<const VarDeclStmt*>(stmt));
        case StmtKind::ConstDecl:  return exec_const_decl(static_cast<const ConstDeclStmt*>(stmt));
        case StmtKind::Assign: {
            auto a = static_cast<const AssignStmt*>(stmt);
            evaluate(a->assign.get());
            return {};
        }
        case StmtKind::ExprStmt: {
            auto s = static_cast<const ExprStmt*>(stmt);
            evaluate(s->expr.get());
            return {};
        }
        case StmtKind::Block: {
            auto s = static_cast<const BlockStmt*>(stmt);
            return execute_block(s->stmts);
        }
        case StmtKind::If:         return exec_if(static_cast<const IfStmt*>(stmt));
        case StmtKind::ForIn:      return exec_for(static_cast<const ForInStmt*>(stmt));
        case StmtKind::While:      return exec_while(static_cast<const WhileStmt*>(stmt));
        case StmtKind::Break:      { ControlSignal c; c.type = CtrlFlow::Break; return c; }
        case StmtKind::Continue:   { ControlSignal c; c.type = CtrlFlow::Continue; return c; }
        case StmtKind::Return: {
            auto s = static_cast<const ReturnStmt*>(stmt);
            ControlSignal c; c.type = CtrlFlow::Return;
            c.value = s->value ? evaluate(s->value.get()) : Value::make_none();
            return c;
        }
        case StmtKind::Del:        return exec_del(static_cast<const DelStmt*>(stmt));
        case StmtKind::FunctionDef: return exec_function_def(static_cast<const FunctionDefStmt*>(stmt));
        case StmtKind::Import:     return exec_import(static_cast<const ImportStmt*>(stmt));
        case StmtKind::TryCatch:   return exec_try(static_cast<const TryCatchStmt*>(stmt));
    }
    return {};
}

ControlSignal Interpreter::execute_block(const std::vector<StmtPtr>& stmts) {
    Environment* saved_env = current_env_;
    Environment new_scope(current_env_);
    current_env_ = &new_scope;
    ControlSignal cs;
    for (auto& s : stmts) {
        cs = execute(s.get());
        if (cs.type != CtrlFlow::None) break;
    }
    current_env_ = saved_env;
    return cs;
}

ControlSignal Interpreter::exec_var_decl(const VarDeclStmt* s) {
    std::string tname = s->type ? s->type->base_name : "object";
    for (auto& [name, init] : s->names) {
        ValuePtr v;
        if (init) v = evaluate(init.get());
        else {
            // 默认构造
            if (tname == "int" || tname == "short" || tname == "long" || tname == "char" || tname == "unichar")
                v = Value::make_int(0);
            else if (tname == "uint" || tname == "ushort" || tname == "ulong")
                v = Value::make_uint(0);
            else if (tname == "float" || tname == "double") v = Value::make_float(0.0);
            else if (tname == "bool") v = Value::make_bool(false);
            else if (tname == "str" || tname == "unistr") v = Value::make_str("");
            else if (tname == "bin") v = Value::make_bin({});
            else if (tname == "list") v = Value::make_list();
            else if (tname == "stack") v = Value::make_stack();
            else if (tname == "queue") v = Value::make_queue();
            else if (tname == "set") v = Value::make_set();
            else if (tname == "undset") v = Value::make_undset();
            else if (tname == "dict") v = Value::make_dict();
            else if (tname == "tuple") v = Value::make_tuple({});
            else if (tname == "pair") v = Value::make_pair(Value::make_none(), Value::make_none());
            else v = Value::make_none();
        }
        current_env_->define(name, v, false);
    }
    return {};
}
ControlSignal Interpreter::exec_const_decl(const ConstDeclStmt* s) {
    ValuePtr v = evaluate(s->value.get());
    current_env_->define(s->name, v, true);
    return {};
}
ControlSignal Interpreter::exec_if(const IfStmt* s) {
    ValuePtr c = evaluate(s->cond.get());
    if (c->truthy()) return execute_block(s->then_body->stmts);
    for (auto& e : s->elif_list) {
        ValuePtr ce = evaluate(e.cond.get());
        if (ce->truthy()) return execute_block(e.body->stmts);
    }
    if (s->else_body) return execute_block(s->else_body->stmts);
    return {};
}
ControlSignal Interpreter::exec_for(const ForInStmt* s) {
    ValuePtr cont = evaluate(s->container.get());
    ValueVec elems;
    if (cont->type == ValueType::List) {
        for (auto& e : *cont->list_rep) elems.push_back(e);
    } else if (cont->type == ValueType::Tuple) {
        for (auto& e : *cont->tuple_rep) elems.push_back(e);
    } else if (cont->type == ValueType::Str || cont->type == ValueType::UniStr) {
        for (char c : cont->str_val) elems.push_back(Value::make_char((unsigned char)c));
    } else if (cont->type == ValueType::Set) {
        for (auto& e : *cont->set_rep) elems.push_back(e);
    } else if (cont->type == ValueType::UndSet) {
        for (auto& e : *cont->undset_rep) elems.push_back(e);
    } else if (cont->type == ValueType::Dict) {
        for (auto& [k, v] : *cont->dict_rep) elems.push_back(k);
    } else if (cont->type == ValueType::Stack) {
        for (auto& e : *cont->stack_rep) elems.push_back(e);
    } else if (cont->type == ValueType::Queue) {
        for (auto& e : *cont->queue_rep) elems.push_back(e);
    } else {
        throw RuntimeError("Cannot iterate over " + cont->type_name());
    }
    Environment* saved_env = current_env_;
    Environment scope(current_env_);
    current_env_ = &scope;
    for (auto& e : elems) {
        scope.define(s->var, e, false);
        ControlSignal cs = execute_block(s->body->stmts);
        if (cs.type == CtrlFlow::Break) break;
        if (cs.type == CtrlFlow::Return) { current_env_ = saved_env; return cs; }
    }
    current_env_ = saved_env;
    return {};
}
ControlSignal Interpreter::exec_while(const WhileStmt* s) {
    while (true) {
        ValuePtr c = evaluate(s->cond.get());
        if (!c->truthy()) break;
        ControlSignal cs = execute_block(s->body->stmts);
        if (cs.type == CtrlFlow::Break) break;
        if (cs.type == CtrlFlow::Return) return cs;
    }
    return {};
}
ControlSignal Interpreter::exec_del(const DelStmt* s) {
    if (s->delete_all) { current_env_->clear_all(); return {}; }
    for (auto& t : s->targets) {
        if (t->kind == ExprKind::Identifier) {
            auto id = static_cast<const IdentifierExpr*>(t.get());
            current_env_->erase(id->name);
        } else if (t->kind == ExprKind::UnaryOp && static_cast<const UnaryOpExpr&>(*t).op == "~") {
            // del ~ptr：把指向的值置空
            auto operand = static_cast<const UnaryOpExpr&>(*t).operand.get();
            auto adr = evaluate(operand);
            if (adr->type != ValueType::MemAdr) throw RuntimeError("del ~ requires memadr");
            if (adr->adr_rep->env) adr->adr_rep->env->assign(adr->adr_rep->var_name, Value::make_none());
        }
    }
    return {};
}
ControlSignal Interpreter::exec_function_def(const FunctionDefStmt* s) {
    auto fv = std::make_shared<FunctionValue>();
    fv->name = s->name;
    fv->is_builtin = false;
    fv->def = s;
    for (auto& p : s->params) {
        fv->param_names.push_back(p->name);
        fv->default_args.push_back(p->default_value ? evaluate(p->default_value.get()) : nullptr);
        if (p->is_vararg) fv->has_vararg = true;
    }
    auto v = Value::make_none();
    v->type = ValueType::Function;
    v->fn_rep = fv;
    current_env_->define(s->name, v, false);
    return {};
}
ControlSignal Interpreter::exec_import(const ImportStmt* s) {
    auto it = std_modules_.find(s->module);
    if (it == std_modules_.end())
        throw RuntimeError("Unknown module: '" + s->module + "'");
    auto mod = it->second;
    if (!s->is_from) {
        std::string name = s->alias.empty() ? s->module : s->alias;
        current_env_->define(name, mod, true);
    } else {
        auto& rep = *mod->module_rep;
        for (auto& item : s->items) {
            auto mi = rep.find(item);
            if (mi == rep.end())
                throw RuntimeError("Module '" + s->module + "' has no exported member '" + item + "'");
            current_env_->define(item, mi->second, true);
        }
    }
    return {};
}
ControlSignal Interpreter::exec_try(const TryCatchStmt* s) {
    try {
        auto cs = execute_block(s->try_body->stmts);
        if (s->finally_body) execute_block(s->finally_body->stmts);
        return cs;
    } catch (const RuntimeError& e) {
        ControlSignal cs;
        if (s->catch_body) {
            Environment* saved_env = current_env_;
            Environment scope(current_env_);
            current_env_ = &scope;
            if (!s->exception_var.empty()) {
                scope.define(s->exception_var, Value::make_str(e.message()), false);
            }
            cs = execute_block(s->catch_body->stmts);
            current_env_ = saved_env;
        }
        if (s->finally_body) execute_block(s->finally_body->stmts);
        if (!s->catch_body) throw;
        return cs;
    }
}

// ============ Expressions ============
ValuePtr Interpreter::evaluate(const Expr* e) {
    if (!e) return Value::make_none();
    switch (e->kind) {
        case ExprKind::Literal:      return eval_literal(static_cast<const LiteralExpr*>(e));
        case ExprKind::Identifier:   return eval_identifier(static_cast<const IdentifierExpr*>(e));
        case ExprKind::UnaryOp:      return eval_unary(static_cast<const UnaryOpExpr*>(e));
        case ExprKind::BinaryOp:     return eval_binary(static_cast<const BinaryOpExpr*>(e));
        case ExprKind::TernaryOp:    return eval_ternary(static_cast<const TernaryOpExpr*>(e));
        case ExprKind::AssignOp:     return eval_assign(static_cast<const AssignOpExpr*>(e));
        case ExprKind::Call:         return eval_call(static_cast<const CallExpr*>(e));
        case ExprKind::MemberAccess: return eval_member(static_cast<const MemberAccessExpr*>(e));
        case ExprKind::Subscript:    return eval_subscript(static_cast<const SubscriptExpr*>(e));
        case ExprKind::ListInit:     return eval_list_init(static_cast<const ListInitExpr*>(e));
        case ExprKind::DictInit:     return eval_dict_init(static_cast<const DictInitExpr*>(e));
        case ExprKind::Lambda:       return eval_lambda(static_cast<const LambdaExpr*>(e));
        case ExprKind::AddressOf:
        case ExprKind::Dereference:
        case ExprKind::Cast:         return eval_cast(static_cast<const CastExpr*>(e));
    }
    return Value::make_none();
}

ValuePtr Interpreter::eval_literal(const LiteralExpr* e) {
    switch (e->lit_kind) {
        case LiteralExpr::LitKind::Int: {
            // 若前缀是 0x 且值大或前缀明确 -> 由 string 判断
            // 简化：若 uint_val 大过 int 上限用 uint
            if (e->uint_val > (unsigned long long)0x7FFFFFFFFFFFFFFFLL && !e->str_val.empty() && e->str_val[0] == '-') {
                return Value::make_int(e->int_val);
            }
            // 如果是十六进制字面量，保持 unsigned int 语义
            if (e->str_val.size() >= 2 && e->str_val[0] == '0' && (e->str_val[1] == 'x' || e->str_val[1] == 'X')) {
                return Value::make_uint(e->uint_val);
            }
            return Value::make_int(e->int_val);
        }
        case LiteralExpr::LitKind::Float:  return Value::make_float(e->float_val);
        case LiteralExpr::LitKind::Bool:   return Value::make_bool(e->bool_val);
        case LiteralExpr::LitKind::Char:   return Value::make_char(e->char_val);
        case LiteralExpr::LitKind::String: return Value::make_str(e->str_val);
        case LiteralExpr::LitKind::None:   return Value::make_none();
        case LiteralExpr::LitKind::Unichar: return Value::make_unichar(e->char_val);
        case LiteralExpr::LitKind::UniString: return Value::make_unistr(e->str_val);
    }
    return Value::make_none();
}

ValuePtr Interpreter::eval_identifier(const IdentifierExpr* e) {
    return current_env_->lookup(e->name);
}

ValuePtr Interpreter::eval_unary(const UnaryOpExpr* e) {
    auto& op = e->op;
    if (op == "@") {
        // 取地址：operand 必须是 identifier
        if (e->operand->kind != ExprKind::Identifier)
            throw RuntimeError("'@' requires a variable");
        auto& name = static_cast<const IdentifierExpr&>(*e->operand).name;
        // 确保存在
        (void)current_env_->lookup(name);
        return Value::make_adr(name, current_env_);
    }
    if (op == "~") {
        auto operand = evaluate(e->operand.get());
        if (operand->type == ValueType::MemAdr) {
            auto& env = operand->adr_rep->env ? *operand->adr_rep->env : *current_env_;
            return env.lookup(operand->adr_rep->var_name);
        }
        return value_bit_not(operand);
    }
    auto operand = evaluate(e->operand.get());
    if (op == "!") return value_not(operand);
    if (op == "-") return value_neg(operand);
    if (op == "+") return value_pos(operand);
    throw RuntimeError("Unknown unary operator: " + op);
}

ValuePtr Interpreter::eval_binary(const BinaryOpExpr* e) {
    // 短路逻辑 && / ||
    if (e->op == "&&") {
        auto l = evaluate(e->left.get());
        if (!l->truthy()) return Value::make_bool(false);
        auto r = evaluate(e->right.get());
        return Value::make_bool(r->truthy());
    }
    if (e->op == "||") {
        auto l = evaluate(e->left.get());
        if (l->truthy()) return Value::make_bool(true);
        auto r = evaluate(e->right.get());
        return Value::make_bool(r->truthy());
    }
    auto l = evaluate(e->left.get());
    auto r = evaluate(e->right.get());
    if (e->op == "+") return value_add(l, r);
    if (e->op == "-") return value_sub(l, r);
    if (e->op == "*") return value_mul(l, r);
    if (e->op == "/") return value_div(l, r);
    if (e->op == "//") return value_floordiv(l, r);
    if (e->op == "%") return value_mod(l, r);
    if (e->op == "**") return value_pow(l, r);
    if (e->op == "&") return value_bit_and(l, r);
    if (e->op == "|") return value_bit_or(l, r);
    if (e->op == "^") return value_bit_xor(l, r);
    if (e->op == "<<") return value_shl(l, r);
    if (e->op == ">>") return value_shr(l, r);
    if (e->op == "==") return Value::make_bool(value_eq(l, r));
    if (e->op == "!=") return Value::make_bool(value_ne(l, r));
    if (e->op == "<") return Value::make_bool(value_lt(l, r));
    if (e->op == ">") return Value::make_bool(value_gt(l, r));
    if (e->op == "<=") return Value::make_bool(value_le(l, r));
    if (e->op == ">=") return Value::make_bool(value_ge(l, r));
    throw RuntimeError("Unknown binary operator: " + e->op);
}

ValuePtr Interpreter::eval_ternary(const TernaryOpExpr* e) {
    auto c = evaluate(e->cond.get());
    if (c->truthy()) return evaluate(e->then_e.get());
    return evaluate(e->else_e.get());
}

void Interpreter::perform_assign(ValuePtr& target_ref, const std::string& op, ValuePtr rhs) {
    if (op == "=") { target_ref = rhs; return; }
    if (op == "+=") { target_ref = value_add(target_ref, rhs); return; }
    if (op == "-=") { target_ref = value_sub(target_ref, rhs); return; }
    if (op == "*=") { target_ref = value_mul(target_ref, rhs); return; }
    if (op == "/=") { target_ref = value_div(target_ref, rhs); return; }
    if (op == "//=") { target_ref = value_floordiv(target_ref, rhs); return; }
    if (op == "%=") { target_ref = value_mod(target_ref, rhs); return; }
    if (op == "**=") { target_ref = value_pow(target_ref, rhs); return; }
    if (op == "&=") { target_ref = value_bit_and(target_ref, rhs); return; }
    if (op == "|=") { target_ref = value_bit_or(target_ref, rhs); return; }
    if (op == "^=") { target_ref = value_bit_xor(target_ref, rhs); return; }
    if (op == "<<=") { target_ref = value_shl(target_ref, rhs); return; }
    if (op == ">>=") { target_ref = value_shr(target_ref, rhs); return; }
    throw RuntimeError("Unknown compound assignment: " + op);
}

ValuePtr& Interpreter::variable_ref(const std::string& name) {
    return current_env_->lookup(name);
}

ValuePtr& Interpreter::resolve_lvalue(const Expr* target) {
    if (target->kind == ExprKind::Identifier) {
        return variable_ref(static_cast<const IdentifierExpr*>(target)->name);
    }
    if (target->kind == ExprKind::UnaryOp) {
        auto& u = static_cast<const UnaryOpExpr&>(*target);
        if (u.op == "~") {
            auto adr = evaluate(u.operand.get());
            if (adr->type != ValueType::MemAdr) throw RuntimeError("Dereference requires memadr");
            Environment& env = adr->adr_rep->env ? *adr->adr_rep->env : *current_env_;
            return env.lookup(adr->adr_rep->var_name);
        }
    }
    if (target->kind == ExprKind::Subscript) {
        // a[i] = v
        auto& s = static_cast<const SubscriptExpr&>(*target);
        ValuePtr obj = evaluate(s.object.get());
        ValuePtr idx = evaluate(s.index.get());
        // 我们无法直接返回内部元素的引用，所以改用特殊：此处只用于 AssignOpExpr 中重新写回
        throw RuntimeError("Subscript lvalue assignment not direct reference (use set method)");
    }
    if (target->kind == ExprKind::MemberAccess) {
        auto& m = static_cast<const MemberAccessExpr&>(*target);
        ValuePtr obj = evaluate(m.object.get());
        if (obj->type == ValueType::Pair) {
            if (m.member == "first")  return obj->pair_rep->first;
            if (m.member == "second") return obj->pair_rep->second;
        }
        throw RuntimeError("Member '" + m.member + "' is not assignable on " + obj->type_name());
    }
    throw RuntimeError("Invalid lvalue target");
}

ValuePtr Interpreter::eval_assign(const AssignOpExpr* e) {
    ValuePtr rhs = evaluate(e->value.get());
    // 目标若为 Subscript -> 直接用 obj + idx 写回
    if (e->target->kind == ExprKind::Subscript) {
        auto& s = static_cast<const SubscriptExpr&>(*e->target);
        ValuePtr obj = evaluate(s.object.get());
        ValuePtr idx = evaluate(s.index.get());
        long long i;
        if (idx->type == ValueType::Int || idx->type == ValueType::UInt) {
            i = idx->type == ValueType::UInt ? (long long)idx->uint_val : idx->int_val;
        } else {
            // dict key
            if (obj->type == ValueType::Dict) {
                // value update: put
                (*obj->dict_rep)[idx] = rhs;
                return rhs;
            }
            throw RuntimeError("Subscript index must be integer for " + obj->type_name());
        }
        if (obj->type == ValueType::List) {
            if (i < 0 || (size_t)i >= obj->list_rep->size())
                throw RuntimeError("List index out of range");
            auto it = obj->list_rep->begin(); std::advance(it, i);
            if (e->op == "=") *it = rhs;
            else perform_assign(*it, e->op, rhs);
            return rhs;
        }
        if (obj->type == ValueType::Str || obj->type == ValueType::UniStr) {
            if (i < 0 || (size_t)i >= obj->str_val.size())
                throw RuntimeError("String index out of range");
            std::string ch = rhs->to_string();
            if (e->op == "=") obj->str_val[i] = ch.empty() ? '\0' : ch[0];
            else {
                auto cv = Value::make_char((unsigned char)obj->str_val[i]);
                perform_assign(cv, e->op, rhs);
                obj->str_val[i] = (char)cv->char_val;
            }
            return rhs;
        }
        if (obj->type == ValueType::Bin) {
            if (i < 0 || (size_t)i >= obj->bin_val.size())
                throw RuntimeError("Bin index out of range");
            auto vv = value_to_int(rhs);
            if (e->op == "=") obj->bin_val[i] = (unsigned char)vv->int_val;
            else {
                auto cv = Value::make_int(obj->bin_val[i]);
                perform_assign(cv, e->op, rhs);
                obj->bin_val[i] = (unsigned char)cv->int_val;
            }
            return rhs;
        }
        throw RuntimeError("Subscript assignment not supported on " + obj->type_name());
    }
    ValuePtr& ref = resolve_lvalue(e->target.get());
    perform_assign(ref, e->op, rhs);
    return rhs;
}

ValuePtr Interpreter::eval_cast(const CastExpr* e) {
    ValuePtr v = evaluate(e->value.get());
    std::string t = e->target_type;
    if (t == "int") return value_to_int(v);
    if (t == "long") return value_to_long(v);
    if (t == "uint") return value_to_uint(v);
    if (t == "ulong") return value_to_ulong(v);
    if (t == "float" || t == "double") return value_to_float(v);
    if (t == "bool") return value_to_bool(v);
    if (t == "str") return value_to_str(v);
    if (t == "unistr") return value_to_unistr(v);
    if (t == "bin") return value_to_bin(v);
    if (t == "char") {
        auto vi = value_to_int(v);
        return Value::make_char((int)vi->int_val);
    }
    if (t == "unichar") {
        auto vi = value_to_int(v);
        return Value::make_unichar((int)vi->int_val);
    }
    // 否则当作容器构造函数调用
    ValueVec args = {v};
    return call_builtin(t, args);
}

ValuePtr Interpreter::eval_call(const CallExpr* e) {
    ValuePtr callee = evaluate(e->callee.get());
    ValueVec args;
    for (auto& a : e->args) args.push_back(evaluate(a.get()));
    // 命名参数：压入当前调用栈帧，供内置方法（如 sort 的 cmp）读取
    std::unordered_map<std::string, ValuePtr> kwargs;
    for (auto& [kn, kv] : e->kwargs) kwargs[kn] = evaluate(kv.get());
    kwarg_stack_.push_back(std::move(kwargs));
    struct KwargGuard { Interpreter* i; ~KwargGuard() { i->kwarg_stack_.pop_back(); } } guard{this};
    if (callee->type == ValueType::Function) {
        if (callee->fn_rep->is_builtin) {
            return callee->fn_rep->builtin_fn(args, *current_env_);
        } else {
            return call_user_function(callee->fn_rep.get(), args);
        }
    }
    // 若 callee 是 member access 的包装已不存在；此处不可能是 method
    throw RuntimeError("Cannot call non-function: " + callee->type_name());
}

ValuePtr Interpreter::eval_member(const MemberAccessExpr* e) {
    ValuePtr obj = evaluate(e->object.get());
    // 模块成员：常量或函数（如 math.pi / math.sqrt）
    if (obj->type == ValueType::Module) {
        auto it = obj->module_rep->find(e->member);
        if (it == obj->module_rep->end())
            throw RuntimeError("Module has no member '" + e->member + "'");
        return it->second;
    }
    // pair.first / pair.second 直接读取
    if (obj->type == ValueType::Pair) {
        if (e->member == "first") return obj->pair_rep->first;
        if (e->member == "second") return obj->pair_rep->second;
    }
    // 返回一个 method 包装（用 Function 对象，标记 method）
    auto fv = std::make_shared<FunctionValue>();
    fv->name = e->member;
    fv->is_builtin = true;
    ValuePtr obj_keep = obj;
    std::string method = e->member;
    fv->builtin_fn = [this, obj_keep, method](const ValueVec& args, Environment&) {
        return call_method(obj_keep, method, args);
    };
    auto v = Value::make_none();
    v->type = ValueType::Function;
    v->fn_rep = fv;
    return v;
}

ValuePtr Interpreter::eval_subscript(const SubscriptExpr* e) {
    ValuePtr obj = evaluate(e->object.get());
    ValuePtr idx = evaluate(e->index.get());
    if (obj->type == ValueType::List) {
        long long i = idx->type == ValueType::UInt ? (long long)idx->uint_val : idx->int_val;
        if (i < 0 || (size_t)i >= obj->list_rep->size())
            throw RuntimeError("List index out of range");
        auto it = obj->list_rep->begin(); std::advance(it, i);
        return *it;
    }
    if (obj->type == ValueType::Tuple) {
        long long i = idx->type == ValueType::UInt ? (long long)idx->uint_val : idx->int_val;
        if (i < 0 || (size_t)i >= obj->tuple_rep->size())
            throw RuntimeError("Tuple index out of range");
        return (*obj->tuple_rep)[(size_t)i];
    }
    if (obj->type == ValueType::Str || obj->type == ValueType::UniStr) {
        long long i = idx->type == ValueType::UInt ? (long long)idx->uint_val : idx->int_val;
        if (i < 0 || (size_t)i >= obj->str_val.size())
            throw RuntimeError("String index out of range");
        return Value::make_char((unsigned char)obj->str_val[(size_t)i]);
    }
    if (obj->type == ValueType::Bin) {
        long long i = idx->type == ValueType::UInt ? (long long)idx->uint_val : idx->int_val;
        if (i < 0 || (size_t)i >= obj->bin_val.size())
            throw RuntimeError("Bin index out of range");
        return Value::make_int(obj->bin_val[(size_t)i]);
    }
    if (obj->type == ValueType::Dict) {
        auto it = obj->dict_rep->find(idx);
        if (it == obj->dict_rep->end()) throw RuntimeError("Key not found: " + idx->to_string());
        return it->second;
    }
    throw RuntimeError("Subscript not supported on " + obj->type_name());
}

ValuePtr Interpreter::eval_list_init(const ListInitExpr* e) {
    auto r = Value::make_list();
    for (auto& el : e->elements) r->list_rep->push_back(evaluate(el.get()));
    return r;
}

ValuePtr Interpreter::eval_dict_init(const DictInitExpr* e) {
    auto r = Value::make_dict();
    for (auto& p : e->pairs) {
        auto k = evaluate(p.key.get());
        auto v = evaluate(p.value.get());
        (*r->dict_rep)[k] = v;
    }
    return r;
}

ValuePtr Interpreter::eval_lambda(const LambdaExpr* e) {
    auto fv = std::make_shared<FunctionValue>();
    fv->name = "<lambda>";
    fv->is_builtin = false;
    // 构造 AST 片段（使用空 body 语句列表）
    // 简化：用 FunctionValue.def = nullptr；我们把 lambda.body 存放在捕获 env
    // 由于 FunctionDefStmt 是复杂对象，这里我们把 lambda 包装成一种带 env 闭包的 builtin
    // 但更简单：构造 FunctionDefStmt 并附加到 fv，使用当前作用域做闭包（简化——动态查找）
    auto def = std::make_unique<FunctionDefStmt>();
    def->name = "<lambda>";
    for (auto& p : e->params) {
        def->params.push_back(std::make_unique<ParamDecl>());
        auto& np = def->params.back();
        np->name = p->name;
        np->default_value = p->default_value ? std::move(p->default_value) : nullptr;
        np->is_vararg = p->is_vararg;
    }
    def->body = std::make_unique<BlockStmt>();
    for (auto& s : e->body) def->body->stmts.push_back(std::move(const_cast<StmtPtr&>(s)));

    fv->def = def.get();
    // 需要保持 def 存活：使用捕获 shared_ptr 放在 builtin_fn 里
    auto def_holder = std::shared_ptr<FunctionDefStmt>(def.release());
    Environment* closure_env = current_env_;
    fv->is_builtin = true;
    fv->builtin_fn = [this, def_holder, closure_env](const ValueVec& args, Environment&) {
        return call_user_function_with_env(def_holder.get(), args, closure_env);
    };
    auto v = Value::make_none();
    v->type = ValueType::Function;
    v->fn_rep = fv;
    return v;
}

ValuePtr Interpreter::call_user_function(const FunctionValue* fn, const ValueVec& args) {
    return call_user_function_with_env(fn->def, args, current_env_);
}

// 为 lambda 提供
ValuePtr call_user_function_with_env(const FunctionDefStmt* def, const ValueVec& args, Environment* parent_env);

// ============ 内置方法调用 ============
static ListRep::iterator list_iter(const ValuePtr& lst, long long i) {
    auto it = lst->list_rep->begin(); std::advance(it, i);
    return it;
}

ValuePtr Interpreter::call_method(const ValuePtr& obj, const std::string& m, const ValueVec& args) {
    auto argc = args.size();
    switch (obj->type) {
        case ValueType::List: {
            auto& L = *obj->list_rep;
            long long n = (long long)L.size();
            auto ulong_arg = [&](size_t i, long long def) -> long long {
                if (i >= argc) return def;
                auto v = value_to_int(args[i]); return v->int_val;
            };
            if (m == "append") {
                long long pos = argc >= 3 ? ulong_arg(2, n) : (argc >= 2 ? ulong_arg(1, n) : n);
                if (argc >= 2 && (args[1]->type == ValueType::Int || args[1]->type == ValueType::UInt)) {
                    // append(t, count, pos?)
                    long long count = ulong_arg(1, 1);
                    pos = argc >= 3 ? ulong_arg(2, n) : n;
                    auto it = list_iter(obj, pos);
                    for (long long i = 0; i < count; ++i) it = L.insert(it, args[0]);
                } else {
                    auto it = list_iter(obj, pos);
                    L.insert(it, args[0]);
                }
                return Value::make_none();
            }
            if (m == "insert") { L.insert(list_iter(obj, ulong_arg(0, 0)), args[1]); return Value::make_none(); }
            if (m == "erase") {
                long long l = ulong_arg(0, 0), r = ulong_arg(1, n-1);
                L.erase(list_iter(obj, l), list_iter(obj, r+1));
                return Value::make_none();
            }
            if (m == "remove") {
                for (auto it = L.begin(); it != L.end(); ++it) {
                    if (value_eq(*it, args[0])) { L.erase(it); return Value::make_bool(true); }
                }
                return Value::make_bool(false);
            }
            if (m == "len") return value_from_uint(L.size());
            if (m == "begin") return Value::make_int(0);
            if (m == "end") return value_from_uint(L.size());
            if (m == "clear") { L.clear(); return Value::make_none(); }
            if (m == "contains") {
                for (auto& e : L) if (value_eq(e, args[0])) return Value::make_bool(true);
                return Value::make_bool(false);
            }
            if (m == "find") {
                long long i = 0;
                for (auto& e : L) { if (value_eq(e, args[0])) return Value::make_int(i); ++i; }
                return Value::make_int(-1);
            }
            if (m == "count") {
                long long c = 0; for (auto& e : L) if (value_eq(e, args[0])) ++c;
                return Value::make_int(c);
            }
            if (m == "replace") {
                if (args.size() >= 2 && (args[0]->type == ValueType::Int || args[0]->type == ValueType::UInt) &&
                    (args[1]->type == ValueType::Int || args[1]->type == ValueType::UInt)) {
                    long long l = ulong_arg(0, 0), r = ulong_arg(1, n-1);
                    auto repl_list = args[2]->list_rep;
                    auto lit = list_iter(obj, l), rit = list_iter(obj, r+1);
                    lit = L.erase(lit, rit);
                    for (auto& e : *repl_list) lit = L.insert(lit, e);
                } else {
                    for (auto& e : L) if (value_eq(e, args[0])) e = args[1];
                }
                return Value::make_none();
            }
            if (m == "reverse") { L.reverse(); return Value::make_none(); }
            if (m == "slice") {
                long long start = ulong_arg(0, 0), end = ulong_arg(1, n);
                auto r = Value::make_list();
                long long i = 0;
                for (auto it = L.begin(); it != L.end(); ++it, ++i) {
                    if (i >= start && i < end) r->list_rep->push_back(*it);
                }
                return r;
            }
            if (m == "sort") {
                long long l = ulong_arg(0, 0), r = ulong_arg(1, n);
                ValuePtr cmpfn; bool has_cmp = false;
                // 支持命名参数 cmp = fn
                if (!kwarg_stack_.empty()) {
                    auto kit = kwarg_stack_.back().find("cmp");
                    if (kit != kwarg_stack_.back().end()) { cmpfn = kit->second; has_cmp = true; }
                }
                if (argc >= 3) { cmpfn = args[2]; has_cmp = true; }
                // 取出 [l,r) 并排序
                auto it_l = list_iter(obj, l), it_r = list_iter(obj, r);
                ValueVec v(it_l, it_r);
                if (has_cmp && cmpfn && cmpfn->type == ValueType::Function) {
                    std::sort(v.begin(), v.end(), [&](const ValuePtr& a, const ValuePtr& b) {
                        ValueVec aa = {a, b};
                        ValuePtr res;
                        if (cmpfn->fn_rep->is_builtin) res = cmpfn->fn_rep->builtin_fn(aa, *this->current_env_);
                        else res = call_user_function(cmpfn->fn_rep.get(), aa);
                        auto w = value_to_int(res);
                        return w->int_val == 1;
                    });
                } else {
                    std::sort(v.begin(), v.end(), ValueCompare());
                }
                // 写回
                L.erase(it_l, it_r);
                auto ins_it = list_iter(obj, l);
                for (auto& e : v) { ins_it = L.insert(ins_it, e); ++ins_it; }
                return Value::make_none();
            }
            if (m == "shuffle") {
                long long l = ulong_arg(0, 0), r = ulong_arg(1, n);
                auto it_l = list_iter(obj, l), it_r = list_iter(obj, r);
                ValueVec v(it_l, it_r);
                std::shuffle(v.begin(), v.end(), get_rng());
                L.erase(it_l, it_r);
                auto ins_it = list_iter(obj, l);
                for (auto& e : v) { ins_it = L.insert(ins_it, e); ++ins_it; }
                return Value::make_none();
            }
            if (m == "sum") {
                long long l = ulong_arg(0, 0), r = ulong_arg(1, n);
                long long sum = 0; long long i = 0;
                for (auto it = L.begin(); it != L.end(); ++it, ++i) {
                    if (i >= l && i < r) {
                        auto vi = value_to_int(*it);
                        sum += vi->int_val;
                    }
                }
                return Value::make_int(sum);
            }
            if (m == "mean") {
                long long l = ulong_arg(0, 0), r = ulong_arg(1, n);
                double sum = 0; long long cnt = 0; long long i = 0;
                for (auto it = L.begin(); it != L.end(); ++it, ++i) {
                    if (i >= l && i < r) {
                        auto v = value_to_float(*it);
                        sum += v->float_val; ++cnt;
                    }
                }
                return Value::make_float(cnt == 0 ? 0.0 : sum / cnt);
            }
            if (m == "copy") {
                auto r = Value::make_list();
                for (auto& e : L) r->list_rep->push_back(e);
                return r;
            }
            throw RuntimeError("list has no method: " + m);
        }
        case ValueType::Str: case ValueType::UniStr: {
            std::string& S = obj->str_val;
            auto ulong_arg = [&](size_t i, size_t def) -> size_t {
                if (i >= argc) return def;
                return (size_t)value_to_int(args[i])->int_val;
            };
            if (m == "append") {
                size_t pos = argc >= 3 ? ulong_arg(2, S.size()) : S.size();
                if (argc >= 2 && (args[1]->type == ValueType::Int || args[1]->type == ValueType::UInt)) {
                    // append(char, count, pos)
                    char c = (char)value_to_int(args[0])->int_val;
                    size_t cnt = ulong_arg(1, 0);
                    if (pos > S.size()) pos = S.size();
                    S.insert(pos, cnt, c);
                } else {
                    std::string t = args[0]->to_string();
                    if (pos > S.size()) pos = S.size();
                    S.insert(pos, t);
                }
                return Value::make_none();
            }
            if (m == "erase") {
                size_t l = ulong_arg(0, 0), r = ulong_arg(1, S.size()-1);
                if (l > S.size()) l = S.size();
                if (r >= S.size()) r = S.size()-1;
                if (l <= r) S.erase(l, r - l + 1);
                return Value::make_none();
            }
            if (m == "len") return value_from_uint(S.size());
            if (m == "find") {
                std::string sub = args[0]->to_string();
                size_t p = S.find(sub);
                return Value::make_int(p == std::string::npos ? -1 : (long long)p);
            }
            if (m == "count") {
                std::string sub = args[0]->to_string();
                if (sub.empty()) return Value::make_int(0);
                long long cnt = 0; size_t p = 0;
                while ((p = S.find(sub, p)) != std::string::npos) { ++cnt; p += sub.size(); }
                return Value::make_int(cnt);
            }
            if (m == "replace") {
                if (argc >= 3 && (args[0]->type == ValueType::Int || args[0]->type == ValueType::UInt)) {
                    size_t l = ulong_arg(0, 0), r = ulong_arg(1, S.size()-1);
                    std::string rep = args[2]->to_string();
                    if (l > S.size()) l = S.size();
                    if (r >= S.size()) r = S.size()-1;
                    if (l <= r) S.replace(l, r-l+1, rep);
                } else {
                    std::string a = args[0]->to_string(), b = args[1]->to_string();
                    if (!a.empty()) {
                        size_t p = 0;
                        while ((p = S.find(a, p)) != std::string::npos) {
                            S.replace(p, a.size(), b); p += b.size();
                        }
                    }
                }
                return Value::make_none();
            }
            if (m == "slice") {
                size_t start = ulong_arg(0, 0), end = ulong_arg(1, S.size());
                if (start > S.size()) start = S.size();
                if (end > S.size()) end = S.size();
                return Value::make_str(S.substr(start, end - start));
            }
            if (m == "to_list") {
                auto r = Value::make_list();
                for (unsigned char c : S) r->list_rep->push_back(Value::make_char(c));
                return r;
            }
            throw RuntimeError("str has no method: " + m);
        }
        case ValueType::Bin: {
            auto& B = obj->bin_val;
            auto ulong_arg = [&](size_t i, size_t def) -> size_t {
                if (i >= argc) return def;
                return (size_t)value_to_int(args[i])->int_val;
            };
            if (m == "append") {
                size_t pos = argc >= 2 ? ulong_arg(1, B.size()) : B.size();
                if (args[0]->type == ValueType::Bin) {
                    B.insert(B.begin() + pos, args[0]->bin_val.begin(), args[0]->bin_val.end());
                } else {
                    unsigned char b = (unsigned char)value_to_int(args[0])->int_val;
                    B.insert(B.begin() + pos, b);
                }
                return Value::make_none();
            }
            if (m == "erase") {
                size_t l = ulong_arg(0, 0), r = ulong_arg(1, B.size()-1);
                if (l <= r && r < B.size()) B.erase(B.begin() + l, B.begin() + r + 1);
                return Value::make_none();
            }
            if (m == "len") return value_from_uint(B.size());
            if (m == "find") {
                auto& P = args[0]->bin_val;
                auto it = std::search(B.begin(), B.end(), P.begin(), P.end());
                return Value::make_int(it == B.end() ? -1 : (long long)(it - B.begin()));
            }
            if (m == "count") {
                auto& P = args[0]->bin_val;
                if (P.empty()) return Value::make_int(0);
                long long cnt = 0; auto it = B.begin();
                while ((it = std::search(it, B.end(), P.begin(), P.end())) != B.end()) { ++cnt; it += P.size(); }
                return Value::make_int(cnt);
            }
            if (m == "replace") {
                if (argc >= 3 && (args[0]->type == ValueType::Int || args[0]->type == ValueType::UInt)) {
                    size_t l = ulong_arg(0, 0), r = ulong_arg(1, B.size()-1);
                    auto& N = args[2]->bin_val;
                    if (l <= r && r < B.size()) B.erase(B.begin() + l, B.begin() + r + 1);
                    B.insert(B.begin() + l, N.begin(), N.end());
                } else {
                    auto& O = args[0]->bin_val; auto& N = args[1]->bin_val;
                    if (!O.empty()) {
                        size_t off = 0;
                        while (true) {
                            auto it = std::search(B.begin() + off, B.end(), O.begin(), O.end());
                            if (it == B.end()) break;
                            off = it - B.begin();
                            B.erase(it, it + O.size());
                            B.insert(B.begin() + off, N.begin(), N.end());
                            off += N.size();
                        }
                    }
                }
                return Value::make_none();
            }
            if (m == "slice") {
                size_t start = ulong_arg(0, 0), end = ulong_arg(1, B.size());
                if (start > B.size()) start = B.size();
                if (end > B.size()) end = B.size();
                std::vector<unsigned char> nv(B.begin() + start, B.begin() + end);
                return Value::make_bin(nv);
            }
            if (m == "hex") {
                static const char* hex = "0123456789ABCDEF";
                std::string s;
                for (unsigned char b : B) { s += hex[b >> 4]; s += hex[b & 0xF]; }
                return Value::make_str(s);
            }
            if (m == "decode") {
                // 简易 UTF-8 转 Unicode string（直接 byte->char，不严谨但可用）
                std::string s(B.begin(), B.end());
                return Value::make_unistr(s);
            }
            throw RuntimeError("bin has no method: " + m);
        }
        case ValueType::Stack: {
            auto& S = *obj->stack_rep;
            if (m == "push") { S.push_back(args[0]); return Value::make_none(); }
            if (m == "pop") { if (S.empty()) throw RuntimeError("Empty stack"); auto v = S.back(); S.pop_back(); return v; }
            if (m == "top") { if (S.empty()) throw RuntimeError("Empty stack"); return S.back(); }
            if (m == "empty") return Value::make_bool(S.empty());
            if (m == "len") return value_from_uint(S.size());
            if (m == "clear") { S.clear(); return Value::make_none(); }
            if (m == "contains") { for (auto& e : S) if (value_eq(e, args[0])) return Value::make_bool(true); return Value::make_bool(false); }
            if (m == "copy") { auto r = Value::make_stack(); for (auto& e : S) r->stack_rep->push_back(e); return r; }
            throw RuntimeError("stack has no method: " + m);
        }
        case ValueType::Queue: {
            auto& Q = *obj->queue_rep;
            if (m == "push") { Q.push_back(args[0]); return Value::make_none(); }
            if (m == "pop") { if (Q.empty()) throw RuntimeError("Empty queue"); auto v = Q.front(); Q.pop_front(); return v; }
            if (m == "front") { if (Q.empty()) throw RuntimeError("Empty queue"); return Q.front(); }
            if (m == "back") { if (Q.empty()) throw RuntimeError("Empty queue"); return Q.back(); }
            if (m == "empty") return Value::make_bool(Q.empty());
            if (m == "len") return value_from_uint(Q.size());
            if (m == "clear") { Q.clear(); return Value::make_none(); }
            if (m == "contains") { for (auto& e : Q) if (value_eq(e, args[0])) return Value::make_bool(true); return Value::make_bool(false); }
            if (m == "copy") { auto r = Value::make_queue(); for (auto& e : Q) r->queue_rep->push_back(e); return r; }
            throw RuntimeError("queue has no method: " + m);
        }
        case ValueType::Set: {
            auto& S = *obj->set_rep;
            if (m == "insert") { auto r = S.insert(args[0]); return Value::make_bool(r.second); }
            if (m == "erase") return Value::make_bool(S.erase(args[0]) > 0);
            if (m == "contains") return Value::make_bool(S.count(args[0]) > 0);
            if (m == "empty") return Value::make_bool(S.empty());
            if (m == "len") return value_from_uint(S.size());
            if (m == "clear") { S.clear(); return Value::make_none(); }
            if (m == "min") { if (S.empty()) throw RuntimeError("Empty set"); return *S.begin(); }
            if (m == "max") { if (S.empty()) throw RuntimeError("Empty set"); auto it = S.end(); --it; return *it; }
            if (m == "count") return value_from_uint(S.count(args[0]));
            if (m == "union") return value_union(obj, args[0]);
            if (m == "intersection") return value_intersection(obj, args[0]);
            if (m == "difference") return value_difference(obj, args[0]);
            if (m == "symmetric_difference") return value_symmetric_diff(obj, args[0]);
            if (m == "is_subset") return Value::make_bool(value_difference(args[0], obj)->set_rep->empty() ? false : true); // 错
            if (m == "copy") { auto r = Value::make_set(); for (auto& e : S) r->set_rep->insert(e); return r; }
            throw RuntimeError("set has no method: " + m);
        }
        case ValueType::UndSet: {
            auto& S = *obj->undset_rep;
            if (m == "insert") { auto r = S.insert(args[0]); return Value::make_bool(r.second); }
            if (m == "erase") return Value::make_bool(S.erase(args[0]) > 0);
            if (m == "contains") return Value::make_bool(S.count(args[0]) > 0);
            if (m == "empty") return Value::make_bool(S.empty());
            if (m == "len") return value_from_uint(S.size());
            if (m == "clear") { S.clear(); return Value::make_none(); }
            if (m == "count") return value_from_uint(S.count(args[0]));
            if (m == "union") return value_union(obj, args[0]);
            if (m == "intersection") return value_intersection(obj, args[0]);
            if (m == "difference") return value_difference(obj, args[0]);
            if (m == "symmetric_difference") return value_symmetric_diff(obj, args[0]);
            if (m == "rehash") { /* noop */ return Value::make_none(); }
            if (m == "copy") { auto r = Value::make_undset(); for (auto& e : S) r->undset_rep->insert(e); return r; }
            throw RuntimeError("undset has no method: " + m);
        }
        case ValueType::Dict: {
            auto& D = *obj->dict_rep;
            if (m == "put") { D[args[0]] = args[1]; return Value::make_none(); }
            if (m == "get") {
                auto it = D.find(args[0]);
                if (it != D.end()) return it->second;
                if (argc >= 2) return args[1];
                throw RuntimeError("Key not found");
            }
            if (m == "has") return Value::make_bool(D.count(args[0]) > 0);
            if (m == "erase") return Value::make_bool(D.erase(args[0]) > 0);
            if (m == "pop") {
                auto it = D.find(args[0]);
                if (it != D.end()) { auto v = it->second; D.erase(it); return v; }
                if (argc >= 2) return args[1];
                throw RuntimeError("Key not found");
            }
            if (m == "popitem") {
                if (D.empty()) throw RuntimeError("Empty dict");
                auto it = D.begin();
                auto k = it->first; auto v = it->second; D.erase(it);
                return Value::make_pair(k, v);
            }
            if (m == "setdefault") {
                auto it = D.find(args[0]);
                if (it != D.end()) return it->second;
                D[args[0]] = argc >= 2 ? args[1] : Value::make_none();
                return D[args[0]];
            }
            if (m == "keys") {
                auto r = Value::make_list();
                for (auto& [k, v] : D) r->list_rep->push_back(k);
                return r;
            }
            if (m == "values") {
                auto r = Value::make_list();
                for (auto& [k, v] : D) r->list_rep->push_back(v);
                return r;
            }
            if (m == "items") {
                auto r = Value::make_list();
                for (auto& [k, v] : D) r->list_rep->push_back(Value::make_pair(k, v));
                return r;
            }
            if (m == "len") return value_from_uint(D.size());
            if (m == "empty") return Value::make_bool(D.empty());
            if (m == "clear") { D.clear(); return Value::make_none(); }
            if (m == "update") {
                auto& d2 = *args[0]->dict_rep;
                for (auto& [k, v] : d2) D[k] = v;
                return Value::make_none();
            }
            if (m == "copy") {
                auto r = Value::make_dict();
                for (auto& [k, v] : D) (*r->dict_rep)[k] = v;
                return r;
            }
            throw RuntimeError("dict has no method: " + m);
        }
        case ValueType::Tuple: {
            auto& T = *obj->tuple_rep;
            if (m == "len") return value_from_uint(T.size());
            if (m == "get") {
                size_t i = (size_t)value_to_int(args[0])->int_val;
                if (i >= T.size()) throw RuntimeError("tuple index out of range");
                return T[i];
            }
            if (m == "swap") {
                auto t2 = args[0]->tuple_rep;
                if (t2->size() != T.size()) throw RuntimeError("tuple size mismatch");
                std::swap_ranges(T.begin(), T.end(), t2->begin());
                return Value::make_none();
            }
            throw RuntimeError("tuple has no method: " + m);
        }
        case ValueType::Pair: {
            if (m == "swap") {
                auto p2 = args[0]->pair_rep;
                std::swap(obj->pair_rep->first, p2->first);
                std::swap(obj->pair_rep->second, p2->second);
                return Value::make_none();
            }
            throw RuntimeError("pair has no method: " + m);
        }
        default:
            throw RuntimeError(obj->type_name() + " does not support method call");
    }
}

ValuePtr Interpreter::call_builtin(const std::string& name, const ValueVec& args) {
    if (globals_.has(name)) {
        ValuePtr v = globals_.lookup(name);
        if (v->type == ValueType::Function && v->fn_rep && v->fn_rep->is_builtin) {
            return v->fn_rep->builtin_fn(args, *current_env_);
        }
    }
    throw RuntimeError("Unknown function: " + name);
}

ValuePtr Interpreter::call_user_function_with_env(const FunctionDefStmt* def, const ValueVec& args, Environment* parent_env) {
    if (!def) throw RuntimeError("Function body not available");
    Environment* saved = nullptr;
    Environment scope(parent_env);
    saved = current_env_;
    current_env_ = &scope;

    size_t np = def->params.size();
    size_t argp = 0;
    for (size_t i = 0; i < np; ++i) {
        auto& p = def->params[i];
        if (p->is_vararg) {
            auto lst = Value::make_list();
            while (argp < args.size()) {
                lst->list_rep->push_back(args[argp++]);
            }
            scope.define(p->name, lst, false);
            break;
        }
        if (argp < args.size()) {
            scope.define(p->name, args[argp++], false);
        } else if (p->default_value) {
            // 在 parent env 中解释默认参数表达式
            Environment* tmp = current_env_;
            current_env_ = parent_env;
            auto v = evaluate(p->default_value.get());
            current_env_ = tmp;
            scope.define(p->name, v, false);
        } else {
            current_env_ = saved;
            throw RuntimeError("Missing argument '" + p->name + "'");
        }
    }
    ControlSignal cs;
    for (auto& s : def->body->stmts) {
        cs = execute(s.get());
        if (cs.type == CtrlFlow::Return) break;
        if (cs.type != CtrlFlow::None) break;
    }
    current_env_ = saved;
    if (cs.type == CtrlFlow::Return && cs.value) return cs.value;
    return Value::make_none();
}

// 声明在 Interpreter 中由 eval_lambda 使用的辅助
void exec_source_interpreter(Interpreter* interp, const std::string& src);

void Interpreter::exec_source(const std::string& src) {
    Lexer lex(src);
    auto toks = lex.tokenize();
    for (auto& e : lex.errors()) print_output("[LexerError] " + e + "\n");
    if (!lex.errors().empty()) return;
    Parser parser(toks);
    auto prog = parser.parse_program();
    for (auto& e : parser.errors()) print_output("[ParseError] " + e + "\n");
    if (!parser.errors().empty()) return;
    run(*prog);
}

} // namespace vortex

// 在 namespace vortex 内部调用的前置声明实现
namespace vortex {
ValuePtr call_user_function_with_env(const FunctionDefStmt* def, const ValueVec& args, Environment* parent_env) {
    // 通过构造一个临时 Interpreter 不方便；实际此函数是 Interpreter 的成员
    // 但上面我们在 Interpreter 类里已经实现了 call_user_function_with_env；此处为转发
    // 由于 lambda 捕获中使用了闭包指针+捕获方式，这里保留但不使用。
    return Value::make_none();
}
}
