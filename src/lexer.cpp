#include "lexer.h"
#include <cctype>
#include <sstream>
#include <unordered_map>
#include <algorithm>

namespace vortex {

static const std::unordered_map<std::string, TokenType> KEYWORDS = {
    {"def", TokenType::Kw_def}, {"const", TokenType::Kw_const},
    {"del", TokenType::Kw_del}, {"if", TokenType::Kw_if},
    {"else", TokenType::Kw_else}, {"elif", TokenType::Kw_elif},
    {"for", TokenType::Kw_for}, {"while", TokenType::Kw_while},
    {"break", TokenType::Kw_break}, {"continue", TokenType::Kw_continue},
    {"return", TokenType::Kw_return}, {"lambda", TokenType::Kw_lambda},
    {"import", TokenType::Kw_import}, {"from", TokenType::Kw_from},
    {"as", TokenType::Kw_as}, {"in", TokenType::Kw_in},
    {"is", TokenType::Kw_is}, {"global", TokenType::Kw_global},
    {"try", TokenType::Kw_try}, {"catch", TokenType::Kw_catch},
    {"finally", TokenType::Kw_finally},
    {"and", TokenType::Kw_and}, {"or", TokenType::Kw_or}, {"not", TokenType::Kw_not},
    {"true", TokenType::TrueLit}, {"false", TokenType::FalseLit}, {"None", TokenType::NoneLit},

    {"int", TokenType::Typ_int}, {"uint", TokenType::Typ_uint},
    {"short", TokenType::Typ_short}, {"ushort", TokenType::Typ_ushort},
    {"long", TokenType::Typ_long}, {"ulong", TokenType::Typ_ulong},
    {"float", TokenType::Typ_float}, {"double", TokenType::Typ_double},
    {"char", TokenType::Typ_char}, {"unichar", TokenType::Typ_unichar},
    {"memadr", TokenType::Typ_memadr}, {"bool", TokenType::Typ_bool},
    {"str", TokenType::Typ_str}, {"unistr", TokenType::Typ_unistr},
    {"bin", TokenType::Typ_bin},
    {"list", TokenType::Typ_list}, {"stack", TokenType::Typ_stack},
    {"queue", TokenType::Typ_queue}, {"set", TokenType::Typ_set},
    {"undset", TokenType::Typ_undset}, {"dict", TokenType::Typ_dict},
    {"pair", TokenType::Typ_pair}, {"tuple", TokenType::Typ_tuple},
    {"object", TokenType::Typ_object}, {"function", TokenType::Typ_function},
};

std::string token_type_name_str(TokenType t) {
    switch (t) {
        case TokenType::IntLit: return "IntLit"; case TokenType::FloatLit: return "FloatLit";
        case TokenType::CharLit: return "CharLit"; case TokenType::UnicharLit: return "UnicharLit";
        case TokenType::StringLit: return "StringLit"; case TokenType::UniStringLit: return "UniStringLit";
        case TokenType::TrueLit: return "true"; case TokenType::FalseLit: return "false"; case TokenType::NoneLit: return "None";
        case TokenType::Identifier: return "Identifier";
        case TokenType::LParen: return "("; case TokenType::RParen: return ")";
        case TokenType::LBrack: return "["; case TokenType::RBrack: return "]";
        case TokenType::LBrace: return "{"; case TokenType::RBrace: return "}";
        case TokenType::Comma: return ","; case TokenType::Semicolon: return ";"; case TokenType::Colon: return ":";
        case TokenType::Dot: return "."; case TokenType::At: return "@"; case TokenType::Tilde: return "~";
        case TokenType::Bang: return "!"; case TokenType::Question: return "?"; case TokenType::Arrow: return "=>";
        case TokenType::Plus: return "+"; case TokenType::Minus: return "-"; case TokenType::Star: return "*";
        case TokenType::Slash: return "/"; case TokenType::SlashSlash: return "//";
        case TokenType::Percent: return "%"; case TokenType::StarStar: return "**";
        case TokenType::Amp: return "&"; case TokenType::Pipe: return "|"; case TokenType::Caret: return "^";
        case TokenType::Shl: return "<<"; case TokenType::Shr: return ">>";
        case TokenType::AmpAmp: return "&&"; case TokenType::PipePipe: return "||";
        case TokenType::EqEq: return "=="; case TokenType::NotEq: return "!=";
        case TokenType::Less: return "<"; case TokenType::Greater: return ">";
        case TokenType::LessEq: return "<="; case TokenType::GreaterEq: return ">=";
        case TokenType::Assign: return "="; case TokenType::PlusEq: return "+="; case TokenType::MinusEq: return "-=";
        case TokenType::StarEq: return "*="; case TokenType::SlashEq: return "/="; case TokenType::SlashSlashEq: return "//=";
        case TokenType::PercentEq: return "%="; case TokenType::StarStarEq: return "**=";
        case TokenType::AmpEq: return "&="; case TokenType::PipeEq: return "|="; case TokenType::CaretEq: return "^=";
        case TokenType::ShlEq: return "<<="; case TokenType::ShrEq: return ">>=";
        case TokenType::EndOfFile: return "EOF";
        default: break;
    }
    return "?";
}

std::string token_type_name(TokenType t) { return token_type_name_str(t); }

bool is_type_keyword(TokenType t) {
    return t >= TokenType::Typ_int && t <= TokenType::Typ_function;
}

// ========== Lexer 实现 ==========
Lexer::Lexer(std::string source) : src_(std::move(source)) {}

char Lexer::peek(size_t off) const {
    return pos_ + off < src_.size() ? src_[pos_ + off] : '\0';
}
char Lexer::advance() {
    char c = src_[pos_++];
    if (c == '\n') { ++line_; col_ = 1; }
    else { ++col_; }
    return c;
}
bool Lexer::match(char c) {
    if (peek() == c) { advance(); return true; }
    return false;
}
void Lexer::error(const std::string& msg) {
    std::ostringstream oss;
    oss << "[Lexer " << line_ << ":" << col_ << "] " << msg;
    errors_.push_back(oss.str());
}
Token Lexer::make(TokenType t, const std::string& text) {
    Token tok; tok.type = t; tok.line = line_; tok.col = col_;
    tok.text = text.empty() ? std::string(1, peek() ? peek() : ' ') : text;
    return tok;
}

void Lexer::skip_whitespace_and_comments() {
    while (pos_ < src_.size()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { advance(); }
        else if (c == '#') {
            while (pos_ < src_.size() && peek() != '\n') advance();
        } else {
            break;
        }
    }
}

Token Lexer::make_number() {
    int start_line = line_, start_col = col_;
    size_t start_pos = pos_;
    Token tok; tok.line = start_line; tok.col = start_col;

    // 检测进制
    int base = 10;
    if (peek() == '0') {
        char n = peek(1);
        if (n == 'b' || n == 'B') { advance(); advance(); base = 2; }
        else if (n == 'o' || n == 'O') { advance(); advance(); base = 8; }
        else if (n == 'x' || n == 'X') { advance(); advance(); base = 16; }
    }

    auto is_digit = [base](char c) -> bool {
        if (c == '\0') return false;
        if (base == 2) return c == '0' || c == '1';
        if (base == 8) return c >= '0' && c <= '7';
        if (base == 16) return std::isxdigit((unsigned char)c);
        return std::isdigit((unsigned char)c);
    };

    std::string num_str;
    while (is_digit(peek())) { num_str += advance(); }

    bool is_float = false;
    if (base == 10 && (peek() == '.' || peek() == 'e' || peek() == 'E')) {
        is_float = true;
        if (peek() == '.') {
            num_str += advance();
            while (std::isdigit((unsigned char)peek())) num_str += advance();
        }
        if (peek() == 'e' || peek() == 'E') {
            num_str += advance();
            if (peek() == '+' || peek() == '-') num_str += advance();
            while (std::isdigit((unsigned char)peek())) num_str += advance();
        }
    }
    // 十六进制浮点 0x1.2p3
    if (base == 16 && peek() == '.') {
        is_float = true;
        num_str += advance();
        while (std::isxdigit((unsigned char)peek())) num_str += advance();
    }
    if (base == 16 && (peek() == 'p' || peek() == 'P')) {
        is_float = true;
        num_str += advance();
        if (peek() == '+' || peek() == '-') num_str += advance();
        while (std::isdigit((unsigned char)peek())) num_str += advance();
    }

    tok.text = src_.substr(start_pos, pos_ - start_pos);
    if (is_float) {
        tok.type = TokenType::FloatLit;
        try {
            tok.float_val = std::stod(tok.text);
        } catch (...) { tok.float_val = 0.0; }
    } else {
        tok.type = TokenType::IntLit;
        try {
            unsigned long long v = std::stoull(num_str.empty() ? "0" : num_str, nullptr, base);
            tok.uint_val = v;
            tok.int_val = (long long)v;
        } catch (...) { tok.int_val = 0; tok.uint_val = 0; }
    }
    return tok;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

Token Lexer::make_string(char quote) {
    int start_line = line_, start_col = col_;
    size_t start_pos = pos_ - 1; // include opening quote
    std::string val;
    while (pos_ < src_.size()) {
        char c = advance();
        if (c == quote) break;
        if (c == '\\') {
            char e = advance();
            switch (e) {
                case 'n': val += '\n'; break; case 't': val += '\t'; break;
                case 'r': val += '\r'; break; case '\\': val += '\\'; break;
                case '"': val += '"'; break; case '\'': val += '\''; break;
                case '0': val += '\0'; break;
                case 'x': {
                    int v = 0;
                    for (int i = 0; i < 2; ++i) {
                        int h = hex_val(advance());
                        if (h < 0) break;
                        v = (v << 4) | h;
                    }
                    val += (char)v;
                    break;
                }
                default: val += e; break;
            }
        } else {
            val += c;
        }
    }
    Token tok; tok.type = TokenType::StringLit;
    tok.line = start_line; tok.col = start_col;
    tok.text = src_.substr(start_pos, pos_ - start_pos);
    tok.str_val = val;
    return tok;
}

Token Lexer::make_char(char quote) {
    int start_line = line_, start_col = col_;
    size_t start_pos = pos_ - 1;
    int value = 0;
    char c = advance();
    if (c == '\\') {
        char e = advance();
        switch (e) {
            case 'n': value = '\n'; break; case 't': value = '\t'; break;
            case 'r': value = '\r'; break; case '0': value = '\0'; break;
            case '\\': value = '\\'; break; case '\'': value = '\''; break;
            case '"': value = '"'; break;
            case 'x': {
                int v = 0;
                for (int i = 0; i < 2; ++i) {
                    int h = hex_val(advance());
                    if (h < 0) break;
                    v = (v << 4) | h;
                }
                value = v; break;
            }
            default: value = e; break;
        }
    } else {
        value = (unsigned char)c;
    }
    advance(); // skip closing quote
    Token tok; tok.type = TokenType::CharLit;
    tok.line = start_line; tok.col = start_col;
    tok.text = src_.substr(start_pos, pos_ - start_pos);
    tok.char_val = value; tok.int_val = value;
    return tok;
}

Token Lexer::make_identifier_or_keyword() {
    int start_line = line_, start_col = col_;
    size_t start_pos = pos_;
    while (pos_ < src_.size()) {
        char c = peek();
        if (std::isalnum((unsigned char)c) || c == '_') advance();
        else break;
    }
    std::string word = src_.substr(start_pos, pos_ - start_pos);
    Token tok; tok.line = start_line; tok.col = start_col; tok.text = word;
    auto it = KEYWORDS.find(word);
    if (it != KEYWORDS.end()) {
        tok.type = it->second;
        if (it->second == TokenType::TrueLit) { tok.bool_val = true; tok.int_val = 1; }
        else if (it->second == TokenType::FalseLit) { tok.bool_val = false; tok.int_val = 0; }
    } else {
        tok.type = TokenType::Identifier;
    }
    return tok;
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> toks;
    pos_ = 0; line_ = 1; col_ = 1;
    while (pos_ < src_.size()) {
        skip_whitespace_and_comments();
        if (pos_ >= src_.size()) break;
        int start_col = col_, start_line = line_;
        size_t start_pos = pos_;
        char c = peek();

        // 数字
        if (std::isdigit((unsigned char)c)) {
            toks.push_back(make_number());
            continue;
        }

        // 标识符 / 关键字
        if (std::isalpha((unsigned char)c) || c == '_') {
            toks.push_back(make_identifier_or_keyword());
            continue;
        }

        // 字符串
        if (c == '"') { advance(); toks.push_back(make_string('"')); continue; }
        if (c == '\'') { advance(); toks.push_back(make_char('\'')); continue; }

        // 标点 & 运算符
        Token tok; tok.line = start_line; tok.col = start_col;
        auto push_op = [&](TokenType t, const std::string& s) {
            tok.type = t; tok.text = s; toks.push_back(tok);
        };
        switch (c) {
            case '(': advance(); push_op(TokenType::LParen, "("); continue;
            case ')': advance(); push_op(TokenType::RParen, ")"); continue;
            case '[': advance(); push_op(TokenType::LBrack, "["); continue;
            case ']': advance(); push_op(TokenType::RBrack, "]"); continue;
            case '{': advance(); push_op(TokenType::LBrace, "{"); continue;
            case '}': advance(); push_op(TokenType::RBrace, "}"); continue;
            case ',': advance(); push_op(TokenType::Comma, ","); continue;
            case ';': advance(); push_op(TokenType::Semicolon, ";"); continue;
            case ':': advance(); push_op(TokenType::Colon, ":"); continue;
            case '.': advance(); push_op(TokenType::Dot, "."); continue;
            case '@': advance(); push_op(TokenType::At, "@"); continue;
            case '~': advance(); push_op(TokenType::Tilde, "~"); continue;
            case '?': advance(); push_op(TokenType::Question, "?"); continue;

            case '+':
                advance();
                if (match('=')) push_op(TokenType::PlusEq, "+=");
                else push_op(TokenType::Plus, "+");
                continue;
            case '-':
                advance();
                if (match('=')) push_op(TokenType::MinusEq, "-=");
                else push_op(TokenType::Minus, "-");
                continue;
            case '*':
                advance();
                if (match('*')) {
                    if (match('=')) push_op(TokenType::StarStarEq, "**=");
                    else push_op(TokenType::StarStar, "**");
                } else if (match('=')) push_op(TokenType::StarEq, "*=");
                else push_op(TokenType::Star, "*");
                continue;
            case '/':
                advance();
                if (match('/')) {
                    if (match('=')) push_op(TokenType::SlashSlashEq, "//=");
                    else push_op(TokenType::SlashSlash, "//");
                } else if (match('=')) push_op(TokenType::SlashEq, "/=");
                else push_op(TokenType::Slash, "/");
                continue;
            case '%':
                advance();
                if (match('=')) push_op(TokenType::PercentEq, "%=");
                else push_op(TokenType::Percent, "%");
                continue;
            case '&':
                advance();
                if (match('&')) push_op(TokenType::AmpAmp, "&&");
                else if (match('=')) push_op(TokenType::AmpEq, "&=");
                else push_op(TokenType::Amp, "&");
                continue;
            case '|':
                advance();
                if (match('|')) push_op(TokenType::PipePipe, "||");
                else if (match('=')) push_op(TokenType::PipeEq, "|=");
                else push_op(TokenType::Pipe, "|");
                continue;
            case '^':
                advance();
                if (match('=')) push_op(TokenType::CaretEq, "^=");
                else push_op(TokenType::Caret, "^");
                continue;
            case '<':
                advance();
                if (match('<')) {
                    if (match('=')) push_op(TokenType::ShlEq, "<<=");
                    else push_op(TokenType::Shl, "<<");
                } else if (match('=')) push_op(TokenType::LessEq, "<=");
                else push_op(TokenType::Less, "<");
                continue;
            case '>':
                advance();
                if (match('>')) {
                    if (match('=')) push_op(TokenType::ShrEq, ">>=");
                    else push_op(TokenType::Shr, ">>");
                } else if (match('=')) push_op(TokenType::GreaterEq, ">=");
                else push_op(TokenType::Greater, ">");
                continue;
            case '=':
                advance();
                if (match('=')) push_op(TokenType::EqEq, "==");
                else if (match('>')) push_op(TokenType::Arrow, "=>");
                else push_op(TokenType::Assign, "=");
                continue;
            case '!':
                advance();
                if (match('=')) push_op(TokenType::NotEq, "!=");
                else push_op(TokenType::Bang, "!");
                continue;
            default: {
                std::ostringstream oss;
                oss << "Unexpected character: '" << c << "' (ASCII " << (int)(unsigned char)c << ")";
                error(oss.str());
                advance();
                Token err; err.type = TokenType::Error;
                err.line = start_line; err.col = start_col;
                err.text = src_.substr(start_pos, pos_ - start_pos);
                toks.push_back(err);
                continue;
            }
        }
    }
    Token eof; eof.type = TokenType::EndOfFile; eof.line = line_; eof.col = col_; eof.text = "EOF";
    toks.push_back(eof);
    return toks;
}

} // namespace vortex
