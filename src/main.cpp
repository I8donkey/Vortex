#include "interpreter.h"
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <cstring>

using namespace vortex;

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "Error: cannot open file '" << path << "'\n";
        return {};
    }
    std::ostringstream oss; oss << f.rdbuf();
    return oss.str();
}

static void print_usage(const char* prog) {
    std::cout << "Vortex Compiler & Interpreter\n";
    std::cout << "Usage: \n";
    std::cout << "  " << prog << " run <file.vt>        Interpret and run source file\n";
    std::cout << "  " << prog << " compile <file.vt>    Lex + Parse, print AST dump\n";
    std::cout << "  " << prog << " <file.vt>             Same as 'run'\n";
    std::cout << "  " << prog << " repl                 Interactive REPL\n";
    std::cout << "  " << prog << " -h | --help          Show this message\n";
}

// AST dump helper
static void dump_stmt(const Stmt* s, int indent = 0);
static void dump_expr(const Expr* e, int indent = 0);
static std::string ind(int n) { return std::string(n * 2, ' '); }

static void dump_expr(const Expr* e, int i) {
    if (!e) { std::cout << ind(i) << "(null)\n"; return; }
    switch (e->kind) {
        case ExprKind::Literal: {
            auto l = static_cast<const LiteralExpr*>(e);
            std::cout << ind(i) << "Literal ";
            switch (l->lit_kind) {
                case LiteralExpr::LitKind::Int: std::cout << l->int_val; break;
                case LiteralExpr::LitKind::Float: std::cout << l->float_val; break;
                case LiteralExpr::LitKind::Bool: std::cout << (l->bool_val ? "true" : "false"); break;
                case LiteralExpr::LitKind::String: std::cout << "\"" << l->str_val << "\""; break;
                case LiteralExpr::LitKind::Char: std::cout << "'" << (char)l->char_val << "'"; break;
                case LiteralExpr::LitKind::None: std::cout << "None"; break;
                case LiteralExpr::LitKind::Unichar: std::cout << "U+" << std::hex << l->char_val; break;
            }
            std::cout << "\n";
            break;
        }
        case ExprKind::Identifier:
            std::cout << ind(i) << "Id " << static_cast<const IdentifierExpr*>(e)->name << "\n";
            break;
        case ExprKind::UnaryOp: {
            auto u = static_cast<const UnaryOpExpr*>(e);
            std::cout << ind(i) << "Unary " << u->op << "\n";
            dump_expr(u->operand.get(), i+1);
            break;
        }
        case ExprKind::BinaryOp: {
            auto b = static_cast<const BinaryOpExpr*>(e);
            std::cout << ind(i) << "Binary " << b->op << "\n";
            dump_expr(b->left.get(), i+1);
            dump_expr(b->right.get(), i+1);
            break;
        }
        case ExprKind::TernaryOp: {
            auto t = static_cast<const TernaryOpExpr*>(e);
            std::cout << ind(i) << "Ternary\n";
            dump_expr(t->cond.get(), i+1); dump_expr(t->then_e.get(), i+1); dump_expr(t->else_e.get(), i+1);
            break;
        }
        case ExprKind::AssignOp: {
            auto a = static_cast<const AssignOpExpr*>(e);
            std::cout << ind(i) << "Assign " << a->op << "\n";
            dump_expr(a->target.get(), i+1); dump_expr(a->value.get(), i+1);
            break;
        }
        case ExprKind::Call: {
            auto c = static_cast<const CallExpr*>(e);
            std::cout << ind(i) << "Call\n" << ind(i+1) << "callee:\n";
            dump_expr(c->callee.get(), i+2);
            std::cout << ind(i+1) << "args:\n";
            for (auto& a : c->args) dump_expr(a.get(), i+2);
            break;
        }
        case ExprKind::MemberAccess: {
            auto m = static_cast<const MemberAccessExpr*>(e);
            std::cout << ind(i) << "Member ." << m->member << "\n";
            dump_expr(m->object.get(), i+1);
            break;
        }
        case ExprKind::Subscript: {
            auto s = static_cast<const SubscriptExpr*>(e);
            std::cout << ind(i) << "Subscript\n";
            dump_expr(s->object.get(), i+1); dump_expr(s->index.get(), i+1);
            break;
        }
        case ExprKind::ListInit: {
            auto l = static_cast<const ListInitExpr*>(e);
            std::cout << ind(i) << "ListInit\n";
            for (auto& e : l->elements) dump_expr(e.get(), i+1);
            break;
        }
        case ExprKind::DictInit: {
            auto d = static_cast<const DictInitExpr*>(e);
            std::cout << ind(i) << "DictInit\n";
            for (auto& p : d->pairs) {
                std::cout << ind(i+1) << "key:\n"; dump_expr(p.key.get(), i+2);
                std::cout << ind(i+1) << "val:\n"; dump_expr(p.value.get(), i+2);
            }
            break;
        }
        case ExprKind::Lambda:
            std::cout << ind(i) << "Lambda (" << static_cast<const LambdaExpr*>(e)->params.size() << " params, "
                      << static_cast<const LambdaExpr*>(e)->body.size() << " stmts)\n";
            break;
        case ExprKind::Cast: {
            auto c = static_cast<const CastExpr*>(e);
            std::cout << ind(i) << "Cast -> " << c->target_type << "\n";
            dump_expr(c->value.get(), i+1);
            break;
        }
        default:
            std::cout << ind(i) << "Expr(kind=" << (int)e->kind << ")\n";
    }
}

