#ifndef VORTEX_SYNTAX_HIGHLIGHTER_H
#define VORTEX_SYNTAX_HIGHLIGHTER_H

#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QRegularExpression>
#include <QVector>

QT_BEGIN_NAMESPACE
class QTextDocument;
QT_END_NAMESPACE

/**
 * @brief Vortex 语言语法高亮器
 *
 * 支持的高亮类别：
 *  - 关键字（def / const / if / else / for / while / return / lambda / import ...）
 *  - 类型关键字（int / float / str / list / dict / bool ...）
 *  - 布尔/None 字面量
 *  - 运算符
 *  - 数字字面量（整数、浮点）
 *  - 字符串字面量（单/双引号，支持转义）
 *  - 单行注释（#）
 *  - 函数调用名
 */
class SyntaxHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT

public:
    explicit SyntaxHighlighter(QTextDocument *parent = nullptr);

protected:
    void highlightBlock(const QString &text) override;

private:
    struct HighlightingRule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };
    QVector<HighlightingRule> highlightingRules_;

    // 字符串字面量起始（多行字符串支持）
    QRegularExpression stringStartExpr_;
    QRegularExpression stringEndExpr_;

    // 各种格式
    QTextCharFormat keywordFormat_;
    QTextCharFormat typeFormat_;
    QTextCharFormat literalFormat_;   // true/false/none
    QTextCharFormat numberFormat_;
    QTextCharFormat stringFormat_;
    QTextCharFormat commentFormat_;
    QTextCharFormat functionFormat_;
    QTextCharFormat operatorFormat_;
};

#endif // VORTEX_SYNTAX_HIGHLIGHTER_H
