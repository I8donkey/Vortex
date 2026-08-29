#ifndef VORTEX_AST_H
#define VORTEX_AST_H

#include <memory>
#include <vector>
#include <string>
#include <utility>

namespace vortex {

// ========== 前向声明 ==========
struct Expr;
struct Stmt;
struct TypeSpec;
struct ParamDecl;

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using TypeSpecPtr = std::unique_ptr<TypeSpec>;
using ParamDeclPtr = std::unique_ptr<ParamDecl>;

// ========== 类型规范 ==========
struct TypeSpec {
    std::string base_name;            // "int","str","list","pair",...
    std::vector<TypeSpecPtr> params;  // 泛型参数 pair<int,str>
    virtual ~TypeSpec() = default;
    TypeSpec() = default;
    TypeSpec(std::string n) : base_name(std::move(n)) {}
};

// ========== 参数声明 ==========
struct ParamDecl {
    TypeSpecPtr type;
    std::string name;
    ExprPtr default_value;  // 可为空
    bool is_vararg = false; // *args
};

// ========== 表达式基类 ==========
enum class ExprKind {
    Literal,
    Identifier,
    UnaryOp,
    BinaryOp,
    TernaryOp,
    AssignOp,
    Call,
    MemberAccess,
    Subscript,
    ListInit,
    DictInit,
    Lambda,
    AddressOf,   // @x
    Dereference, // ~x
    Cast,
};

struct Expr {
    ExprKind kind;
    virtual ~Expr() = default;
protected:
    explicit Expr(ExprKind k) : kind(k) {}
};

// 字面量
struct LiteralExpr : Expr {
    enum class LitKind { Int, Float, Char, Unichar, Bool, String, UniString, None };
    LitKind lit_kind;
    long long int_val = 0;
    unsigned long long uint_val = 0;
    double float_val = 0.0;
    int char_val = 0;
    std::string str_val;
    bool bool_val = false;