static void dump_stmt(const Stmt* s, int i) {
    if (!s) { std::cout << ind(i) << "(null stmt)\n"; return; }
    switch (s->kind) {
        case StmtKind::VarDecl: {
            auto v = static_cast<const VarDeclStmt*>(s);
            std::cout << ind(i) << "VarDecl" << (v->type ? (" " + v->type->base_name) : "") << "\n";
            for (auto& [n, init] : v->names) {
                std::cout << ind(i+1) << "name: " << n << "\n";
                if (init) { std::cout << ind(i+1) << "init:\n"; dump_expr(init.get(), i+2); }
            }
            break;
        }
        case StmtKind::ConstDecl: {
            auto c = static_cast<const ConstDeclStmt*>(s);
            std::cout << ind(i) << "Const " << (c->type ? c->type->base_name : "") << " " << c->name << "\n";
            dump_expr(c->value.get(), i+1);
            break;
        }
        case StmtKind::Assign:
            std::cout << ind(i) << "AssignStmt\n";
            dump_expr(static_cast<const AssignStmt*>(s)->assign.get(), i+1);
            break;
        case StmtKind::ExprStmt:
            std::cout << ind(i) << "ExprStmt\n";
            dump_expr(static_cast<const ExprStmt*>(s)->expr.get(), i+1);
            break;
        case StmtKind::Block: {
            std::cout << ind(i) << "Block (" << static_cast<const BlockStmt*>(s)->stmts.size() << " stmts)\n";
            for (auto& st : static_cast<const BlockStmt*>(s)->stmts) dump_stmt(st.get(), i+1);
            break;
        }
        case StmtKind::If: {
            auto x = static_cast<const IfStmt*>(s);
            std::cout << ind(i) << "If\n" << ind(i+1) << "cond:\n";
            dump_expr(x->cond.get(), i+2);
            std::cout << ind(i+1) << "then:\n";
            for (auto& st : x->then_body->stmts) dump_stmt(st.get(), i+2);
            for (auto& el : x->elif_list) {
                std::cout << ind(i+1) << "elif:\n" << ind(i+2) << "cond:\n";
                dump_expr(el.cond.get(), i+3);
                for (auto& st : el.body->stmts) dump_stmt(st.get(), i+3);
            }
            if (x->else_body) {
                std::cout << ind(i+1) << "else:\n";
                for (auto& st : x->else_body->stmts) dump_stmt(st.get(), i+2);
            }
            break;
        }
        case StmtKind::ForIn: {
            auto f = static_cast<const ForInStmt*>(s);
            std::cout << ind(i) << "For " << f->var << " in\n";
            dump_expr(f->container.get(), i+1);
            std::cout << ind(i+1) << "body:\n";
            for (auto& st : f->body->stmts) dump_stmt(st.get(), i+2);
            break;
        }
        case StmtKind::While: {
            auto w = static_cast<const WhileStmt*>(s);
            std::cout << ind(i) << "While cond:\n";
            dump_expr(w->cond.get(), i+1);
            std::cout << ind(i+1) << "body:\n";
            for (auto& st : w->body->stmts) dump_stmt(st.get(), i+2);
            break;
        }
        case StmtKind::Break: std::cout << ind(i) << "Break\n"; break;
        case StmtKind::Continue: std::cout << ind(i) << "Continue\n"; break;
        case StmtKind::Return: {
            auto r = static_cast<const ReturnStmt*>(s);
            std::cout << ind(i) << "Return\n";
            if (r->value) dump_expr(r->value.get(), i+1);
            break;
        }
        case StmtKind::Del: {
            auto d = static_cast<const DelStmt*>(s);
            std::cout << ind(i) << "Del " << (d->delete_all ? "*" : "");
            for (auto& t : d->targets) {
                if (t->kind == ExprKind::Identifier) std::cout << " " << static_cast<const IdentifierExpr&>(*t).name;
                else std::cout << " ~expr";
            }
            std::cout << "\n";
            break;
        }
        case StmtKind::FunctionDef: {
            auto f = static_cast<const FunctionDefStmt*>(s);
            std::cout << ind(i) << "FunctionDef " << f->name << "(";
            for (size_t k = 0; k < f->params.size(); ++k) {
                if (k) std::cout << ", ";
                std::cout << f->params[k]->name;
                if (f->params[k]->is_vararg) std::cout << "*";
            }
            std::cout << ")\n";
            for (auto& st : f->body->stmts) dump_stmt(st.get(), i+1);
            break;
        }
        case StmtKind::Import: {
            auto m = static_cast<const ImportStmt*>(s);
            std::cout << ind(i) << "Import " << (m->is_from ? "from " : "") << m->module;
            if (!m->items.empty()) { std::cout << " items:"; for (auto& it : m->items) std::cout << " " << it; }
            if (!m->alias.empty()) std::cout << " as " << m->alias;
            std::cout << "\n";
            break;
        }
        case StmtKind::TryCatch: {
            auto t = static_cast<const TryCatchStmt*>(s);
            std::cout << ind(i) << "Try\n";
            for (auto& st : t->try_body->stmts) dump_stmt(st.get(), i+1);
            if (t->catch_body) {
                std::cout << ind(i) << "Catch " << t->exception_type << " " << t->exception_var << "\n";
                for (auto& st : t->catch_body->stmts) dump_stmt(st.get(), i+1);
            }
            if (t->finally_body) {
                std::cout << ind(i) << "Finally\n";
                for (auto& st : t->finally_body->stmts) dump_stmt(st.get(), i+1);
            }
            break;
        }
    }
}

