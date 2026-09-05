// ============================================================
// datetime_module.cpp — 日期时间模块
// ym/ymd/md/today/now/to_iso/from_iso/add_days/days_between，
// 全部返回标量，GMT 基准（与 gmtime 一致避免时区差异）。
// ============================================================
#include "datetime_module.h"
#include "value.h"
#include "interpreter.h"
#include <ctime>
#include <string>

namespace vortex {

static std::tm dt_epoch_struct(const std::tm* t) { return t ? *t : std::tm(); }

static long long days_from_civil(int y, unsigned m, unsigned d) {
    // Howard Hinnant 算法：proleptic gregorian
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);                // [0,399]
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;    // [0,146096]
    return era * 146097 + (long long)doe - 719468;
}

static void civil_from_days(long long z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);             // [0,146096]
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long yy = (long long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y = (int)(yy + (m <= 2));
}

static long long dt_ymd_to_days(int y, int m, int d) { return days_from_civil(y, (unsigned)m, (unsigned)d); }

static bool dt_parse_ymd(const std::string& iso, int& y, int& m, int& d) {
    // 接受 YYYY-MM-DD 或 YYYY-MM-DDTHH:MM:SS 或带毫秒
    y = m = d = 0;
    size_t i = 0;
    auto num = [&](int& out) {
        out = 0; int cnt = 0;
        while (i < iso.size() && iso[i] >= '0' && iso[i] <= '9') { out = out * 10 + (iso[i]-'0'); ++i; ++cnt; }
        return cnt > 0;
    };
    if (!num(y)) return false;
    if (i < iso.size() && iso[i] == '-') ++i; else return false;
    if (!num(m)) return false;
    if (i < iso.size() && iso[i] == '-') ++i; else return false;
    if (!num(d)) return false;
    if (m < 1 || m > 12 || d < 1 || d > 31) return false;
    return true;
}

static std::string dt_days_to_iso(long long days) {
    int y; unsigned mo, d;
    civil_from_days(days, y, mo, d);
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02u-%02u", y, mo, d);
    return buf;
}

void register_datetime_module(std::unordered_map<std::string, ValuePtr>& std_modules) {
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
        u[n] = mk_fn("datetime", n, a0, a1, std::move(f));
    };

    add("ymd", 3, 3, [&](const ValueVec& a) {
        int y = (int)std::stoll(a[0]->to_string());
        int mo = (int)std::stoll(a[1]->to_string());
        int d = (int)std::stoll(a[2]->to_string());
        if (mo < 1 || mo > 12) throw RuntimeError("datetime.ymd: bad month");
        return Value::make_str(dt_days_to_iso(dt_ymd_to_days(y, mo, d)));
    });
    add("to_iso", 1, 1, [&](const ValueVec& a) {
        long long days = std::stoll(a[0]->to_string()) / 86400;
        return Value::make_str(dt_days_to_iso(days));
    });
    add("from_iso", 1, 1, [&](const ValueVec& a) {
        int y, m, d;
        if (!dt_parse_ymd(a[0]->to_string(), y, m, d))
            throw RuntimeError("datetime.from_iso: bad ISO date");
        return Value::make_int(dt_ymd_to_days(y, m, d) * 86400);
    });
    add("today", 0, 0, [&](const ValueVec&) {
        std::time_t t = std::time(nullptr);
        std::tm g = *std::gmtime(&t);
        return Value::make_str(dt_days_to_iso(days_from_civil(g.tm_year + 1900, (unsigned)(g.tm_mon + 1), (unsigned)g.tm_mday)));
    });
    add("add_days", 2, 2, [&](const ValueVec& a) {
        int y, m, d;
        if (!dt_parse_ymd(a[0]->to_string(), y, m, d))
            throw RuntimeError("datetime.add_days: bad ISO date");
        long long n = std::stoll(a[1]->to_string());
        return Value::make_str(dt_days_to_iso(dt_ymd_to_days(y, m, d) + n));
    });
    add("days_between", 2, 2, [&](const ValueVec& a) {
        int y1, m1, d1, y2, m2, d2;
        if (!dt_parse_ymd(a[0]->to_string(), y1, m1, d1) || !dt_parse_ymd(a[1]->to_string(), y2, m2, d2))
            throw RuntimeError("datetime.days_between: bad ISO date");
        return Value::make_int(dt_ymd_to_days(y2, m2, d2) - dt_ymd_to_days(y1, m1, d1));
    });

    std_modules["datetime"] = mod;
}

} // namespace vortex