#include "CodeEditor.h"
#include "SyntaxHighlighter.h"

#include <QPainter>
#include <QTextBlock>
#include <QTextDocument>
#include <QKeyEvent>
#include <QFont>
#include <QFontDatabase>
#include <QVector>
#include <QStringListModel>
#include <QAbstractItemView>
#include <QScrollBar>

static bool isOpenBracket(QChar c)  { return c == '(' || c == '[' || c == '{'; }
static bool isCloseBracket(QChar c) { return c == ')' || c == ']' || c == '}'; }
static QChar mateOf(QChar c)
{
    switch (c.unicode()) {
    case '(': return ')'; case ')': return '(';
    case '[': return ']'; case ']': return '[';
    case '{': return '}'; case '}': return '{';
    }
    return QChar();
}

// 从锚点字符 b 出发，沿 forward 方向寻找匹配括号的位置；未找到返回 -1
static int findMatchingBracket(QTextDocument *doc, int pos, QChar b, bool forward)
{
    const QChar target = mateOf(b);
    int depth = 0;
    const int len = doc->characterCount();
    int i = forward ? pos + 1 : pos - 1;
    while (forward ? i < len : i >= 0) {
        QChar c = doc->characterAt(i);
        if (c == b) {
            ++depth;
        } else if (c == target) {
            if (depth == 0) return i;
            --depth;
        }
        i += forward ? 1 : -1;
    }
    return -1;
}

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

    // 内置关键词/标识符自动补全（仿 IDLE）
    static const char* words[] = {
        "import", "from", "as", "def", "fn", "func", "return", "if", "elif", "else",
        "while", "for", "in", "break", "continue", "class", "var", "int", "float",
        "bool", "str", "string", "char", "object", "list", "dict", "set", "tuple",
        "try", "except", "catch", "finally", "throw", "new", "true", "false",
        "None", "and", "or", "not", "const", "let", "print", "println", "len",
        "range", "input", "sum", "prod", "int()", "float()", "str()", "bool()",
        // 模块成员
        "text.upper", "text.lower", "text.strip", "text.lstrip", "text.rstrip",
        "text.replace", "text.split", "text.find", "text.count", "text.repeat",
        "text.join", "text.format", "text.len", "text.slice", "text.startswith",
        "text.endswith", "text.contains",
        "math.sqrt", "math.pow", "math.abs", "math.floor", "math.ceil", "math.round",
        "math.log", "math.exp", "math.sin", "math.cos", "math.tan", "math.pi",
        "math.min", "math.max", "math.isclose", "math.isfinite", "math.factorial",
        "os.getenv", "os.setenv", "os.cwd", "os.chdir", "os.pid", "os.platform",
        "os.path_join", "os.basename", "os.dirname", "os.argc", "os.arg",
        "file.read", "file.write", "file.append", "file.exists", "file.remove",
        "file.rename", "file.size", "file.isdir", "file.isfile", "file.mkdir",
        "file.listdir",
        "regex.match", "regex.search", "regex.find", "regex.replace", "regex.count",
        "regex.escape", "regex.valid",
        "random.seed", "random.random", "random.randint", "random.uniform",
        "time.time", "time.sleep", "datetime.now", "json.parse",
        "base64.encode", "base64.decode", "hash.md5", "hash.sha1", "hash.sha256",
    };
    QStringList modelWords;
    for (const char* w : words) modelWords << QString::fromLatin1(w);
    setCompleter(new QCompleter(modelWords, this));
}

int CodeEditor::lineNumberAreaWidth()
{
    int digits = 1;
    int max = qMax(1, blockCount());
    while (max >= 10) {
        max /= 10;
        ++digits;
    }
    const int space = foldingGutterWidth() + 8 + digits * fontMetrics().horizontalAdvance(QLatin1Char('9'));
    return space;
}

// ==================== 缩进代码折叠 ====================
int CodeEditor::indentWidthOf(const QString &text) const
{
    int i = 0;
    while (i < text.size() && text[i] == QLatin1Char(' ')) ++i;
    return i;
}

