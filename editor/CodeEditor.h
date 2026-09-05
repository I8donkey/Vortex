#ifndef VORTEX_CODE_EDITOR_H
#define VORTEX_CODE_EDITOR_H

#include <QPlainTextEdit>
#include <QTextBlock>
#include <QSet>
#include <QMouseEvent>
#include <QCompleter>

QT_BEGIN_NAMESPACE
class QPaintEvent;
class QResizeEvent;
class QSize;
class QWidget;
class QKeyEvent;
class QMouseEvent;
QT_END_NAMESPACE

class LineNumberArea;

/**
 * @brief 带行号、当前行高亮、Tab宽度设置、括号匹配、自动缩进与缩进代码折叠的编辑器
 */
class CodeEditor : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit CodeEditor(QWidget *parent = nullptr);

    // 行号区绘制回调（由 LineNumberArea 调用）
    void lineNumberAreaPaintEvent(QPaintEvent *event);
    // 计算行号区所需宽度
    int lineNumberAreaWidth();

    // 文件路径（打开/保存后记录）
    QString filePath() const { return filePath_; }
    void setFilePath(const QString& path) { filePath_ = path; }

    // 内容是否已修改（未保存）
    bool isModifiedUntitled() const;

    // 折叠：根据 y 命中行号区的折叠标记开关对应块的折叠状态
    bool toggleFoldAtY(int y);
    // 当前块是否为可折叠的折叠头（有更缩进的子块）
    bool isFoldHeader(const QTextBlock &block) const;
    // 该折叠头当前是否处于折叠状态
    bool isBlockFolded(const QTextBlock &block) const;

    // 折叠并刷新（供折叠标记点击与按键调用）
    void setBlockFolded(const QTextBlock &block, bool folded);

    // 关键词/标识符自动补全（仿 IDLE）
    void setCompleter(QCompleter *c);
    QCompleter *completer() const { return completer_; }
    // 提取光标前的补全前缀
    QString completionPrefixFrom(const QTextCursor &cur) const;

private slots:
    // 选中补全项后把文本插入编辑器
    void insertCompletion(const QString &completion);

signals:
    // 标签页标题需要刷新（如文件名、修改状态改变）
    void tabTitleNeedsRefresh();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void highlightCurrentLine();
    void updateLineNumberArea(const QRect &rect, int dy);

private:
    // 缩进宽度（前导空格数）
    int indentWidthOf(const QString &text) const;
    // 折叠头 h 的折叠范围结束行号（首个不缩进/空行的前一非空行之后）
    int foldEndLine(int headerLine) const;
    // 依据 folded_ 重新计算所有块的可见性
    void applyFolding();
    // 折叠标记槽宽（行号区最左侧竖条）
    int foldingGutterWidth() const { return 18; }

    QWidget *lineNumberArea_;
    QString filePath_;  // 空字符串表示未命名的新文件
    QSet<int> folded_;  // 已折叠的折叠头块号
    QCompleter *completer_ = nullptr;  // 自动补全器（由 MainWindow 注入或内置）
};

/**
 * @brief 行号区控件，作为 CodeEditor 的视口左边缘子控件
 */
class LineNumberArea : public QWidget
{
public:
    explicit LineNumberArea(CodeEditor *editor)
        : QWidget(editor), codeEditor_(editor) {}

    QSize sizeHint() const override {
        return QSize(codeEditor_->lineNumberAreaWidth(), 0);
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        codeEditor_->lineNumberAreaPaintEvent(event);
    }
    // 点击折叠标记（最左侧小方块）时切换折叠
    void mousePressEvent(QMouseEvent *event) override {
        if ((event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton)
            && codeEditor_->toggleFoldAtY((int)event->position().y())) {
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

private:
    CodeEditor *codeEditor_;
};

#endif // VORTEX_CODE_EDITOR_H
