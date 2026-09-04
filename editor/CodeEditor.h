#ifndef VORTEX_CODE_EDITOR_H
#define VORTEX_CODE_EDITOR_H

#include <QPlainTextEdit>

QT_BEGIN_NAMESPACE
class QPaintEvent;
class QResizeEvent;
class QSize;
class QWidget;
QT_END_NAMESPACE

class LineNumberArea;

/**
 * @brief 带行号、当前行高亮、Tab宽度设置的代码编辑器控件
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

signals:
    // 标签页标题需要刷新（如文件名、修改状态改变）
    void tabTitleNeedsRefresh();

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void highlightCurrentLine();
    void updateLineNumberArea(const QRect &rect, int dy);

private:
    QWidget *lineNumberArea_;
    QString filePath_;  // 空字符串表示未命名的新文件
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

private:
    CodeEditor *codeEditor_;
};

#endif // VORTEX_CODE_EDITOR_H