bool CodeEditor::isFoldHeader(const QTextBlock &block) const
{
    if (!block.isValid()) return false;
    const int indent = indentWidthOf(block.text());
    QTextBlock b = block.next();
    while (b.isValid()) {
        const QString t = b.text();
        if (!t.trimmed().isEmpty()) {
            return indentWidthOf(t) > indent;
        }
        b = b.next();
    }
    return false;
}

int CodeEditor::foldEndLine(int headerLine) const
{
    QTextBlock hb = document()->findBlockByNumber(headerLine);
    if (!hb.isValid()) return headerLine;
    const int indent = indentWidthOf(hb.text());
    QTextBlock b = hb.next();
    int last = headerLine;
    while (b.isValid()) {
        const QString t = b.text();
        if (!t.trimmed().isEmpty()) {
            if (indentWidthOf(t) <= indent) break;
        }
        last = b.blockNumber();
        b = b.next();
    }
    return last;
}

bool CodeEditor::isBlockFolded(const QTextBlock &block) const
{
    return block.isValid() && folded_.contains(block.blockNumber());
}

void CodeEditor::setBlockFolded(const QTextBlock &block, bool folded)
{
    if (!block.isValid()) return;
    const int line = block.blockNumber();
    if (folded)
        folded_.insert(line);
    else
        folded_.remove(line);
    applyFolding();
}

bool CodeEditor::toggleFoldAtY(int y)
{
    // 由 lineNumberArea 相对坐标 y 反算可见块，命中可折叠头即切换折叠
    QTextBlock block = firstVisibleBlock();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    while (block.isValid()) {
        const int bottom = top + qRound(blockBoundingRect(block).height());
        if (y >= top && y < bottom) {
            if (isFoldHeader(block)) {
                setBlockFolded(block, !folded_.contains(block.blockNumber()));
                return true;
            }
            return false;
        }
        block = block.next();
        top = bottom;
    }
    return false;
}

void CodeEditor::applyFolding()
{
    QTextDocument *doc = document();
    const int n = doc->blockCount();
    QVector<bool> hidden(n, false);
    QList<int> lines = folded_.values();
    for (int h : lines) {
        const int end = foldEndLine(h);
        for (int i = h + 1; i <= end && i < n; ++i) hidden[i] = true;
    }
    QTextBlock b = doc->begin();
    while (b.isValid()) {
        const int bn = b.blockNumber();
        b.setVisible(!hidden[bn]);
        b = b.next();
    }
    // 折叠后行号区文本区需要重新布局与重绘
    viewport()->update();
    lineNumberArea_->update();
    updateLineNumberAreaWidth(0);
    update();
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

    // 括号匹配：光标紧邻的括号及其配对括号高亮
    const QTextCursor cur = textCursor();
    QTextDocument *doc = document();
    const int pos = cur.position();
    QChar chAt = doc->characterAt(pos);
    QChar chBefore = doc->characterAt(pos - 1);
    QChar anchor;
    if (isOpenBracket(chAt) || isCloseBracket(chAt))
        anchor = chAt;
    else if (isOpenBracket(chBefore) || isCloseBracket(chBefore))
        anchor = chBefore;

    if (!anchor.isNull()) {
        const bool forward = isOpenBracket(anchor);
        const int matchPos = findMatchingBracket(doc, pos - (anchor == chBefore ? 1 : 0), anchor, forward);
        if (matchPos >= 0) {
            const bool dark = palette().color(QPalette::Base).lightness() <= 128;
            const QColor fg = dark ? QColor(255, 220, 100) : QColor(0, 90, 220);
            const QColor bg = dark ? QColor(90, 70, 0) : QColor(255, 235, 130);

            QTextEdit::ExtraSelection a, m;
            a.format.setForeground(fg);
            a.format.setBackground(bg);
            a.cursor = textCursor();
            a.cursor.clearSelection();
            const int anchorPos = (anchor == chBefore) ? pos - 1 : pos;
            a.cursor.setPosition(anchorPos);
            a.cursor.setPosition(anchorPos + 1, QTextCursor::KeepAnchor);

            m.format.setForeground(fg);
            m.format.setBackground(bg);
            m.cursor = textCursor();
            m.cursor.clearSelection();
            m.cursor.setPosition(matchPos);
            m.cursor.setPosition(matchPos + 1, QTextCursor::KeepAnchor);

            extraSelections.append(a);
            extraSelections.append(m);
        }
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

    const int gutter = foldingGutterWidth();
    const bool dark = palette().color(QPalette::Base).lightness() <= 128;

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const QString number = QString::number(blockNumber + 1);
            painter.setPen(Qt::darkGray);
            painter.drawText(foldingGutterWidth() + 2, top, lineNumberArea_->width() - 4,
                             fontMetrics().height(), Qt::AlignRight, number);

            // 折叠标记：可折叠头绘制 [+]/[-] 小方块
            if (isFoldHeader(block)) {
                const int sq = 12;
                const int yCenter = (top + bottom) / 2;
                const QRect mr(gutter / 2 - sq / 2, yCenter - sq / 2, sq, sq);
                if (dark) {
                    painter.setBrush(QColor(90, 90, 100));
                    painter.setPen(QColor(160, 160, 170));
                } else {
                    painter.setBrush(QColor(225, 225, 225));
                    painter.setPen(QColor(150, 150, 150));
                }
                painter.drawRect(mr);
                // 横杠恒定显示；折叠时额外显示竖杠（[-] 与 [+]）
                const QColor fg = dark ? QColor(230, 230, 230) : QColor(90, 90, 90);
                painter.setPen(fg);
                const int cx = mr.center().x();
                const int cy = mr.center().y();
                painter.drawLine(cx - 3, cy, cx + 3, cy);
                if (isBlockFolded(block))
                    painter.drawLine(cx, cy - 3, cx, cy + 3);
            }
        }

        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++blockNumber;
    }
}

