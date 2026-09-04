#include "CodeEditor.h"
#include "SyntaxHighlighter.h"

#include <QPainter>
#include <QTextBlock>
#include <QFont>
#include <QFontDatabase>

CodeEditor::CodeEditor(QWidget *parent)
    : QPlainTextEdit(parent)
{
    // 使用等宽字体
    const QFont fixedFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    setFont(fixedFont);

    // Tab 宽度：4 个空格
    const int tabStop = 4;
    QFontMetrics metrics(fixedFont);
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
    setTabStopDistance(tabStop * metrics.horizontalAdvance(QLatin1Char(' ')));
#else
    setTabStopWidth(tabStop * metrics.width(QLatin1Char(' ')));
#endif

    // 行号区
    lineNumberArea_ = new LineNumberArea(this);

    // 安装语法高亮器
    new SyntaxHighlighter(document());

    // 连接信号
    connect(this, &QPlainTextEdit::blockCountChanged,
            this, &CodeEditor::updateLineNumberAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest,
            this, &CodeEditor::updateLineNumberArea);
    connect(this, &QPlainTextEdit::cursorPositionChanged,
            this, &CodeEditor::highlightCurrentLine);
    connect(document(), &QTextDocument::modificationChanged,
            this, &CodeEditor::tabTitleNeedsRefresh);

    // 初始化
    updateLineNumberAreaWidth(0);
    highlightCurrentLine();
}

int CodeEditor::lineNumberAreaWidth()
{
    int digits = 1;
    int max = qMax(1, blockCount());
    while (max >= 10) {
        max /= 10;
        ++digits;
    }
    const int space = 10 + digits * fontMetrics().horizontalAdvance(QLatin1Char('9'));
    return space;
}

bool CodeEditor::isModifiedUntitled() const
{
    // 未命名 + 未保存的修改 = 需要提示保存
    return filePath_.isEmpty() && document()->isModified();
}

void CodeEditor::updateLineNumberAreaWidth(int /* newBlockCount */)
{
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void CodeEditor::updateLineNumberArea(const QRect &rect, int dy)
{
    if (dy)
        lineNumberArea_->scroll(0, dy);
    else
        lineNumberArea_->update(0, rect.y(), lineNumberArea_->width(), rect.height());

    if (rect.contains(viewport()->rect()))
        updateLineNumberAreaWidth(0);
}

void CodeEditor::resizeEvent(QResizeEvent *e)
{
    QPlainTextEdit::resizeEvent(e);

    const QRect cr = contentsRect();
    lineNumberArea_->setGeometry(
        QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height())
    );
}

void CodeEditor::highlightCurrentLine()
{
    QList<QTextEdit::ExtraSelection> extraSelections;

    if (!isReadOnly()) {
        QTextEdit::ExtraSelection selection;

        const QColor lineColor = QColor(245, 245, 230);   // 浅黄色当前行
        const QColor lineColorDark = QColor(40, 40, 55);  // 深色兜底

        selection.format.setBackground(palette().color(QPalette::Base).lightness() > 128
                                       ? lineColor
                                       : lineColorDark);
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
        selection.cursor = textCursor();
        selection.cursor.clearSelection();
        extraSelections.append(selection);
    }

    setExtraSelections(extraSelections);
}

void CodeEditor::lineNumberAreaPaintEvent(QPaintEvent *event)
{
    QPainter painter(lineNumberArea_);
    // 背景
    painter.fillRect(event->rect(), QColor(235, 235, 235));

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const QString number = QString::number(blockNumber + 1);
            painter.setPen(Qt::darkGray);
            painter.drawText(0, top, lineNumberArea_->width() - 4,
                             fontMetrics().height(), Qt::AlignRight, number);
        }

        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++blockNumber;
    }
}