static bool run_source(const std::string& src, const std::string& name = "<stdin>") {
    Lexer lex(src);
    auto toks = lex.tokenize();
    if (!lex.errors().empty()) {
        for (auto& e : lex.errors()) std::cerr << name << ": [Lexer] " << e << "\n";
        return false;
    }
    Parser parser(toks);
    std::unique_ptr<Program> prog;
    try {
        prog = parser.parse_program();
    } catch (const std::runtime_error& e) {
        std::cerr << name << ": [Parse] " << e.what() << "\n";
        return false;
    }
    if (!parser.errors().empty()) {
        for (auto& e : parser.errors()) std::cerr << name << ": [Parse] " << e << "\n";
        return false;
    }
    Interpreter interp;
    interp.run(*prog);
    return true;
}

static bool compile_dump(const std::string& src, const std::string& name) {
    Lexer lex(src);
    auto toks = lex.tokenize();
    if (!lex.errors().empty()) {
        for (auto& e : lex.errors()) std::cerr << name << ": [Lexer] " << e << "\n";
        return false;
    }
    Parser parser(toks);
    std::unique_ptr<Program> prog;
    try {
        prog = parser.parse_program();
    } catch (const std::runtime_error& e) {
        std::cerr << name << ": [Parse] " << e.what() << "\n";
        return false;
    }
    if (!parser.errors().empty()) {
        for (auto& e : parser.errors()) std::cerr << name << ": [Parse] " << e << "\n";
        return false;
    }
    // 输出 tokens 摘要 + AST dump
    std::cout << "=== Tokens (" << toks.size() << ") ===\n";
    size_t n = std::min<size_t>(toks.size(), 100);
    for (size_t i = 0; i < n; ++i) {
        auto& t = toks[i];
        std::cout << "  L" << t.line << ":" << t.col << "  " << token_type_name_str(t.type);
        if (!t.text.empty() && t.type != TokenType::EndOfFile) std::cout << "  '" << t.text << "'";
        std::cout << "\n";
    }
    if (toks.size() > 100) std::cout << "  ... (" << (toks.size() - 100) << " more)\n";
    std::cout << "\n=== AST ===\n";
    for (auto& s : prog->stmts) dump_stmt(s.get(), 0);
    return true;
}

