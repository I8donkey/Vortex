// ============================================================
// ModuleLoader.cpp — 把主程序的用户模块(.vt/.vtp)解析、改名并合并
// ============================================================
#include "codegen/ModuleLoader.h"
#include "lexer.h"
#include "parser.h"
#include "pack.h"

#include <set>

namespace vortex {
namespace cg {

// 编译器已转发/已知的内建、扩展模块名：import 它们不当作 "用户源码模块"。
static const std::set<std::string>& builtin_modules() {
    static const std::set<std::string> s = {
        "math","time","random","thread","log","file","zip","xml","html","sql",
        "os","regex","json","base64","datetime","csv","hash","str","text","net",
        "sys","gui","game2d","render3d"
    };
    return s;
}

// ---- AST 改名：把模块顶层符号引用 从 name 改为 mod.name ----

static void mangle_expr(Expr* e, const std::set<std::string>& top, const std::string& mod);
static void mangle_stmt(Stmt* s, const std::set<std::string>& top, const std::string& mod, bool top_level);

static void mangle_expr_ptr(ExprPtr& e, const std::set<std::string>& top, const std::string& mod) {
    if (e) mangle_expr(e.get(), top, mod);
}

static void mangle_expr(Expr* e, const std::set<std::string>& top, const std::string& mod) {
    if (!e) return;
    switch (e->kind) {
        case ExprKind::Identifier: {
            auto* id = static_cast<IdentifierExpr*>(e);
            if (top.count(id->name)) id->name = mod + "." + id->name;
            break;
        }
        case ExprKind::UnaryOp: {
            auto* u = static_cast<UnaryOpExpr*>(e); mangle_expr_ptr(u->operand, top, mod); break;
        }
        case ExprKind::BinaryOp: {
            auto* b = static_cast<BinaryOpExpr*>(e);
            mangle_expr_ptr(b->left, top, mod); mangle_expr_ptr(b->right, top, mod); break;
        }
        case ExprKind::TernaryOp: {
            auto* t = static_cast<TernaryOpExpr*>(e);
            mangle_expr_ptr(t->cond, top, mod); mangle_expr_ptr(t->then_e, top, mod);
            mangle_expr_ptr(t->else_e, top, mod); break;
        }
        case ExprKind::AssignOp: {
            auto* a = static_cast<AssignOpExpr*>(e);
            mangle_expr_ptr(a->target, top, mod); mangle_expr_ptr(a->value, top, mod); break;
        }
        case ExprKind::Call: {
            auto* c = static_cast<CallExpr*>(e);
            mangle_expr_ptr(c->callee, top, mod);
            for (auto& a : c->args) mangle_expr_ptr(a, top, mod);
            for (auto& kv : c->kwargs) mangle_expr_ptr(kv.second, top, mod);
            break;
        }
        case ExprKind::MemberAccess: {
            auto* m = static_cast<MemberAccessExpr*>(e);
            mangle_expr_ptr(m->object, top, mod);
            break;
        }
        case ExprKind::Subscript: {
            auto* s = static_cast<SubscriptExpr*>(e);
            mangle_expr_ptr(s->object, top, mod); mangle_expr_ptr(s->index, top, mod); break;
        }
        case ExprKind::ListInit: {
            auto* l = static_cast<ListInitExpr*>(e);
            for (auto& el : l->elements) mangle_expr_ptr(el, top, mod);
            break;
        }
        case ExprKind::DictInit: {
            auto* d = static_cast<DictInitExpr*>(e);
            for (auto& kv : d->pairs) { mangle_expr_ptr(kv.key, top, mod); mangle_expr_ptr(kv.value, top, mod); }
            break;
        }
        case ExprKind::Lambda: {
            auto* lam = static_cast<LambdaExpr*>(e);
            for (auto& p : lam->params)
                if (p->default_value) mangle_expr_ptr(p->default_value, top, mod);
            for (auto& st : lam->body) mangle_stmt(st.get(), top, mod, false);
            break;
        }
        case ExprKind::Cast: {
            auto* c = static_cast<CastExpr*>(e); mangle_expr_ptr(c->value, top, mod); break;
        }
        case ExprKind::Literal: break;
        default: break; // AddressOf/Dereference 由 UnaryOp(op=@/~) 表示
    }
}

static void mangle_stmt(Stmt* s, const std::set<std::string>& top, const std::string& mod, bool top_level) {
    if (!s) return;
    switch (s->kind) {
        case StmtKind::VarDecl: {
            auto* v = static_cast<VarDeclStmt*>(s);
            for (auto& pr : v->names) {
                if (top_level && top.count(pr.first)) pr.first = mod + "." + pr.first;
                mangle_expr_ptr(pr.second, top, mod);
            }
            break;
        }
        case StmtKind::ConstDecl: {
            auto* c = static_cast<ConstDeclStmt*>(s);
            if (top_level && top.count(c->name)) c->name = mod + "." + c->name;
            mangle_expr_ptr(c->value, top, mod);
            break;
        }
        case StmtKind::Assign: {
            auto* a = static_cast<AssignStmt*>(s); mangle_expr_ptr(a->assign, top, mod); break;
        }
        case StmtKind::ExprStmt: {
            auto* es = static_cast<ExprStmt*>(s); mangle_expr_ptr(es->expr, top, mod); break;
        }
        case StmtKind::Block: {
            auto* bl = static_cast<BlockStmt*>(s);
            for (auto& st : bl->stmts) mangle_stmt(st.get(), top, mod, false);
            break;
        }
        case StmtKind::If: {
            auto* i = static_cast<IfStmt*>(s);
            mangle_expr_ptr(i->cond, top, mod);
            for (auto& st : i->then_body->stmts) mangle_stmt(st.get(), top, mod, false);
            for (auto& el : i->elif_list) {
                mangle_expr_ptr(el.cond, top, mod);
                for (auto& st : el.body->stmts) mangle_stmt(st.get(), top, mod, false);
            }
            if (i->else_body) for (auto& st : i->else_body->stmts) mangle_stmt(st.get(), top, mod, false);
            break;
        }
        case StmtKind::ForIn: {
            auto* f = static_cast<ForInStmt*>(s);
            mangle_expr_ptr(f->container, top, mod);
            for (auto& st : f->body->stmts) mangle_stmt(st.get(), top, mod, false);
            break;
        }
        case StmtKind::While: {
            auto* w = static_cast<WhileStmt*>(s);
            mangle_expr_ptr(w->cond, top, mod);
            for (auto& st : w->body->stmts) mangle_stmt(st.get(), top, mod, false);
            break;
        }
        case StmtKind::Return: {
            auto* r = static_cast<ReturnStmt*>(s); mangle_expr_ptr(r->value, top, mod); break;
        }
        case StmtKind::Del: {
            auto* d = static_cast<DelStmt*>(s);
            for (auto& t : d->targets) mangle_expr_ptr(t, top, mod);
            break;
        }
        case StmtKind::FunctionDef: {
            auto* f = static_cast<FunctionDefStmt*>(s);
            if (top_level && top.count(f->name)) f->name = mod + "." + f->name;
            for (auto& p : f->params)
                if (p->default_value) mangle_expr_ptr(p->default_value, top, mod);
            for (auto& st : f->body->stmts) mangle_stmt(st.get(), top, mod, false);
            break;
        }
        case StmtKind::TryCatch: {
            auto* t = static_cast<TryCatchStmt*>(s);
            for (auto& st : t->try_body->stmts) mangle_stmt(st.get(), top, mod, false);
            if (t->catch_body) for (auto& st : t->catch_body->stmts) mangle_stmt(st.get(), top, mod, false);
            if (t->finally_body) for (auto& st : t->finally_body->stmts) mangle_stmt(st.get(), top, mod, false);
            break;
        }
        case StmtKind::Import: break; // 编译期 import 不产码
        case StmtKind::Break: case StmtKind::Continue: break;
        default: break;
    }
}

// 收集模块顶层符号名（第一遍）
static void collect_top(Stmt* s, std::set<std::string>& top) {
    if (!s) return;
    switch (s->kind) {
        case StmtKind::FunctionDef: top.insert(static_cast<FunctionDefStmt*>(s)->name); break;
        case StmtKind::ConstDecl: top.insert(static_cast<ConstDeclStmt*>(s)->name); break;
        case StmtKind::VarDecl:
            for (auto& pr : static_cast<VarDeclStmt*>(s)->names) top.insert(pr.first);
            break;
        default: break;
    }
}

static void qualify_module(Program& prog, const std::string& mod, std::string& err) {
    std::set<std::string> top;
    for (auto& st : prog.stmts) collect_top(st.get(), top);
    for (auto& st : prog.stmts) mangle_stmt(st.get(), top, mod, true);
}

// 深度遍历主程序 imports；用户模块解析+改名后合入 combined
static bool load_and_merge(const std::string& module, const std::string& base_dir,
                           std::set<std::string>& visiting, Program& combined, std::string& err) {
    if (builtin_modules().count(module)) return true; // 内置/扩展模块：交由编译端成员转发
    if (!visiting.insert(module).second) { err = "circular import of module '" + module + "'"; return false; }

    std::string src;
    if (!pack::find_module_source(module, base_dir, src, err)) { visiting.erase(module); return false; }

    Lexer lex(src);
    auto toks = lex.tokenize();
    if (!lex.errors().empty()) { err = "module '" + module + "' lex error: " + lex.errors()[0]; visiting.erase(module); return false; }
    Parser parser(toks);
    std::unique_ptr<Program> mp = parser.parse_program();
    if (!mp) { err = "module '" + module + "' parse failed"; visiting.erase(module); return false; }
    if (!parser.errors().empty()) { err = "module '" + module + "' parse error: " + parser.errors()[0]; visiting.erase(module); return false; }

    // 先合并该模块自身的用户子模块（依赖优先）
    for (auto& st : mp->stmts) {
        if (st->kind == StmtKind::Import) {
            auto* im = static_cast<ImportStmt*>(st.get());
            if (!load_and_merge(im->module, base_dir, visiting, combined, err)) { visiting.erase(module); return false; }
        }
    }
    qualify_module(*mp, module, err);
    // 去掉模块内的 import 语句（已处理依赖；gen_stmt 会忽略，但合入后自身的 `import x` 保留即可）
    for (auto& st : mp->stmts) combined.stmts.push_back(std::move(st));

    visiting.erase(module);
    return true;
}

bool collect_modules(const std::string& main_src, const std::string& main_path,
                     Program& combined, std::string& err) {
    // 主程序解析
    Lexer lex(main_src);
    auto toks = lex.tokenize();
    if (!lex.errors().empty()) { err = lex.errors()[0]; return false; }
    Parser parser(toks);
    std::unique_ptr<Program> main = parser.parse_program();
    if (!main) { err = "parse failed"; return false; }
    if (!parser.errors().empty()) { err = parser.errors()[0]; return false; }

    // 模块搜索根：主文件所在目录 / cwd
    std::string base = "";
    {
        size_t slash = main_path.find_last_of("/\\");
        if (slash != std::string::npos) base = main_path.substr(0, slash);
    }

    std::set<std::string> visiting;
    Program modules; // 依赖先合（DFS 顺序）
    for (auto& st : main->stmts) {
        if (st->kind == StmtKind::Import) {
            auto* im = static_cast<ImportStmt*>(st.get());
            if (!load_and_merge(im->module, base, visiting, modules, err)) return false;
        }
    }
    for (auto& st : modules.stmts) combined.stmts.push_back(std::move(st));
    for (auto& st : main->stmts) combined.stmts.push_back(std::move(st));
    return true;
}

} // namespace cg
} // namespace vortex