// ==================== 自动补全（仿 IDLE） ====================
void CodeEditor::setCompleter(QCompleter *c)
{
    if (completer_) {
        completer_->disconnect(this);
        delete completer_;
    }
    completer_ = c;
    if (!completer_)
        return;
    completer_->setWidget(this);
    completer_->setCompletionMode(QCompleter::PopupCompletion);
    completer_->setCaseSensitivity(Qt::CaseInsensitive);
    completer_->setWrapAround(false);
    QObject::connect(completer_, QOverload<const QString &>::of(&QCompleter::activated),
                     this, &CodeEditor::insertCompletion);
}

QString CodeEditor::completionPrefixFrom(const QTextCursor &cur) const
{
    QTextDocument *doc = document();
    int pos = cur.position();
    int start = pos;
    while (start > 0) {
        const QChar c = doc->characterAt(start - 1);
        if (c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('.'))
            --start;
        else
            break;
    }
    QTextCursor tc(doc);
    tc.setPosition(start);
    tc.setPosition(pos, QTextCursor::KeepAnchor);
    return tc.selectedText();
}

void CodeEditor::insertCompletion(const QString &completion)
{
    QTextCursor tc = textCursor();
    const int extra = completion.length() - completer_->completionPrefix().length();
    tc.movePosition(QTextCursor::Left);
    tc.movePosition(QTextCursor::EndOfWord);
    tc.insertText(completion.right(extra));
    setTextCursor(tc);
}

