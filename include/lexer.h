#ifndef NEWCODERING_LEXER_H
#define NEWCODERING_LEXER_H

#include <string>
#include <vector>
#include <cstddef>

namespace ncr {

enum class TokenType {
    // 字面量
    IntLit, FloatLit, CharLit, UnicharLit, StringLit, UniStringLit,
    TrueLit, FalseLit, NoneLit,

    // 标识符 / 关键字
    Identifier,
    Kw_def, Kw_const, Kw_del, Kw_if, Kw_else, Kw_elif, Kw_for, Kw_while,
    Kw_break, Kw_continue, Kw_return, Kw_lambda, Kw_import, Kw_from, Kw_as,
    Kw_in, Kw_is, Kw_global,
    Kw_try, Kw_catch, Kw_finally,
    Kw_and, Kw_or, Kw_not,

    // 类型关键字（用于声明）
    Typ_int, Typ_uint, Typ_short, Typ_ushort, Typ_long, Typ_ulong,
    Typ_float, Typ_double, Typ_char, Typ_unichar, Typ_memadr, Typ_bool,
    Typ_str, Typ_unistr, Typ_bin,
    Typ_list, Typ_stack, Typ_queue, Typ_set, Typ_undset, Typ_dict,
    Typ_pair, Typ_tuple, Typ_object, Typ_function,

    // 标点
    LParen, RParen,        // ( )
    LBrack, RBrack,        // [ ]
    LBrace, RBrace,        // { }
    Comma, Semicolon, Colon, Dot, At, Tilde, Bang, Question, Arrow,

    // 运算符（单/双/多字符）
    Plus, Minus, Star, Slash, SlashSlash, Percent, StarStar,
    Amp, Pipe, Caret, Shl, Shr,
    AmpAmp, PipePipe,
    EqEq, NotEq, Less, Greater, LessEq, GreaterEq,
    Assign, PlusEq, MinusEq, StarEq, SlashEq, SlashSlashEq, PercentEq, StarStarEq,
    AmpEq, PipeEq, CaretEq, ShlEq, ShrEq,

    // 特殊
    EndOfFile, Error,
};

struct Token {
    TokenType type;
    std::string text;      // 原始文本
    std::string str_val;   // 字符串字面量（已去引号转义）
    long long int_val = 0;
    unsigned long long uint_val = 0;
    double float_val = 0.0;
    int char_val = 0;
    bool bool_val = false;
    int line = 1;
    int col = 1;
};

class Lexer {
public:
    explicit Lexer(std::string source);
    std::vector<Token> tokenize();

    // 错误信息
    const std::vector<std::string>& errors() const { return errors_; }

private:
    std::string src_;
    size_t pos_ = 0;
    int line_ = 1;
    int col_ = 1;
    std::vector<std::string> errors_;

    char peek(size_t off = 0) const;
    char advance();
    bool match(char c);
    void skip_whitespace_and_comments();
    void error(const std::string& msg);

    Token make(TokenType t, const std::string& text = {});
    Token make_number();
    Token make_string(char quote);
    Token make_char(char quote);
    Token make_identifier_or_keyword();
};

// 工具
std::string token_type_name(TokenType t);
bool is_type_keyword(TokenType t);
std::string token_type_name_str(TokenType t);

} // namespace ncr

#endif // NEWCODERING_LEXER_H
