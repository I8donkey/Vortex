// ============================================================
// sql_module.cpp — SQLite 模块
// ============================================================
#include "sql_module.h"
#include "value.h"
#include "interpreter.h"
#include <sqlite3.h>
#include <string>
#include <vector>

namespace vortex {

namespace zsql {
struct Conn { sqlite3* db = nullptr; std::string path; };
} // namespace zsql

static long long sql_open_impl(const std::string& path) {
    auto c = new zsql::Conn();
    int rc = sqlite3_open(path.c_str(), &c->db);
    if (rc != SQLITE_OK) {
        std::string msg = sqlite3_errmsg(c->db);
        sqlite3_close(c->db);
        delete c;
        throw RuntimeError("sql.open: " + msg);
    }
    c->path = path;
    return (long long)c;
}
static zsql::Conn* sql_conn_from(long long h) {
    auto c = reinterpret_cast<zsql::Conn*>(h);
    if (!c || !c->db) throw RuntimeError("sql: invalid connection handle");
    return c;
}
static long long val_to_handle(const ValuePtr& v) {
    return std::stoll(v->to_string());
}

void register_sql_module(std::unordered_map<std::string, ValuePtr>& std_modules) {
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
        u[n] = mk_fn("sql", n, a0, a1, std::move(f));
    };

    add("open", 1, 1, [&](const ValueVec& a) {
        return Value::make_int(sql_open_impl(a[0]->to_string()));
    });

    add("close", 1, 1, [&](const ValueVec& a) {
        auto c = sql_conn_from(val_to_handle(a[0]));
        sqlite3_close(c->db);
        c->db = nullptr;
        delete c;
        return Value::make_none();
    });

    add("execute", 2, 2, [&](const ValueVec& a) {
        auto c = sql_conn_from(val_to_handle(a[0]));
        char* err = nullptr;
        int rc = sqlite3_exec(c->db, a[1]->to_string().c_str(), nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            std::string m = err ? err : sqlite3_errmsg(c->db);
            if (err) sqlite3_free(err);
            throw RuntimeError("sql.execute: " + m);
        }
        return Value::make_int((long long)sqlite3_changes(c->db));
    });

    add("query_one", 2, 2, [&](const ValueVec& a) {
        auto c = sql_conn_from(val_to_handle(a[0]));
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(c->db, a[1]->to_string().c_str(), -1, &st, nullptr) != SQLITE_OK)
            throw RuntimeError(std::string("sql.query_one: ") + sqlite3_errmsg(c->db));
        std::string val;
        if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_count(st) > 0)
            val = (const char*)sqlite3_column_text(st, 0);
        sqlite3_finalize(st);
        return Value::make_str(val);
    });

    add("query", 2, 2, [&](const ValueVec& a) {
        auto c = sql_conn_from(val_to_handle(a[0]));
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(c->db, a[1]->to_string().c_str(), -1, &st, nullptr) != SQLITE_OK)
            throw RuntimeError(std::string("sql.query: ") + sqlite3_errmsg(c->db));
        auto rows = Value::make_list();
        while (sqlite3_step(st) == SQLITE_ROW) {
            auto row = Value::make_list();
            int n = sqlite3_column_count(st);
            for (int i = 0; i < n; ++i) {
                const unsigned char* txt = sqlite3_column_text(st, i);
                row->list_rep->push_back(Value::make_str(txt ? (const char*)txt : ""));
            }
            rows->list_rep->push_back(row);
        }
        sqlite3_finalize(st);
        return rows;
    });

    add("table_exists", 2, 2, [&](const ValueVec& a) {
        auto c = sql_conn_from(val_to_handle(a[0]));
        sqlite3_stmt* st = nullptr;
        std::string q = "SELECT name FROM sqlite_master WHERE type='table' AND name=?1";
        if (sqlite3_prepare_v2(c->db, q.c_str(), -1, &st, nullptr) != SQLITE_OK)
            throw RuntimeError("sql.table_exists: prepare failed");
        sqlite3_bind_text(st, 1, a[1]->to_string().c_str(), -1, SQLITE_TRANSIENT);
        int found = (sqlite3_step(st) == SQLITE_ROW);
        sqlite3_finalize(st);
        return Value::make_bool(found != 0);
    });

    std_modules["sql"] = mod;
}

} // namespace vortex