// 回车自动缩进（仿 IDLE 智能缩进）：
//  - 新行继承当前行的前置空格
//  - 若光标前的内容以 ':' 结尾（非注释行），新行再缩进一级
void CodeEditor::keyPressEvent(QKeyEvent *event)
{
    // 补全弹窗打开时，把 Enter/Return/Escape 交给补全器处理
    if (completer_ && completer_->popup()->isVisible()) {
        switch (event->key()) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Escape:
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            event->ignore();
            return;
        default:
            break;
        }
    }
    // 括号自动补全：输入 ( [ { 时插入成对括号并把光标放到中间
    const bool ctrl = event->modifiers() & Qt::ControlModifier;

    // Ctrl+/ ：切换选中行（或当前行）的行注释（前导 #）
    if (ctrl && event->key() == Qt::Key_Slash) {
        QTextCursor cur = textCursor();
        const int selStart = cur.selectionStart();
        const int selEnd = cur.selectionEnd();
        const QTextBlock first = document()->findBlock(selStart);
        QTextBlock last = document()->findBlock(selEnd);
        if (selEnd > selStart && cur.position() != selStart
            && document()->characterAt(selEnd - 1) == QLatin1Char('\n'))
            last = last.previous();  // 选中整行时不含末尾所在空行
        const int lastNum = last.blockNumber();
        bool allCommented = true;
        for (QTextBlock b = first; b.isValid() && b.blockNumber() <= lastNum; b = b.next()) {
            const QString lt = b.text();
            int i = 0;
            while (i < lt.size() && lt[i] == QLatin1Char(' ')) ++i;
            if (i >= lt.size() || lt[i] != QLatin1Char('#')) { allCommented = false; break; }
        }
        cur.beginEditBlock();
        QTextCursor tc(document());
        tc.beginEditBlock();
        for (QTextBlock b = first; b.isValid() && b.blockNumber() <= lastNum; b = b.next()) {
            const QString lt = b.text();
            tc.setPosition(b.position());
            if (allCommented) {
                tc.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 1);
                if (tc.selectedText() == QLatin1String("#")) tc.removeSelectedText();
                else if (tc.selectedText() == QLatin1String("# ") ||
                         (tc.selectedText().size() == 2 && tc.selectedText() == QLatin1String("#\t"))) {
                    tc.setPosition(b.position()); tc.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 2);
                    tc.removeSelectedText();
                }
            } else {
                tc.setPosition(b.position());
                tc.insertText(QStringLiteral("# "));
            }
        }
        tc.endEditBlock();
        cur.endEditBlock();
        event->accept();
        return;
    }

    // Ctrl+D ：复制当前行（或选中行块）到其下方
    if (ctrl && event->key() == Qt::Key_D) {
        QTextCursor cur = textCursor();
        QString block;
        const QTextBlock first = document()->findBlock(cur.selectionStart());
        const QTextBlock last = document()->findBlock(cur.selectionEnd());
        QTextBlock b = first;
        const int lastNum = last.blockNumber();
        block += first.text() + QLatin1Char('\n');
        for (b = first.next(); b.isValid() && b.blockNumber() <= lastNum; b = b.next())
            block += b.text() + QLatin1Char('\n');
        QTextCursor ins(document());
        ins.setPosition(last.position() + last.length());
        ins.insertText(block);
        event->accept();
        return;
    }

    const QString t = event->text();
    if (t == QStringLiteral("(") || t == QStringLiteral("[") || t == QStringLiteral("{")) {
        QChar open = t.at(0);
        QChar close = (open == QLatin1Char('(')) ? QLatin1Char(')')
                    : (open == QLatin1Char('[')) ? QLatin1Char(']') : QLatin1Char('}');
        QTextCursor cur = textCursor();
        cur.removeSelectedText();
        cur.beginEditBlock();
        cur.insertText(QString(open));
        cur.insertText(QString(close));
        cur.movePosition(QTextCursor::Left);
        cur.endEditBlock();
        setTextCursor(cur);
        event->accept();
        return;
    }
    // 跳过量：输入右括号而光标右侧正是同款右括号时，直接右移一格（不重复插入）
    if (t == QStringLiteral(")") || t == QStringLiteral("]") || t == QStringLiteral("}")) {
        QTextCursor cur = textCursor();
        if (document()->characterAt(cur.position()) == t.at(0)) {
            cur.movePosition(QTextCursor::Right);
            setTextCursor(cur);
            event->accept();
            return;
        }
    }

    // 退格：光标位于空括号对中间时，一次删除整对
    if (event->key() == Qt::Key_Backspace) {
        QTextCursor cur = textCursor();
        if (!cur.hasSelection() && !cur.atBlockStart()) {
            const QChar before = document()->characterAt(cur.position() - 1);
            const QChar after  = document()->characterAt(cur.position());
            if (isOpenBracket(before) && mateOf(before) == after) {
                cur.beginEditBlock();
                cur.deletePreviousChar();
                cur.deleteChar();
                cur.endEditBlock();
                event->accept();
                return;
            }
        }
    }

    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        QTextCursor cur = textCursor();
        const QString lineText = cur.block().text();
        int i = 0;
        while (i < lineText.size() && lineText[i].isSpace()) ++i;
        QString indent = lineText.left(i);

        const QString beforeCursor = lineText.left(cur.positionInBlock());
        const QString trimmedBefore = beforeCursor.trimmed();
        if ((trimmedBefore.endsWith(':') || trimmedBefore.endsWith('{'))
            && !trimmedBefore.startsWith('#'))
            indent += "    ";

        cur.beginEditBlock();
        cur.removeSelectedText();
        cur.insertText(QStringLiteral("\n") + indent);
        cur.endEditBlock();
        event->accept();
        return;
    }

    // 在仅空白的光标行输入 '}' 时，先减少一级缩进再插入（仿 IDLE 自动反缩进）
    if (event->text() == QStringLiteral("}")) {
        QTextCursor cur = textCursor();
        const QString before = cur.block().text().left(cur.positionInBlock());
        if (before.trimmed().isEmpty()) {
            const QString line = cur.block().text();
            int toRemove = 0;
            while (toRemove < 4 && toRemove < line.size() && line[toRemove] == ' ')
                ++toRemove;
            if (toRemove > 0) {
                const int blockPos = cur.block().position();
                cur.beginEditBlock();
                cur.setPosition(blockPos);
                cur.setPosition(blockPos + toRemove, QTextCursor::KeepAnchor);
                cur.removeSelectedText();
                cur.insertText(QStringLiteral("}"));
                cur.endEditBlock();
                event->accept();
                return;
            }
        }
    }

    if (event->key() == Qt::Key_Tab) {
        const bool shift = event->modifiers() & Qt::ShiftModifier;
        if (!shift) {
            // Tab 插入 4 个空格
            QTextCursor cur = textCursor();
            cur.removeSelectedText();
            cur.insertText(QStringLiteral("    "));
            event->accept();
            return;
        } else {
            // Shift+Tab：删除当前行开头最多 4 个空格（仅当光标在行首区域）
            QTextCursor cur = textCursor();
            if (cur.atBlockStart() || cur.columnNumber() <= 4) {
                const QString line = cur.block().text();
                int toRemove = 0;
                while (toRemove < 4 && toRemove < line.size() && line[toRemove] == ' ')
                    ++toRemove;
                if (toRemove > 0) {
                    cur.setPosition(cur.block().position());
                    cur.setPosition(cur.block().position() + toRemove, QTextCursor::KeepAnchor);
                    cur.removeSelectedText();
                }
                event->accept();
                return;
            }
        }
    }

    QPlainTextEdit::keyPressEvent(event);

    // 输入字符后，按光标前的标识符前缀自动弹出补全列表
    if (completer_ && !event->text().isEmpty()) {
        const QString prefix = completionPrefixFrom(textCursor());
        if (prefix.isEmpty() || prefix != completer_->completionPrefix()) {
            completer_->setCompletionPrefix(prefix);
            if (prefix.length() >= 1 && completer_->completionCount() > 0) {
                completer_->popup()->setCurrentIndex(completer_->completionModel()->index(0, 0));
                QRect cr = cursorRect();
                cr.setWidth(completer_->popup()->sizeHintForColumn(0)
                            + completer_->popup()->verticalScrollBar()->sizeHint().width());
                completer_->complete(cr);
            } else {
                completer_->popup()->hide();
            }
        }
    }
}