    LiteralExpr() : Expr(ExprKind::Literal) {}
};

// 标识符
struct IdentifierExpr : Expr {
    std::string name;
    explicit IdentifierExpr(std::string n) : Expr(ExprKind::Identifier), name(std::move(n)) {}
};

// 一元运算符
struct UnaryOpExpr : Expr {
    std::string op;   // "!", "+", "-", "~", "@"
    ExprPtr operand;
    UnaryOpExpr(std::string o, ExprPtr e)
        : Expr(ExprKind::UnaryOp), op(std::move(o)), operand(std::move(e)) {}
};

// 二元运算符
struct BinaryOpExpr : Expr {
    std::string op;   // "+", "-", "*", "/", "//", "%", "**", "&", "|", "^", "&&", "||", ...
    ExprPtr left;
    ExprPtr right;
    BinaryOpExpr(std::string o, ExprPtr l, ExprPtr r)
        : Expr(ExprKind::BinaryOp), op(std::move(o)), left(std::move(l)), right(std::move(r)) {}
};

// 三目条件
struct TernaryOpExpr : Expr {
    ExprPtr cond;
    ExprPtr then_e;
    ExprPtr else_e;
    TernaryOpExpr(ExprPtr c, ExprPtr t, ExprPtr e)
        : Expr(ExprKind::TernaryOp), cond(std::move(c)), then_e(std::move(t)), else_e(std::move(e)) {}
};

// 赋值与复合赋值
struct AssignOpExpr : Expr {
    std::string op;   // "=", "+=", ...
    ExprPtr target;
    ExprPtr value;
    AssignOpExpr(std::string o, ExprPtr t, ExprPtr v)
        : Expr(ExprKind::AssignOp), op(std::move(o)), target(std::move(t)), value(std::move(v)) {}
};

// 函数调用
struct CallExpr : Expr {
    ExprPtr callee;
    std::vector<ExprPtr> args;
    std::vector<std::pair<std::string, ExprPtr>> kwargs; // 命名参数 (name, value)
    CallExpr(ExprPtr c, std::vector<ExprPtr> a)
        : Expr(ExprKind::Call), callee(std::move(c)), args(std::move(a)) {}
};

// 成员访问 a.b
struct MemberAccessExpr : Expr {
    ExprPtr object;
    std::string member;
    MemberAccessExpr(ExprPtr o, std::string m)
        : Expr(ExprKind::MemberAccess), object(std::move(o)), member(std::move(m)) {}
};

// 下标访问 a[b]
struct SubscriptExpr : Expr {
    ExprPtr object;
    ExprPtr index;
    SubscriptExpr(ExprPtr o, ExprPtr i)
        : Expr(ExprKind::Subscript), object(std::move(o)), index(std::move(i)) {}
};

// 列表初始化 [1,2,3]
struct ListInitExpr : Expr {
    std::vector<ExprPtr> elements;
    ListInitExpr() : Expr(ExprKind::ListInit) {}
    explicit ListInitExpr(std::vector<ExprPtr> es) : Expr(ExprKind::ListInit), elements(std::move(es)) {}
};

// 字典初始化 {k1:v1, k2:v2} 或 [(k,v),...]
struct DictInitExpr : Expr {
    struct KV { ExprPtr key; ExprPtr value; };
    std::vector<KV> pairs;
    DictInitExpr() : Expr(ExprKind::DictInit) {}
};

// Lambda
struct LambdaExpr : Expr {
    std::vector<ParamDeclPtr> params;
    std::vector<StmtPtr> body;
    LambdaExpr() : Expr(ExprKind::Lambda) {}
};

// 类型转换函数调用被视作 Cast 表达式
struct CastExpr : Expr {
    std::string target_type; // "int","str",...
    ExprPtr value;
    CastExpr(std::string t, ExprPtr v)
        : Expr(ExprKind::Cast), target_type(std::move(t)), value(std::move(v)) {}
};

// ========== 语句基类 ==========
enum class StmtKind {
    VarDecl,
    ConstDecl,
    Assign,
    ExprStmt,
    Block,
    If,
    ForIn,
    While,
    Break,
    Continue,
    Return,
    Del,
    FunctionDef,
    Import,
    TryCatch,
};

struct Stmt {
    StmtKind kind;
    virtual ~Stmt() = default;
protected:
    explicit Stmt(StmtKind k) : kind(k) {}
};

// 变量声明
struct VarDeclStmt : Stmt {
    TypeSpecPtr type;
    std::vector<std::pair<std::string, ExprPtr>> names; // name, optional init
    VarDeclStmt() : Stmt(StmtKind::VarDecl) {}
};

// 常量声明
struct ConstDeclStmt : Stmt {
    TypeSpecPtr type;
    std::string name;
    ExprPtr value;
    ConstDeclStmt() : Stmt(StmtKind::ConstDecl) {}
};

// 赋值语句（包装 AssignOpExpr）
struct AssignStmt : Stmt {
    ExprPtr assign; // AssignOpExpr 或复合赋值
    explicit AssignStmt(ExprPtr a) : Stmt(StmtKind::Assign), assign(std::move(a)) {}
};

// 表达式语句
struct ExprStmt : Stmt {
    ExprPtr expr;
    explicit ExprStmt(ExprPtr e) : Stmt(StmtKind::ExprStmt), expr(std::move(e)) {}
};

// 语句块
struct BlockStmt : Stmt {
    std::vector<StmtPtr> stmts;
    BlockStmt() : Stmt(StmtKind::Block) {}
};

// If
struct IfStmt : Stmt {
    struct Elif { ExprPtr cond; std::unique_ptr<BlockStmt> body; };
    ExprPtr cond;
    std::unique_ptr<BlockStmt> then_body;
    std::vector<Elif> elif_list;
    std::unique_ptr<BlockStmt> else_body;
    IfStmt() : Stmt(StmtKind::If) {}
};

// for-in
struct ForInStmt : Stmt {
    std::string var;
    ExprPtr container;
    std::unique_ptr<BlockStmt> body;
    ForInStmt() : Stmt(StmtKind::ForIn) {}
};

// while
struct WhileStmt : Stmt {
    ExprPtr cond;
    std::unique_ptr<BlockStmt> body;
    WhileStmt() : Stmt(StmtKind::While) {}
};

struct BreakStmt : Stmt { BreakStmt() : Stmt(StmtKind::Break) {} };
struct ContinueStmt : Stmt { ContinueStmt() : Stmt(StmtKind::Continue) {} };

// return
struct ReturnStmt : Stmt {
    ExprPtr value; // 可为空
    explicit ReturnStmt(ExprPtr v = nullptr) : Stmt(StmtKind::Return), value(std::move(v)) {}
};

// del
struct DelStmt : Stmt {
    bool delete_all = false;
    std::vector<ExprPtr> targets; // Identifier or Dereference
    DelStmt() : Stmt(StmtKind::Del) {}
};

// 函数定义
struct FunctionDefStmt : Stmt {
    std::string name;
    std::vector<ParamDeclPtr> params;
    std::unique_ptr<BlockStmt> body;
    FunctionDefStmt() : Stmt(StmtKind::FunctionDef) {}
};

// import
struct ImportStmt : Stmt {
    std::string module;
    std::string alias;
    std::vector<std::string> items; // from import
    bool is_from = false;
    ImportStmt() : Stmt(StmtKind::Import) {}
};

// try-catch-finally
struct TryCatchStmt : Stmt {
    std::unique_ptr<BlockStmt> try_body;
    std::string exception_type;
    std::string exception_var;
    std::unique_ptr<BlockStmt> catch_body;
    std::unique_ptr<BlockStmt> finally_body;
    TryCatchStmt() : Stmt(StmtKind::TryCatch) {}
};

// 整个程序 = 语句列表
struct Program {
    std::vector<StmtPtr> stmts;
};

} // namespace vortex

#endif // VORTEX_AST_H