static void run_repl() {
    std::cout << "Vortex REPL. Type 'exit' to quit, 'dump <stmt>' to dump AST.\n";
    Interpreter interp;
    std::string line;
    int count = 0;
    while (true) {
        std::cout << "vortex[" << count++ << "]> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (line == "exit" || line == "quit") break;
        if (line.empty()) continue;
        if (line.rfind("dump ", 0) == 0) {
            std::string src = line.substr(5) + ";";
            compile_dump(src, "repl");
            continue;
        }
        // 允许连续输入多行（若以 { 结尾）
        std::string full = line;
        int depth = 0;
        for (char c : full) {
            if (c == '{') ++depth;
            else if (c == '}') depth = std::max(0, depth - 1);
        }
        while (depth > 0) {
            std::cout << ".... | " << std::flush;
            if (!std::getline(std::cin, line)) break;
            full += "\n" + line;
            for (char c : line) {
                if (c == '{') ++depth;
                else if (c == '}') depth = std::max(0, depth - 1);
            }
        }
        // 如果不以分号结尾，补 ; 但如果是 function / if / for / while 以 { } 结尾则不需要
        try {
            interp.exec_source(full);
        } catch (const std::exception& e) {
            std::cerr << "[Error] " << e.what() << "\n";
        }
    }
}

int main(int argc, char** argv) {
    if (argc <= 1) {
        print_usage(argv[0]);
        return 0;
    }
    std::string a1 = argv[1];
    if (a1 == "-h" || a1 == "--help" || a1 == "help") {
        print_usage(argv[0]);
        return 0;
    }
    if (a1 == "repl") {
        run_repl();
        return 0;
    }
    if (a1 == "run") {
        if (argc < 3) { std::cerr << "Missing file path\n"; return 1; }
        std::string src = read_file(argv[2]);
        if (src.empty()) return 1;
        return run_source(src, argv[2]) ? 0 : 1;
    }
    if (a1 == "compile") {
        if (argc < 3) { std::cerr << "Missing file path\n"; return 1; }
        std::string src = read_file(argv[2]);
        if (src.empty()) return 1;
        return compile_dump(src, argv[2]) ? 0 : 1;
    }
    // 默认：如果是文件则 run
    std::string src = read_file(a1);
    if (src.empty()) {
        std::cerr << "Unknown command or file: " << a1 << "\n";
        print_usage(argv[0]);
        return 1;
    }
    return run_source(src, a1) ? 0 : 1;
}
