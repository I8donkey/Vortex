#ifndef VORTEX_MAIN_WINDOW_H
#define VORTEX_MAIN_WINDOW_H

#include <QMainWindow>
#include <QString>
#include <QSettings>
#include <memory>
#include "CodeEditor.h"
namespace vortex { class Interpreter; }

QT_BEGIN_NAMESPACE
class QAction;
class QToolBar;
class QTabWidget;
class QPlainTextEdit;
class QLineEdit;
class QLabel;
class QSplitter;
class QComboBox;
class QMenuBar;
class QMenu;
class CodeEditor;
QT_END_NAMESPACE

/**
 * @brief Vortex 编辑器主窗口
 *
 * 功能：
 *  - 多标签代码编辑 + 语法高亮
 *  - 编译（仅检查词法/语法）+ 运行
 *  - 撤销/重做 (Ctrl+Z / Ctrl+Shift+Z)
 *  - 双语言：English (默认) / 中文
 *  - 亮/暗主题切换
 *  - 语言和主题设置持久化
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void openFiles(const QStringList &paths);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void newFile();
    void openFile();
    bool save();
    bool saveAs();
    bool saveAll();
    void undo();
    void redo();
    void runCurrent();        // 用解释器运行
    void compileCurrent();    // 仅编译（vortexcc 生成 exe）
    void compileRunCurrent(); // 编译并运行生成的 exe
    void debugCurrent();      // --debug 编译后用 gdb 调试运行
    void currentTabChanged(int index);
    void closeTab(int index);
    void documentModifiedChanged(bool modified);
    void cursorPositionChanged();
    void onLanguageChanged(int idx);
    void onThemeChanged(int idx);
    void showFindDialog(bool replace);
    void goToLine();
    void onShellSubmit();

private:
    void createActions();
    void createMenuBar();
    void createToolBar();
    void createStatusBar();
    void createCentralWidget();
    void createShell();
    void retranslateUi();
    void applyTheme(int idx);
    void saveSettings();
    void loadSettings();

    CodeEditor *currentEditor() const;
    CodeEditor *editorAt(int index) const;
    int findTabByPath(const QString &filePath) const;
    int addEditorTab(CodeEditor *editor, const QString &title = QString());
    void refreshTabTitle(int index);
    QString tabTitleFor(CodeEditor *editor) const;

    bool loadFile(const QString &filePath);
    bool saveFile(CodeEditor *editor, const QString &filePath);
    bool maybeSave(CodeEditor *editor);
    bool maybeSaveAll();

    void appendOutput(const QString &text, bool isError = false);
    void clearOutput();
    // 用 vortexcc 把源文件编译成 exe；成功返回 true。outPath/diag 接收结果。
    bool buildToExe(const QString &srcPath, QString &outPath, QString &diag, bool debug = false);
    // 编译/运行/调试前确保当前文档已保存；未保存则弹另存为，取消返回 false。
    bool ensureSaved();
    bool shellNeedsMore(const QString &src) const;
    QString shellContinuationIndent() const;

    // ===== 成员 =====
    QToolBar    *toolBar_   = nullptr;
    QTabWidget  *tabWidget_ = nullptr;
    QPlainTextEdit *output_ = nullptr;
    QLabel      *statusPos_ = nullptr;
    QLabel      *statusMsg_ = nullptr;
    QComboBox   *langCombo_ = nullptr;
    QComboBox   *themeCombo_ = nullptr;
    QPlainTextEdit *shell_ = nullptr;
    QLineEdit  *shellInput_ = nullptr;
    QLabel     *shellPrompt_ = nullptr;
    QWidget    *shell_holder_ = nullptr;
    QString    shellBuffer_;        // 多行续行的累积源码
    bool       shellContinuation_ = false;
    std::unique_ptr<vortex::Interpreter> shellInterp_;

    QAction *actNew_      = nullptr;
    QAction *actOpen_     = nullptr;
    QAction *actSave_     = nullptr;
    QAction *actSaveAs_   = nullptr;
    QAction *actSaveAll_  = nullptr;
    QAction *actUndo_     = nullptr;
    QAction *actRedo_     = nullptr;
    QAction *actCompile_  = nullptr;
    QAction *actRun_      = nullptr;
    QAction *actCompileRun_ = nullptr;
    QAction *actDebug_     = nullptr;
    QAction *actCloseTab_ = nullptr;
    QAction *actFind_     = nullptr;
    QAction *actReplace_  = nullptr;
    QAction *actGoToLine_ = nullptr;

    int language_ = 0;  // 0=English, 1=中文
    int theme_ = 0;     // 0=Light, 1=Dark
    QSettings settings_;
};

#endif // VORTEX_MAIN_WINDOW_H
