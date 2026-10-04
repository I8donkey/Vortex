#ifndef VORTEX_INTERPRETER_H
#define VORTEX_INTERPRETER_H

#include "value.h"
#include "ast.h"
#include <string>
#include <iostream>
#include <unordered_map>
#include <vector>
#include <memory>
#include <functional>

namespace vortex {

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
    // 整条作用域链可见的名字（去重），用于"did you mean"建议
    std::vector<std::string> all_names() const;

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

    // 模块搜索根目录（默认 cwd）；main 入口可设为脚本所在目录
    void set_module_path(const std::string& dir) { module_base_ = dir; }

    // 记录源码供运行时错误定位（path + 各行文本）；便于输出 file:line:col: error: 上下文
    void set_source_for_errors(const std::string& path, const std::string& src);

    // 注册额外模块（供编辑器等注入出厂标配之外的模块，如 gui）
    void register_extra_module(const std::string& name, ValuePtr mod);

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

    // 用户定义函数调用（供扩展模块在子线程/回调中调用用户函数）
    ValuePtr call_user_function(const FunctionValue* fn, const ValueVec& args);
    ValuePtr call_user_function_with_env(const FunctionDefStmt* def, const ValueVec& args, Environment* parent_env);

private:
    Environment globals_;
    Environment* current_env_ = &globals_;

    // 运行时错误定位（gcc/clang 风格 file:line:col: error: 消息 + 源码上下文）
    int cur_line_ = 0, cur_col_ = 0;      // 当前正在执行的语句位置
    std::string err_path_;                 // 源码文件路径（显示用）
    std::vector<std::string> err_lines_;   // 源码按行拆分（0 基，行号 = index+1）
    // 组装 "path:line:col: error: msg\n  | <src line>\n  | <caret>"；无源码行时退回纯消息
    std::string format_error(const std::string& msg) const;

    // 调用栈的命名参数（用于内置方法如 sort 的 cmp 关键字）
    std::vector<std::unordered_map<std::string, ValuePtr>> kwarg_stack_;

    // 已注册的标准库模块（供 import 使用）
    std::unordered_map<std::string, ValuePtr> std_modules_;

    // 已加载的用户模块（.vt/.vtp 源码模块）。子解释器须保活：
    // 模块内函数闭包捕获其 globals_ 与 AST(def 指针)，销毁则悬空。
    std::unordered_map<std::string, std::unique_ptr<Interpreter>> user_modules_;

    std::string module_base_;   // import 用户模块的搜索根（脚本目录 > cwd）

    // 由本解释器持有并执行的 Program（供模块导入复用，保活函数 def 指针）
    std::shared_ptr<Program> held_prog_;

    // 加载用户源码模块（import 未命中 std_modules_ 时）；失败抛 RuntimeError
    ValuePtr load_user_module(const std::string& module);

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
    ControlSignal exec_class(const ClassDeclStmt* s);

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
    ValuePtr eval_list_comp(const ListCompExpr* e);
    ValuePtr eval_dict_comp(const DictCompExpr* e);

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

    // 左值地址保存（@var）
    // 解释器辅助：赋值
    void perform_assign(ValuePtr& target_ref, const std::string& op, ValuePtr rhs);
};

} // namespace vortex

#endif // VORTEX_INTERPRETER_H
