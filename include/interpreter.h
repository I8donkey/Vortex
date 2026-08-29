#ifndef NEWCODERING_INTERPRETER_H
#define NEWCODERING_INTERPRETER_H

#include "value.h"
#include "ast.h"
#include <string>
#include <iostream>
#include <unordered_map>
#include <vector>
#include <memory>
#include <functional>

namespace ncr {

// 控制流信号
enum class CtrlFlow { None, Break, Continue, Return };

struct ControlSignal {
    CtrlFlow type = CtrlFlow::None;
    ValuePtr value; // for return
};

// 作用域：Environment 实现一个链
class Environment {
public:
    explicit Environment(Environment* parent = nullptr) : parent_(parent) {}

    // 定义变量（在当前作用域）
    void define(const std::string& name, ValuePtr value, bool is_const = false);
    // 查找变量（向上递归）
    ValuePtr& lookup(const std::string& name);
    // 赋值：必须先存在
    void assign(const std::string& name, ValuePtr value);
    // 是否存在
    bool has(const std::string& name) const;
    // 删除
    void erase(const std::string& name);
    void clear_all();

    // 当前作用域定义的所有名字（用于 del *）
    std::vector<std::string> locals() const;

    Environment* parent() { return parent_; }

private:
    Environment* parent_ = nullptr;
    std::unordered_map<std::string, std::pair<ValuePtr, bool>> vars_; // value, is_const
};

class Interpreter {
public:
    Interpreter();

    // 运行整个程序
    ValuePtr run(const Program& program);

    // 运行单条语句
    ControlSignal execute(const Stmt* stmt);
    // 求值表达式
    ValuePtr evaluate(const Expr* expr);

    // 获取全局环境
    Environment& globals() { return globals_; }

    // 输出输出接口（以便重定向）
    std::function<void(const std::string&)> print_output = [](const std::string& s) {
        fputs(s.c_str(), stdout);
    };
    std::function<std::string(const std::string&)> read_input = [](const std::string& p) {
        fputs(p.c_str(), stdout); fflush(stdout);
        std::string line; std::getline(std::cin, line); return line;
    };

    // 解析并执行字符串源
    void exec_source(const std::string& src);

private:
    Environment globals_;
    Environment* current_env_ = &globals_;

    // 调用栈的命名参数（用于内置方法如 sort 的 cmp 关键字）
    std::vector<std::unordered_map<std::string, ValuePtr>> kwarg_stack_;

    // 已注册的标准库模块（供 import 使用）
    std::unordered_map<std::string, ValuePtr> std_modules_;

    // push/pop scope
    struct ScopeGuard {
        Interpreter* interp;
        Environment saved;
        explicit ScopeGuard(Interpreter* i, Environment* new_env) : interp(i) {
            interp->current_env_ = new_env;
        }
        ~ScopeGuard() {
            // 回到父作用域
            if (interp->current_env_ && interp->current_env_->parent())
                interp->current_env_ = interp->current_env_->parent();
            else
                interp->current_env_ = &interp->globals_;
        }
    };

    Environment* enter_scope();
    void leave_scope();

    // 执行辅助
    ControlSignal execute_block(const std::vector<StmtPtr>& stmts);

    // 语句执行分派
    ControlSignal exec_var_decl(const VarDeclStmt* s);
    ControlSignal exec_const_decl(const ConstDeclStmt* s);
    ControlSignal exec_if(const IfStmt* s);
    ControlSignal exec_for(const ForInStmt* s);
    ControlSignal exec_while(const WhileStmt* s);
    ControlSignal exec_del(const DelStmt* s);
    ControlSignal exec_function_def(const FunctionDefStmt* s);
    ControlSignal exec_import(const ImportStmt* s);
    ControlSignal exec_try(const TryCatchStmt* s);

    // 表达式求值分派
    ValuePtr eval_literal(const LiteralExpr* e);
    ValuePtr eval_identifier(const IdentifierExpr* e);
    ValuePtr eval_unary(const UnaryOpExpr* e);
    ValuePtr eval_binary(const BinaryOpExpr* e);
    ValuePtr eval_ternary(const TernaryOpExpr* e);
    ValuePtr eval_assign(const AssignOpExpr* e);
    ValuePtr eval_call(const CallExpr* e);
    ValuePtr eval_member(const MemberAccessExpr* e);
    ValuePtr eval_subscript(const SubscriptExpr* e);
    ValuePtr eval_list_init(const ListInitExpr* e);
    ValuePtr eval_dict_init(const DictInitExpr* e);
    ValuePtr eval_lambda(const LambdaExpr* e);
    ValuePtr eval_cast(const CastExpr* e);

    // 辅助：赋值目标解析（返回可修改的引用）
    ValuePtr& resolve_lvalue(const Expr* target);
    ValuePtr& variable_ref(const std::string& name);

    // 内置函数
    ValuePtr call_builtin(const std::string& name, const ValueVec& args);
    bool is_builtin(const std::string& name) const;

    // 容器方法调用
    ValuePtr call_method(const ValuePtr& obj, const std::string& method, const ValueVec& args);

    // 初始化全局内置函数
    void init_builtins();
    // 初始化标准库模块（math / time / random）
    void init_modules();

    // 用户定义函数调用
    ValuePtr call_user_function(const FunctionValue* fn, const ValueVec& args);
    ValuePtr call_user_function_with_env(const FunctionDefStmt* def, const ValueVec& args, Environment* parent_env);

    // 左值地址保存（@var）
    // 解释器辅助：赋值
    void perform_assign(ValuePtr& target_ref, const std::string& op, ValuePtr rhs);
};

} // namespace ncr

#endif // NEWCODERING_INTERPRETER_H
