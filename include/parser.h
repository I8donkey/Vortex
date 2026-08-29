#ifndef VORTEX_PARSER_H
#define VORTEX_PARSER_H

#include "lexer.h"
#include "ast.h"
#include <vector>
#include <string>
#include <memory>

namespace vortex {

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    std::unique_ptr<Program> parse_program();

    const std::vector<std::string>& errors() const { return errors_; }

private:
    std::vector<Token> toks_;
    size_t pos_ = 0;
    std::vector<std::string> errors_;

    const Token& peek(size_t off = 0) const;
    TokenType peek_t(size_t off = 0) const { return peek(off).type; }
    Token advance();
    bool check(TokenType t) const { return peek_t() == t; }
    bool match(TokenType t);
    Token expect(TokenType t, const std::string& what = {});
    [[noreturn]] void error(const std::string& msg);

    // ==== 语句 ====
    StmtPtr parse_stmt();
    StmtPtr parse_decl_or_stmt();   // 可能以 type keyword 开头
    std::unique_ptr<BlockStmt> parse_block();
    std::vector<StmtPtr> parse_stmt_list_until(TokenType end); // 直到 } 或 EOF，但不消费
    StmtPtr parse_var_decl(TypeSpecPtr type_hint = nullptr);
    StmtPtr parse_const_decl();
    StmtPtr parse_if();
    StmtPtr parse_for();
    StmtPtr parse_while();
    StmtPtr parse_del();
    StmtPtr parse_function_def();
    StmtPtr parse_import();
    StmtPtr parse_try();
    StmtPtr parse_expr_as_stmt(ExprPtr first_expr = nullptr);

    // ==== 表达式（按优先级） ====
    ExprPtr parse_expr();
    ExprPtr parse_assign();       // =, +=, ... (lowest)
    ExprPtr parse_ternary();      // ?:
    ExprPtr parse_or();           // ||
    ExprPtr parse_and();          // &&
    ExprPtr parse_bitor();        // |
    ExprPtr parse_bitxor();       // ^
    ExprPtr parse_bitand();       // &
    ExprPtr parse_eq();           // == !=
    ExprPtr parse_rel();          // < > <= >=
    ExprPtr parse_shift();        // << >>
    ExprPtr parse_add();          // + -
    ExprPtr parse_mul();          // * / // %
    ExprPtr parse_power();        // **
    ExprPtr parse_unary();        // @ ~ ! - +
    ExprPtr parse_postfix();      // () [] .
    ExprPtr parse_primary();
    ExprPtr parse_atom();         // 括号、字面量、标识符、列表、字典初始化

    // 类型解析
    TypeSpecPtr parse_type_spec();
    ParamDeclPtr parse_param_decl();

    // lambda
    ExprPtr parse_lambda();
};

} // namespace vortex

#endif // VORTEX_PARSER_H
