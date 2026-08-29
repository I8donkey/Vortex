#include "parser.h"
#include <sstream>
#include <stdexcept>

namespace ncr {

Parser::Parser(std::vector<Token> tokens) : toks_(std::move(tokens)) {}

const Token& Parser::peek(size_t off) const {
    size_t i = pos_ + off;
    return i < toks_.size() ? toks_[i] : toks_.back();
}
Token Parser::advance() { return toks_[pos_++]; }
bool Parser::match(TokenType t) {
    if (check(t)) { advance(); return true; }
    return false;
}
Token Parser::expect(TokenType t, const std::string& what) {
    if (check(t)) return advance();
    std::ostringstream oss;
    oss << "[" << peek().line << ":" << peek().col << "] "
        << "Expected " << (what.empty() ? token_type_name_str(t) : what)
        << " but got '" << peek().text << "' (" << token_type_name_str(peek_t()) << ")";
    error(oss.str());
    return {};
}

void Parser::error(const std::string& msg) {
    errors_.push_back(msg);
    throw std::runtime_error(msg);
}

// ======= helpers =======
static bool is_assign_op(TokenType t) {
    switch (t) {
        case TokenType::Assign: case TokenType::PlusEq: case TokenType::MinusEq:
        case TokenType::StarEq: case TokenType::SlashEq: case TokenType::SlashSlashEq:
        case TokenType::PercentEq: case TokenType::StarStarEq:
        case TokenType::AmpEq: case TokenType::PipeEq: case TokenType::CaretEq:
        case TokenType::ShlEq: case TokenType::ShrEq: return true;
        default: return false;
    }
}

static std::string assign_op_str(TokenType t) { return token_type_name_str(t); }

// ======= statements =======
std::unique_ptr<Program> Parser::parse_program() {
    auto prog = std::make_unique<Program>();
    while (!check(TokenType::EndOfFile)) {
        try {
            auto s = parse_stmt();
            if (s) prog->stmts.push_back(std::move(s));
        } catch (const std::runtime_error& e) {
            // 错误同步：跳过直到分号或右大括号
            while (!check(TokenType::EndOfFile) &&
                   !check(TokenType::Semicolon) && !check(TokenType::RBrace))
                advance();
            if (check(TokenType::Semicolon)) advance();
        }
    }
    return prog;
}

StmtPtr Parser::parse_stmt() {
    // 跳过多余分号
    while (match(TokenType::Semicolon)) {}
    if (check(TokenType::EndOfFile)) return nullptr;

    // if / for / while / def / import / try / return / break / continue / del / const
    if (check(TokenType::Kw_if)) return parse_if();
    if (check(TokenType::Kw_for)) return parse_for();
    if (check(TokenType::Kw_while)) return parse_while();
    if (check(TokenType::Kw_def)) return parse_function_def();
    if (check(TokenType::Kw_import) || check(TokenType::Kw_from)) return parse_import();
    if (check(TokenType::Kw_try)) return parse_try();
    if (check(TokenType::Kw_break)) { advance(); expect(TokenType::Semicolon); return std::make_unique<BreakStmt>(); }
    if (check(TokenType::Kw_continue)) { advance(); expect(TokenType::Semicolon); return std::make_unique<ContinueStmt>(); }
    if (check(TokenType::Kw_return)) {
        advance();
        ExprPtr v;
        if (!check(TokenType::Semicolon)) v = parse_expr();
        expect(TokenType::Semicolon);
        return std::make_unique<ReturnStmt>(std::move(v));
    }
    if (check(TokenType::Kw_del)) return parse_del();
    if (check(TokenType::Kw_const)) return parse_const_decl();
    if (check(TokenType::Kw_lambda)) {
        // 顶层 lambda 语句不常见，但允许
        auto e = parse_lambda();
        return parse_expr_as_stmt(std::move(e));
    }

    // 变量声明或表达式：检查下一个标识符后面是否是另一个标识符（声明形式: typename x;）
    // 或者是 type keyword 开头
    if (is_type_keyword(peek_t())) {
        return parse_decl_or_stmt();
    }
    // 普通语句 = 表达式 或 赋值 或 声明 int a=1;
    return parse_decl_or_stmt();
}

StmtPtr Parser::parse_decl_or_stmt() {
    // 可能：<type-keyword> [generic] <identifier> (, <identifier>)* [= init] ;
    // 或者：<identifier> = ... ;（赋值）
    // 或者：<expression> ;（表达式语句，如 call）
    // 为简单起见：先尝试解析一个表达式（可能是 type keyword 做标识符的情况会失败回退）
    // 策略：若第一个 token 是 type-keyword 且第二个 token 是 Identifier / type-keyword / '*'（删除所有）/ '(' / ',' 的情况下按声明走
    if (is_type_keyword(peek_t())) {
        TokenType first = peek_t();
        // 看第二个 token：若为 Identifier 或 '*' 或 ',' 或 '[' 则认为是声明
        TokenType nxt = peek_t(1);
        if (nxt == TokenType::Identifier || nxt == TokenType::Star ||
            nxt == TokenType::Comma || nxt == TokenType::LBrack ||
            is_type_keyword(nxt) || nxt == TokenType::Typ_object) {
            return parse_var_decl();
        }
        // 否则：作为标识符（如 str(...) 转型或调用）
        // 进入 parse_expr_as_stmt
    }
    auto e = parse_expr();
    return parse_expr_as_stmt(std::move(e));
}

StmtPtr Parser::parse_expr_as_stmt(ExprPtr first_expr) {
    ExprPtr e = first_expr ? std::move(first_expr) : parse_expr();
    // 检查是否是赋值目标
    if (is_assign_op(peek_t())) {
        std::string op = assign_op_str(advance());
        auto v = parse_expr();
        e = std::make_unique<AssignOpExpr>(op, std::move(e), std::move(v));
    }
    expect(TokenType::Semicolon);
    // 如果根是 AssignOpExpr -> AssignStmt，否则 ExprStmt
    if (e && e->kind == ExprKind::AssignOp) {
        return std::make_unique<AssignStmt>(std::move(e));
    }
    return std::make_unique<ExprStmt>(std::move(e));
}

StmtPtr Parser::parse_var_decl(TypeSpecPtr type_hint) {
    TypeSpecPtr type;
    if (type_hint) type = std::move(type_hint);
    else type = parse_type_spec();

    auto stmt = std::make_unique<VarDeclStmt>();
    stmt->type = std::move(type);

    // 读取多个 (name [= init])
    while (true) {
        if (check(TokenType::Star) && stmt->names.empty()) {
            // del *; 不是声明场景；这里不处理
        }
        Token n = expect(TokenType::Identifier, "variable name");
        std::string name = n.text;
        ExprPtr init;
        if (match(TokenType::Assign)) {
            init = parse_expr();
        }
        stmt->names.push_back({name, std::move(init)});
        if (!match(TokenType::Comma)) break;
    }
    expect(TokenType::Semicolon);
    return stmt;
}

StmtPtr Parser::parse_const_decl() {
    expect(TokenType::Kw_const);
    auto stmt = std::make_unique<ConstDeclStmt>();
    stmt->type = parse_type_spec();
    Token n = expect(TokenType::Identifier, "constant name");
    stmt->name = n.text;
    expect(TokenType::Assign);
    stmt->value = parse_expr();
    expect(TokenType::Semicolon);
    return stmt;
}

std::unique_ptr<BlockStmt> Parser::parse_block() {
    auto blk = std::make_unique<BlockStmt>();
    expect(TokenType::LBrace);
    while (!check(TokenType::RBrace) && !check(TokenType::EndOfFile)) {
        auto s = parse_stmt();
        if (s) blk->stmts.push_back(std::move(s));
    }
    expect(TokenType::RBrace);
    return blk;
}

StmtPtr Parser::parse_if() {
    auto stmt = std::make_unique<IfStmt>();
    expect(TokenType::Kw_if);
    expect(TokenType::LParen);
    stmt->cond = parse_expr();
    expect(TokenType::RParen);
    stmt->then_body = parse_block();
    while (match(TokenType::Kw_elif)) {
        IfStmt::Elif e;
        expect(TokenType::LParen);
        e.cond = parse_expr();
        expect(TokenType::RParen);
        e.body = parse_block();
        stmt->elif_list.push_back(std::move(e));
    }
    if (match(TokenType::Kw_else)) {
        stmt->else_body = parse_block();
    }
    return stmt;
}

StmtPtr Parser::parse_for() {
    auto stmt = std::make_unique<ForInStmt>();
    expect(TokenType::Kw_for);
    expect(TokenType::LParen);
    Token n = expect(TokenType::Identifier, "loop variable");
    stmt->var = n.text;
    expect(TokenType::Kw_in);
    stmt->container = parse_expr();
    expect(TokenType::RParen);
    stmt->body = parse_block();
    return stmt;
}

StmtPtr Parser::parse_while() {
    auto stmt = std::make_unique<WhileStmt>();
    expect(TokenType::Kw_while);
    expect(TokenType::LParen);
    stmt->cond = parse_expr();
    expect(TokenType::RParen);
    stmt->body = parse_block();
    return stmt;
}

StmtPtr Parser::parse_del() {
    auto stmt = std::make_unique<DelStmt>();
    expect(TokenType::Kw_del);
    if (match(TokenType::Star)) {
        stmt->delete_all = true;
    } else {
        while (true) {
            // 目标：标识符或 ~x 解引用
            if (match(TokenType::Tilde)) {
                auto id = parse_atom(); // ~x 其实我们通过解析 Identifier 包装成解引用
                auto e = std::make_unique<UnaryOpExpr>("~", std::move(id));
                stmt->targets.push_back(std::move(e));
            } else {
                Token n = expect(TokenType::Identifier, "variable to delete");
                stmt->targets.push_back(std::make_unique<IdentifierExpr>(n.text));
            }
            if (!match(TokenType::Comma)) break;
        }
    }
    expect(TokenType::Semicolon);
    return stmt;
}

StmtPtr Parser::parse_function_def() {
    auto stmt = std::make_unique<FunctionDefStmt>();
    expect(TokenType::Kw_def);
    Token n = expect(TokenType::Identifier, "function name");
    stmt->name = n.text;
    expect(TokenType::LParen);
    if (!check(TokenType::RParen)) {
        while (true) {
            stmt->params.push_back(parse_param_decl());
            if (!match(TokenType::Comma)) break;
        }
    }
    expect(TokenType::RParen);
    stmt->body = parse_block();
    return stmt;
}

ParamDeclPtr Parser::parse_param_decl() {
    auto p = std::make_unique<ParamDecl>();
    if (match(TokenType::Star)) {
        p->is_vararg = true;
        Token n = expect(TokenType::Identifier, "vararg name");
        p->name = n.text;
        return p;
    }
    Token n = expect(TokenType::Identifier, "parameter name");
    p->name = n.text;
    // 允许 `name: type` 或 `type name`（规范里是 type param，但也支持 name: type）
    if (match(TokenType::Colon)) {
        p->type = parse_type_spec();
    } else if (is_type_keyword(peek_t()) && !p->type) {
        // 参数列表中可能先写类型
        // 回退：刚才 advance 过 name，其实这是颠倒；简单起见不支持回退，我们忽略
    }
    if (match(TokenType::Assign)) {
        p->default_value = parse_expr();
    }
    return p;
}

StmtPtr Parser::parse_import() {
    auto stmt = std::make_unique<ImportStmt>();
    if (match(TokenType::Kw_from)) {
        stmt->is_from = true;
        Token m = expect(TokenType::Identifier, "module name");
        stmt->module = m.text;
        expect(TokenType::Kw_import);
        while (true) {
            Token it = expect(TokenType::Identifier, "import item");
            stmt->items.push_back(it.text);
            if (!match(TokenType::Comma)) break;
        }
    } else {
        expect(TokenType::Kw_import);
        Token m = expect(TokenType::Identifier, "module name");
        stmt->module = m.text;
        if (match(TokenType::Kw_as)) {
            Token a = expect(TokenType::Identifier, "alias");
            stmt->alias = a.text;
        }
    }
    // 允许分号
    if (check(TokenType::Semicolon)) advance();
    return stmt;
}

StmtPtr Parser::parse_try() {
    auto stmt = std::make_unique<TryCatchStmt>();
    expect(TokenType::Kw_try);
    stmt->try_body = parse_block();
    if (match(TokenType::Kw_catch)) {
        if (match(TokenType::LParen)) {
            if (is_type_keyword(peek_t())) {
                auto tp = parse_type_spec();
                stmt->exception_type = tp->base_name;
            }
            Token var = expect(TokenType::Identifier, "exception var");
            stmt->exception_var = var.text;
            expect(TokenType::RParen);
        }
        stmt->catch_body = parse_block();
    }
    if (match(TokenType::Kw_finally)) {
        stmt->finally_body = parse_block();
    }
    return stmt;
}

TypeSpecPtr Parser::parse_type_spec() {
    // 泛型支持 pair<a,b> / tuple<a,b,c>
    Token t;
    if (is_type_keyword(peek_t())) t = advance();
    else if (check(TokenType::Identifier)) t = advance(); // 用户自定义
    else t = expect(TokenType::Typ_int, "type name");
    auto ts = std::make_unique<TypeSpec>(t.text);
    if (match(TokenType::Less)) {
        while (!check(TokenType::Greater) && !check(TokenType::EndOfFile)) {
            ts->params.push_back(parse_type_spec());
            if (!match(TokenType::Comma)) break;
        }
        expect(TokenType::Greater);
    }
    return ts;
}

// ======= 表达式解析（按优先级）=======
ExprPtr Parser::parse_expr() { return parse_assign(); }

ExprPtr Parser::parse_assign() {
    auto lhs = parse_ternary();
    if (is_assign_op(peek_t())) {
        std::string op = assign_op_str(advance());
        auto rhs = parse_assign(); // 右结合
        return std::make_unique<AssignOpExpr>(op, std::move(lhs), std::move(rhs));
    }
    return lhs;
}

ExprPtr Parser::parse_ternary() {
    auto cond = parse_or();
    if (match(TokenType::Question)) {
        auto t = parse_expr();
        expect(TokenType::Colon);
        auto e = parse_ternary();
        return std::make_unique<TernaryOpExpr>(std::move(cond), std::move(t), std::move(e));
    }
    return cond;
}

ExprPtr Parser::parse_or() {
    auto lhs = parse_and();
    while (match(TokenType::PipePipe)) {
        auto rhs = parse_and();
        lhs = std::make_unique<BinaryOpExpr>("||", std::move(lhs), std::move(rhs));
    }
    return lhs;
}
ExprPtr Parser::parse_and() {
    auto lhs = parse_bitor();
    while (match(TokenType::AmpAmp)) {
        auto rhs = parse_bitor();
        lhs = std::make_unique<BinaryOpExpr>("&&", std::move(lhs), std::move(rhs));
    }
    return lhs;
}
ExprPtr Parser::parse_bitor() {
    auto lhs = parse_bitxor();
    while (match(TokenType::Pipe)) {
        auto rhs = parse_bitxor();
        lhs = std::make_unique<BinaryOpExpr>("|", std::move(lhs), std::move(rhs));
    }
    return lhs;
}
ExprPtr Parser::parse_bitxor() {
    auto lhs = parse_bitand();
    while (match(TokenType::Caret)) {
        auto rhs = parse_bitand();
        lhs = std::make_unique<BinaryOpExpr>("^", std::move(lhs), std::move(rhs));
    }
    return lhs;
}
ExprPtr Parser::parse_bitand() {
    auto lhs = parse_eq();
    while (match(TokenType::Amp)) {
        auto rhs = parse_eq();
        lhs = std::make_unique<BinaryOpExpr>("&", std::move(lhs), std::move(rhs));
    }
    return lhs;
}
ExprPtr Parser::parse_eq() {
    auto lhs = parse_rel();
    while (check(TokenType::EqEq) || check(TokenType::NotEq)) {
        std::string op = token_type_name_str(advance().type);
        auto rhs = parse_rel();
        lhs = std::make_unique<BinaryOpExpr>(op, std::move(lhs), std::move(rhs));
    }
    return lhs;
}
ExprPtr Parser::parse_rel() {
    auto lhs = parse_shift();
    while (check(TokenType::Less) || check(TokenType::Greater) ||
           check(TokenType::LessEq) || check(TokenType::GreaterEq)) {
        std::string op = token_type_name_str(advance().type);
        auto rhs = parse_shift();
        lhs = std::make_unique<BinaryOpExpr>(op, std::move(lhs), std::move(rhs));
    }
    return lhs;
}
ExprPtr Parser::parse_shift() {
    auto lhs = parse_add();
    while (check(TokenType::Shl) || check(TokenType::Shr)) {
        std::string op = token_type_name_str(advance().type);
        auto rhs = parse_add();
        lhs = std::make_unique<BinaryOpExpr>(op, std::move(lhs), std::move(rhs));
    }
    return lhs;
}
ExprPtr Parser::parse_add() {
    auto lhs = parse_mul();
    while (check(TokenType::Plus) || check(TokenType::Minus)) {
        std::string op = token_type_name_str(advance().type);
        auto rhs = parse_mul();
        lhs = std::make_unique<BinaryOpExpr>(op, std::move(lhs), std::move(rhs));
    }
    return lhs;
}
ExprPtr Parser::parse_mul() {
    auto lhs = parse_power();
    while (check(TokenType::Star) || check(TokenType::Slash) ||
           check(TokenType::SlashSlash) || check(TokenType::Percent)) {
        std::string op = token_type_name_str(advance().type);
        auto rhs = parse_power();
        lhs = std::make_unique<BinaryOpExpr>(op, std::move(lhs), std::move(rhs));
    }
    return lhs;
}
ExprPtr Parser::parse_power() {
    // 右结合
    auto base = parse_unary();
    if (match(TokenType::StarStar)) {
        auto exp = parse_power();
        return std::make_unique<BinaryOpExpr>("**", std::move(base), std::move(exp));
    }
    return base;
}

ExprPtr Parser::parse_unary() {
    if (match(TokenType::At)) {
        // 取地址 @x
        auto operand = parse_unary();
        return std::make_unique<UnaryOpExpr>("@", std::move(operand));
    }
    if (match(TokenType::Tilde)) {
        auto operand = parse_unary();
        return std::make_unique<UnaryOpExpr>("~", std::move(operand));
    }
    if (match(TokenType::Bang)) {
        auto operand = parse_unary();
        return std::make_unique<UnaryOpExpr>("!", std::move(operand));
    }
    if (check(TokenType::Kw_not)) {
        advance();
        auto operand = parse_unary();
        return std::make_unique<UnaryOpExpr>("!", std::move(operand));
    }
    if (match(TokenType::Minus)) {
        auto operand = parse_unary();
        return std::make_unique<UnaryOpExpr>("-", std::move(operand));
    }
    if (match(TokenType::Plus)) {
        auto operand = parse_unary();
        return std::make_unique<UnaryOpExpr>("+", std::move(operand));
    }
    return parse_postfix();
}

ExprPtr Parser::parse_postfix() {
    auto base = parse_atom();
    while (true) {
        if (match(TokenType::LParen)) {
            // 调用
            std::vector<ExprPtr> args;
            if (!check(TokenType::RParen)) {
                while (true) {
                    args.push_back(parse_expr());
                    if (!match(TokenType::Comma)) break;
                }
            }
            expect(TokenType::RParen);
            // 类型转换：type-name(args)
            if (base && base->kind == ExprKind::Identifier) {
                auto& id = static_cast<IdentifierExpr&>(*base);
                if (args.size() == 1 &&
                    (id.name == "int" || id.name == "uint" || id.name == "long" ||
                     id.name == "ulong" || id.name == "float" || id.name == "double" ||
                     id.name == "bool" || id.name == "str" || id.name == "unistr" ||
                     id.name == "bin" || id.name == "char" || id.name == "unichar")) {
                    // Cast
                    base = std::make_unique<CastExpr>(id.name, std::move(args[0]));
                    continue;
                }
            }
            base = std::make_unique<CallExpr>(std::move(base), std::move(args));
            continue;
        }
        if (match(TokenType::Dot)) {
            Token m = expect(TokenType::Identifier, "member name");
            base = std::make_unique<MemberAccessExpr>(std::move(base), m.text);
            continue;
        }
        if (match(TokenType::LBrack)) {
            auto idx = parse_expr();
            expect(TokenType::RBrack);
            base = std::make_unique<SubscriptExpr>(std::move(base), std::move(idx));
            continue;
        }
        break;
    }
    return base;
}

ExprPtr Parser::parse_atom() {
    // 字面量
    if (check(TokenType::IntLit)) {
        Token t = advance();
        auto e = std::make_unique<LiteralExpr>();
        e->lit_kind = LiteralExpr::LitKind::Int;
        if (t.text.size() >= 2 && t.text[0] == '0' && (t.text[1] == 'x' || t.text[1] == 'X')) {
            e->lit_kind = LiteralExpr::LitKind::Int;
        }
        e->int_val = t.int_val; e->uint_val = t.uint_val;
        e->str_val = t.text;
        return e;
    }
    if (check(TokenType::FloatLit)) {
        Token t = advance();
        auto e = std::make_unique<LiteralExpr>();
        e->lit_kind = LiteralExpr::LitKind::Float;
        e->float_val = t.float_val;
        return e;
    }
    if (check(TokenType::TrueLit) || check(TokenType::FalseLit)) {
        Token t = advance();
        auto e = std::make_unique<LiteralExpr>();
        e->lit_kind = LiteralExpr::LitKind::Bool;
        e->bool_val = (t.type == TokenType::TrueLit);
        return e;
    }
    if (check(TokenType::NoneLit)) {
        advance();
        auto e = std::make_unique<LiteralExpr>();
        e->lit_kind = LiteralExpr::LitKind::None;
        return e;
    }
    if (check(TokenType::StringLit)) {
        Token t = advance();
        auto e = std::make_unique<LiteralExpr>();
        e->lit_kind = LiteralExpr::LitKind::String;
        e->str_val = t.str_val;
        return e;
    }
    if (check(TokenType::CharLit)) {
        Token t = advance();
        auto e = std::make_unique<LiteralExpr>();
        e->lit_kind = LiteralExpr::LitKind::Char;
        e->char_val = t.char_val;
        return e;
    }
    if (check(TokenType::LParen)) {
        advance();
        auto e = parse_expr();
        expect(TokenType::RParen);
        return e;
    }
    if (check(TokenType::LBrack)) {
        advance();
        auto lst = std::make_unique<ListInitExpr>();
        if (!check(TokenType::RBrack)) {
            while (true) {
                lst->elements.push_back(parse_expr());
                if (!match(TokenType::Comma)) break;
            }
        }
        expect(TokenType::RBrack);
        return lst;
    }
    if (check(TokenType::LBrace)) {
        advance();
        // 字典初始化（k:v, ...）或 set 字面量
        auto dict = std::make_unique<DictInitExpr>();
        std::vector<ExprPtr> set_list;
        bool is_dict = false;
        if (!check(TokenType::RBrace)) {
            while (true) {
                auto first = parse_expr();
                if (match(TokenType::Colon)) {
                    is_dict = true;
                    auto second = parse_expr();
                    dict->pairs.push_back({std::move(first), std::move(second)});
                } else {
                    set_list.push_back(std::move(first));
                }
                if (!match(TokenType::Comma)) break;
            }
        }
        expect(TokenType::RBrace);
        if (is_dict) return dict;
        // set
        auto lst = std::make_unique<ListInitExpr>(std::move(set_list));
        // 包装为 set(x) 的调用形式：用标识符 "set_init_marker" 加参数，由解释器通过 set 构造判定
        // 更简单：使用特殊 Identifier + Call
        auto callee = std::make_unique<IdentifierExpr>("__set_literal__");
        std::vector<ExprPtr> args;
        args.push_back(std::move(lst));
        return std::make_unique<CallExpr>(std::move(callee), std::move(args));
    }
    if (check(TokenType::Kw_lambda)) {
        return parse_lambda();
    }
    if (check(TokenType::Identifier)) {
        Token t = advance();
        // 可能是 make_pair / make_tuple 等调用，但这里只是 Identifier
        return std::make_unique<IdentifierExpr>(t.text);
    }
    if (is_type_keyword(peek_t())) {
        Token t = advance();
        return std::make_unique<IdentifierExpr>(t.text);
    }
    // error
    std::ostringstream oss;
    oss << "[" << peek().line << ":" << peek().col << "] Unexpected token '"
        << peek().text << "' in expression";
    error(oss.str());
    return nullptr;
}

ExprPtr Parser::parse_lambda() {
    auto expr = std::make_unique<LambdaExpr>();
    expect(TokenType::Kw_lambda);
    expect(TokenType::LParen);
    if (!check(TokenType::RParen)) {
        while (true) {
            expr->params.push_back(parse_param_decl());
            if (!match(TokenType::Comma)) break;
        }
    }
    expect(TokenType::RParen);
    if (match(TokenType::Colon)) {}
    expect(TokenType::LBrace);
    while (!check(TokenType::RBrace) && !check(TokenType::EndOfFile)) {
        auto s = parse_stmt();
        if (s) expr->body.push_back(std::move(s));
    }
    expect(TokenType::RBrace);
    return expr;
}

} // namespace ncr
