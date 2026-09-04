#include "SyntaxHighlighter.h"

#include <QBrush>
#include <QColor>

SyntaxHighlighter::SyntaxHighlighter(QTextDocument *parent)
    : QSyntaxHighlighter(parent)
{
    HighlightingRule rule;

    // ========== 颜色方案（浅色友好，深色下自动兜底） ==========
    keywordFormat_.setForeground(QColor(139, 69, 19));     // 棕色 关键字
    keywordFormat_.setFontWeight(QFont::Bold);

    typeFormat_.setForeground(QColor(0, 100, 180));        // 深蓝色 类型
    typeFormat_.setFontItalic(true);

    literalFormat_.setForeground(QColor(220, 20, 60));     // 猩红 true/false/none
    literalFormat_.setFontWeight(QFont::Bold);

    numberFormat_.setForeground(QColor(205, 92, 92));      // 印度红 数字

    stringFormat_.setForeground(QColor(34, 139, 34));      // 森林绿 字符串

    commentFormat_.setForeground(QColor(128, 128, 128));   // 灰色 注释
    commentFormat_.setFontItalic(true);

    functionFormat_.setForeground(QColor(0, 0, 205));      // 中蓝 函数调用名

    operatorFormat_.setForeground(QColor(139, 0, 0));      // 暗红 运算符

    // ========== 关键字（Vortex lexer 中 Kw_*） ==========
    const QStringList keywordPatterns = {
        QStringLiteral("\\bdef\\b"),
        QStringLiteral("\\bconst\\b"),
        QStringLiteral("\\bdel\\b"),
        QStringLiteral("\\bif\\b"),
        QStringLiteral("\\belse\\b"),
        QStringLiteral("\\belif\\b"),
        QStringLiteral("\\bfor\\b"),
        QStringLiteral("\\bwhile\\b"),
        QStringLiteral("\\bbreak\\b"),
        QStringLiteral("\\bcontinue\\b"),
        QStringLiteral("\\breturn\\b"),
        QStringLiteral("\\blambda\\b"),
        QStringLiteral("\\bimport\\b"),
        QStringLiteral("\\bfrom\\b"),
        QStringLiteral("\\bas\\b"),
        QStringLiteral("\\bin\\b"),
        QStringLiteral("\\bis\\b"),
        QStringLiteral("\\bglobal\\b"),
        QStringLiteral("\\btry\\b"),
        QStringLiteral("\\bcatch\\b"),
        QStringLiteral("\\bfinally\\b"),
        QStringLiteral("\\band\\b"),
        QStringLiteral("\\bor\\b"),
        QStringLiteral("\\bnot\\b"),
    };
    for (const QString &pattern : keywordPatterns) {
        rule.pattern = QRegularExpression(pattern);
        rule.format  = keywordFormat_;
        highlightingRules_.append(rule);
    }

    // ========== 类型关键字（Typ_*） ==========
    const QStringList typePatterns = {
        QStringLiteral("\\bint\\b"),
        QStringLiteral("\\buint\\b"),
        QStringLiteral("\\bshort\\b"),
        QStringLiteral("\\bushort\\b"),
        QStringLiteral("\\blong\\b"),
        QStringLiteral("\\bulong\\b"),
        QStringLiteral("\\bfloat\\b"),
        QStringLiteral("\\bdouble\\b"),
        QStringLiteral("\\bchar\\b"),
        QStringLiteral("\\bunichar\\b"),
        QStringLiteral("\\bmemadr\\b"),
        QStringLiteral("\\bbool\\b"),
        QStringLiteral("\\bstr\\b"),
        QStringLiteral("\\bunistr\\b"),
        QStringLiteral("\\bbin\\b"),
        QStringLiteral("\\blist\\b"),
        QStringLiteral("\\bstack\\b"),
        QStringLiteral("\\bqueue\\b"),
        QStringLiteral("\\bset\\b"),
        QStringLiteral("\\bundset\\b"),
        QStringLiteral("\\bdict\\b"),
        QStringLiteral("\\bpair\\b"),
        QStringLiteral("\\btuple\\b"),
        QStringLiteral("\\bobject\\b"),
        QStringLiteral("\\bfunction\\b"),
    };
    for (const QString &pattern : typePatterns) {
        rule.pattern = QRegularExpression(pattern);
        rule.format  = typeFormat_;
        highlightingRules_.append(rule);
    }

    // ========== true / false / none ==========
    const QStringList literalPatterns = {
        QStringLiteral("\\btrue\\b"),
        QStringLiteral("\\bfalse\\b"),
        QStringLiteral("\\bnone\\b"),
        QStringLiteral("\\bTrue\\b"),
        QStringLiteral("\\bFalse\\b"),
        QStringLiteral("\\bNone\\b"),
    };
    for (const QString &pattern : literalPatterns) {
        rule.pattern = QRegularExpression(pattern);
        rule.format  = literalFormat_;
        highlightingRules_.append(rule);
    }

    // ========== 数字：整数 / 浮点 / 十六进制 ==========
    rule.pattern = QRegularExpression(QStringLiteral("\\b0x[0-9A-Fa-f]+\\b"));
    rule.format  = numberFormat_;
    highlightingRules_.append(rule);

    rule.pattern = QRegularExpression(QStringLiteral("\\b[0-9]+\\.[0-9]+([eE][+-]?[0-9]+)?\\b"));
    rule.format  = numberFormat_;
    highlightingRules_.append(rule);

    rule.pattern = QRegularExpression(QStringLiteral("\\b[0-9]+\\b"));
    rule.format  = numberFormat_;
    highlightingRules_.append(rule);

    // ========== 运算符（常见的几类；注意顺序避免吞掉 == 等） ==========
    rule.pattern = QRegularExpression(QStringLiteral("[\\+\\-\\*\\/%=<>!&|^~?:]+"));
    rule.format  = operatorFormat_;
    highlightingRules_.append(rule);

    // ========== 函数调用名：标识符紧跟 '(' ==========
    rule.pattern = QRegularExpression(QStringLiteral("\\b([A-Za-z_][A-Za-z0-9_]*)\\s*\\("));
    rule.format  = functionFormat_;
    highlightingRules_.append(rule);

    // ========== 单行注释：# 到行尾（最后处理，避免被前面规则抢颜色） ==========
    rule.pattern = QRegularExpression(QStringLiteral("#[^\n]*"));
    rule.format  = commentFormat_;
    highlightingRules_.append(rule);

    // ========== 字符串（单行版，优先匹配；多行下面单独处理） ==========
    // "xxx" 或 'xxx'，支持简单转义
    rule.pattern = QRegularExpression(QStringLiteral("\"(?:[^\"\\\\]|\\\\.)*\""));
    rule.format  = stringFormat_;
    highlightingRules_.append(rule);

    rule.pattern = QRegularExpression(QStringLiteral("'(?:[^'\\\\]|\\\\.)*'"));
    rule.format  = stringFormat_;
    highlightingRules_.append(rule);
}

void SyntaxHighlighter::highlightBlock(const QString &text)
{
    // 1. 基础：逐规则匹配
    for (const HighlightingRule &rule : highlightingRules_) {
        QRegularExpressionMatchIterator it = rule.pattern.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            // 对函数名规则：只给第一个捕获组上色（括号前的名称），不给 '(' 上色
            int start, len;
            if (rule.format == functionFormat_ && match.lastCapturedIndex() >= 1) {
                start = match.capturedStart(1);
                len   = match.capturedLength(1);
            } else {
                start = match.capturedStart();
                len   = match.capturedLength();
            }
            setFormat(start, len, rule.format);
        }
    }
}
