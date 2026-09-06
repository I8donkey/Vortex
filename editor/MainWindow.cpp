#include "MainWindow.h"
#include "CodeEditor.h"

#include "interpreter.h"
#include "lexer.h"
#include "parser.h"
#include "ast.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QWidget>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QStatusBar>
#include <QLabel>
#include <QSplitter>
#include <QCloseEvent>
#include <QTextStream>
#include <QTextCursor>
#include <QIcon>
#include <QFontDatabase>
#include <QComboBox>
#include <QDebug>
#include <QProcess>
#include <QDir>
#include <QMenuBar>
#include <QMenu>
#include <QLineEdit>
#include <QKeyEvent>
#include <QDialog>
#include <QPushButton>
#include <QCheckBox>
#include <QInputDialog>
#include <QTextBlock>
#include <QStringList>
#include <functional>
#include <sstream>

// ============================================================
// 主题样式表
// ============================================================
static const char* kLightTheme = R"(
QWidget { background: #ffffff; color: #1a1a2e; }
QPlainTextEdit, QTextEdit { background: #ffffff; color: #1a1a2e; border: 1px solid #e0e0e0; }
QTabWidget::pane { border: 1px solid #d0d0d0; }
QTabBar::tab { background: #f0f0f0; color: #555; padding: 6px 12px; border: 1px solid #d0d0d0; }
QTabBar::tab:selected { background: #ffffff; color: #1a1a2e; }
QToolBar { background: #f5f5f5; border: none; spacing: 2px; }
QStatusBar { background: #f5f5f5; color: #555; }
QComboBox { background: #ffffff; border: 1px solid #ccc; padding: 2px 6px; }
QLabel { color: #1a1a2e; }
)";

static const char* kDarkTheme = R"(
QWidget { background: #1e1e2e; color: #cdd6f4; }
QPlainTextEdit, QTextEdit { background: #1a1a2a; color: #cdd6f4; border: 1px solid #313244; }
QTabWidget::pane { border: 1px solid #313244; }
QTabBar::tab { background: #181825; color: #6c7086; padding: 6px 12px; border: 1px solid #313244; }
QTabBar::tab:selected { background: #313244; color: #cdd6f4; }
QToolBar { background: #181825; border: none; spacing: 2px; }
QStatusBar { background: #181825; color: #6c7086; }
QComboBox { background: #313244; color: #cdd6f4; border: 1px solid #45475a; padding: 2px 6px; }
QLabel { color: #cdd6f4; }
QMessageBox { background: #1e1e2e; }
)";

// ============================================================
// MainWindow 实现
// ============================================================

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , settings_("VortexLang", "VortexEditor")
{
    loadSettings();
    setWindowTitle(QStringLiteral("Vortex Editor"));
    resize(1024, 720);

    createActions();
    createMenuBar();
    createToolBar();
    createStatusBar();
    createCentralWidget();

    applyTheme(theme_);
    retranslateUi();

    newFile();
}

MainWindow::~MainWindow() {
    saveSettings();
}

// ---------- 设置持久化 ----------

void MainWindow::saveSettings() {
    settings_.setValue("language", language_);
    settings_.setValue("theme", theme_);
}

void MainWindow::loadSettings() {
    language_ = settings_.value("language", 0).toInt();  // 默认 English
    theme_ = settings_.value("theme", 0).toInt();         // 默认 Light
}

// ---------- UI 构建 ----------

void MainWindow::createActions() {
    actNew_ = new QAction(this);
    actNew_->setShortcut(QKeySequence::New);
    connect(actNew_, &QAction::triggered, this, &MainWindow::newFile);

    actOpen_ = new QAction(this);
    actOpen_->setShortcut(QKeySequence::Open);
    connect(actOpen_, &QAction::triggered, this, &MainWindow::openFile);

    actSave_ = new QAction(this);
    actSave_->setShortcut(QKeySequence::Save);
    connect(actSave_, &QAction::triggered, this, &MainWindow::save);

    actSaveAs_ = new QAction(this);
    actSaveAs_->setShortcut(QKeySequence::SaveAs);
    connect(actSaveAs_, &QAction::triggered, this, &MainWindow::saveAs);

    actSaveAll_ = new QAction(this);
    connect(actSaveAll_, &QAction::triggered, this, &MainWindow::saveAll);

    actCompile_ = new QAction(this);
    actCompile_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_B));
    connect(actCompile_, &QAction::triggered, this, &MainWindow::compileCurrent);

    actRun_ = new QAction(this);
    actRun_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    connect(actRun_, &QAction::triggered, this, &MainWindow::runCurrent);

    actCompileRun_ = new QAction(this);
    actCompileRun_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F5));
    connect(actCompileRun_, &QAction::triggered, this, &MainWindow::compileRunCurrent);

    actDebug_ = new QAction(this);
    actDebug_->setShortcut(QKeySequence(Qt::Key_F5));
    connect(actDebug_, &QAction::triggered, this, &MainWindow::debugCurrent);

    actCloseTab_ = new QAction(this);
    actCloseTab_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_W));
    connect(actCloseTab_, &QAction::triggered, this, [this]() {
        if (tabWidget_->count() > 0)
            closeTab(tabWidget_->currentIndex());
    });

    actUndo_ = new QAction(this);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    actUndo_->setShortcut(QKeySequence(QKeyCombination(Qt::ControlModifier, Qt::Key_Z)));
    actRedo_ = new QAction(this);
    actRedo_->setShortcut(QKeySequence(QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_Z)));
#else
    actUndo_->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_Z));
    actRedo_ = new QAction(this);
    actRedo_->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_Z));
#endif
    actUndo_->setShortcutContext(Qt::WindowShortcut);
    actRedo_->setShortcutContext(Qt::WindowShortcut);
    connect(actUndo_, &QAction::triggered, this, &MainWindow::undo);
    connect(actRedo_, &QAction::triggered, this, &MainWindow::redo);

    actFind_ = new QAction(this);
    actFind_->setShortcut(QKeySequence::Find);
    connect(actFind_, &QAction::triggered, this, [this]() { showFindDialog(false); });

    actReplace_ = new QAction(this);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    actReplace_->setShortcut(QKeySequence(QKeyCombination(Qt::ControlModifier, Qt::Key_H)));
#else
    actReplace_->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_H));
#endif
    connect(actReplace_, &QAction::triggered, this, [this]() { showFindDialog(true); });

    actGoToLine_ = new QAction(this);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    actGoToLine_->setShortcut(QKeySequence(QKeyCombination(Qt::ControlModifier, Qt::Key_G)));
#else
    actGoToLine_->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_G));
#endif
    connect(actGoToLine_, &QAction::triggered, this, &MainWindow::goToLine);
}

void MainWindow::createToolBar() {
    toolBar_ = addToolBar(QStringLiteral("toolbar"));
    toolBar_->setMovable(false);
    toolBar_->setIconSize(QSize(20, 20));

    toolBar_->addAction(actNew_);
    toolBar_->addAction(actOpen_);
    toolBar_->addAction(actSave_);
    toolBar_->addAction(actSaveAs_);
    toolBar_->addAction(actSaveAll_);
    toolBar_->addSeparator();
    toolBar_->addAction(actUndo_);
    toolBar_->addAction(actRedo_);
    toolBar_->addSeparator();
    toolBar_->addAction(actCompile_);
    toolBar_->addAction(actRun_);
    toolBar_->addAction(actCompileRun_);
    toolBar_->addAction(actDebug_);
    toolBar_->addSeparator();
    toolBar_->addAction(actCloseTab_);

    // 语言和主题选择器
    toolBar_->addSeparator();
    langCombo_ = new QComboBox(toolBar_);
    langCombo_->addItem("English");
    langCombo_->addItem("中文");
    langCombo_->setCurrentIndex(language_);
    connect(langCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onLanguageChanged);
    toolBar_->addWidget(langCombo_);

    themeCombo_ = new QComboBox(toolBar_);
    themeCombo_->addItem("Light");
    themeCombo_->addItem("Dark");
    themeCombo_->setCurrentIndex(theme_);
    connect(themeCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onThemeChanged);
    toolBar_->addWidget(themeCombo_);
}

void MainWindow::createStatusBar() {
    statusMsg_ = new QLabel(this);
    statusMsg_->setMinimumWidth(300);
    statusPos_ = new QLabel(this);

    statusBar()->addWidget(statusMsg_, 1);
    statusBar()->addPermanentWidget(statusPos_);
}

void MainWindow::createMenuBar() {
    QMenu *mFile = menuBar()->addMenu(QStringLiteral("File"));
    mFile->addAction(actNew_);
    mFile->addAction(actOpen_);
    mFile->addSeparator();
    mFile->addAction(actSave_);
    mFile->addAction(actSaveAs_);
    mFile->addAction(actSaveAll_);
    mFile->addSeparator();
    mFile->addAction(actCloseTab_);

    QMenu *mEdit = menuBar()->addMenu(QStringLiteral("Edit"));
    mEdit->addAction(actUndo_);
    mEdit->addAction(actRedo_);
    mEdit->addSeparator();
    mEdit->addAction(actFind_);
    mEdit->addAction(actReplace_);
    mEdit->addSeparator();
    mEdit->addAction(actGoToLine_);

    QMenu *mRun = menuBar()->addMenu(QStringLiteral("Run"));
    mRun->addAction(actRun_);
    mRun->addAction(actCompile_);
    mRun->addAction(actCompileRun_);
    mRun->addAction(actDebug_);
}

void MainWindow::createShell() {
    // Shell 页：只读输出 + 底部输入行
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(2, 2, 2, 2);
    lay->setSpacing(2);

    shell_ = new QPlainTextEdit(page);
    shell_->setReadOnly(true);
    shell_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    shell_->setMaximumBlockCount(10000);

    auto *row = new QWidget(page);
    auto *hlay = new QHBoxLayout(row);
    hlay->setContentsMargins(0, 0, 0, 0);
    auto *prompt = new QLabel(QStringLiteral(">>>"), row);
    shellPrompt_ = prompt;
    shellInput_ = new QLineEdit(row);
    shellInput_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    hlay->addWidget(prompt);
    hlay->addWidget(shellInput_, 1);

    lay->addWidget(shell_, 1);
    lay->addWidget(row);

    shell_holder_ = page;
    shellInterp_ = std::make_unique<vortex::Interpreter>();
    shellInterp_->print_output = [this](const std::string& s) {
        shell_->insertPlainText(QString::fromUtf8(s.data(), (int)s.size()));
        shell_->ensureCursorVisible();
    };
    connect(shellInput_, &QLineEdit::returnPressed, this, &MainWindow::onShellSubmit);
    shell_->appendPlainText(QStringLiteral("Vortex Shell — type code and press Enter."));
    shell_->appendPlainText(QStringLiteral("Hint: run `print(2 + 3)` for example."));
}

void MainWindow::onShellSubmit() {
    const QString code = shellInput_->text();
    shellInput_->clear();

    // 空行 + 不在续行状态 → 直接忽略（不执行空语句）
    if (code.isEmpty() && !shellContinuation_)
        return;

    shellBuffer_ += code + QStringLiteral("\n");
    shell_->appendPlainText(QStringLiteral("%1 %2")
                            .arg(shellContinuation_ ? QStringLiteral("...") : QStringLiteral(">>>"),
                                 code));

    if (shellNeedsMore(shellBuffer_)) {
        shellContinuation_ = true;
        if (shellPrompt_) shellPrompt_->setText(QStringLiteral("..."));
        // 预填缩进（仿 IDLE 自动缩进）
        shellInput_->setText(shellContinuationIndent());
        shellInput_->setCursorPosition(shellInput_->text().size());
        return;
    }

    // 语句完整 → 执行
    if (shellInterp_) {
        const std::string src = shellBuffer_.toStdString();
        try {
            shellInterp_->exec_source(src);
        } catch (const std::exception &e) {
            QString what = QString::fromUtf8(e.what());
            shell_->appendPlainText(QStringLiteral("[Runtime] %1").arg(what));
        }
    }
    shellBuffer_.clear();
    shellContinuation_ = false;
    if (shellPrompt_) shellPrompt_->setText(QStringLiteral(">>>"));
}

// 判断累积源码是否已完成（可执行）：
//  - 以空行结尾 → 块终止，已完成
//  - 存在未闭合括号 / 未闭合字符串 → 继续
//  - 最后一行以 ':' 结尾（块头）→ 继续
bool MainWindow::shellNeedsMore(const QString &src) const
{
    const int nl = src.lastIndexOf(QLatin1Char('\n'));
    QString last = nl < 0 ? src : src.mid(nl + 1);
    if (last.trimmed().isEmpty()) return false; // 空行终止块

    QChar quote = QChar();
    bool comment = false;
    int depth = 0;
    for (int i = 0; i < src.size(); ++i) {
        const QChar c = src[i];
        if (comment) {
            if (c == QLatin1Char('\n')) comment = false;
            continue;
        }
        if (!quote.isNull()) {
            if (c == QLatin1Char('\\')) { ++i; continue; } // 跳过转义字符
            if (c == quote) quote = QChar();
            continue;
        }
        if (c == QLatin1Char('#')) { comment = true; continue; }
        if (c == QLatin1Char('\'') || c == QLatin1Char('"')) { quote = c; continue; }
        if (c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{')) ++depth;
        else if (c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}')) --depth;
    }
    if (!quote.isNull()) return true;   // 未闭合字符串
    if (depth > 0) return true;         // 未闭合括号
    if (last.trimmed().endsWith(QLatin1Char(':'))) return true; // 块头
    return false;
}

// 续行自动缩进：取缓冲区最后一个非空行；若以 ':' 结尾则再缩进一级
QString MainWindow::shellContinuationIndent() const
{
    QStringList lines = shellBuffer_.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    QString last;
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        if (!it->trimmed().isEmpty()) { last = *it; break; }
    }
    int i = 0;
    while (i < last.size() && last[i] == QLatin1Char(' ')) ++i;
    QString indent = last.left(i);
    if (last.trimmed().endsWith(QLatin1Char(':')) || last.trimmed().endsWith(QLatin1Char('{')))
        indent += QStringLiteral("    ");
    return indent;
}

void MainWindow::showFindDialog(bool replace) {
    auto *ed = currentEditor();
    if (!ed) return;

    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(replace
        ? (language_ == 1 ? QStringLiteral("查找与替换") : QStringLiteral("Find & Replace"))
        : (language_ == 1 ? QStringLiteral("查找") : QStringLiteral("Find")));
    auto *lay = new QVBoxLayout(dlg);

    auto *findEdit = new QLineEdit(dlg);
    auto *repEdit = new QLineEdit(dlg);
    if (ed->textCursor().hasSelection())
        findEdit->setText(ed->textCursor().selectedText());

    auto *hlayFind = new QHBoxLayout();
    hlayFind->addWidget(new QLabel(language_ == 1 ? QStringLiteral("查找:") : QStringLiteral("Find:"), dlg));
    hlayFind->addWidget(findEdit, 1);
    lay->addLayout(hlayFind);

    auto *hlayRep = new QHBoxLayout();
    hlayRep->addWidget(new QLabel(replace
        ? (language_ == 1 ? QStringLiteral("替换:") : QStringLiteral("Replace:"))
        : (language_ == 1 ? QStringLiteral("替换(可选):") : QStringLiteral("Replace (optional):")), dlg));
    hlayRep->addWidget(repEdit, 1);
    lay->addLayout(hlayRep);

    auto *caseBox = new QCheckBox(language_ == 1 ? QStringLiteral("区分大小写") : QStringLiteral("Match case"), dlg);
    lay->addWidget(caseBox);

    auto *btnLay = new QHBoxLayout();
    auto *btnFind = new QPushButton(language_ == 1 ? QStringLiteral("查找下一个") : QStringLiteral("Find Next"), dlg);
    auto *btnRep = new QPushButton(language_ == 1 ? QStringLiteral("替换") : QStringLiteral("Replace"), dlg);
    auto *btnRepAll = new QPushButton(language_ == 1 ? QStringLiteral("全部替换") : QStringLiteral("Replace All"), dlg);
    auto *btnClose = new QPushButton(language_ == 1 ? QStringLiteral("关闭") : QStringLiteral("Close"), dlg);
    btnLay->addWidget(btnFind);
    if (replace) { btnLay->addWidget(btnRep); btnLay->addWidget(btnRepAll); }
    btnLay->addWidget(btnClose);
    lay->addLayout(btnLay);

    auto findNext = [ed, findEdit, caseBox](bool forward) {
        QString needle = findEdit->text();
        if (needle.isEmpty()) return;
        QTextDocument::FindFlags flags;
        if (caseBox->isChecked()) flags |= QTextDocument::FindCaseSensitively;
        if (!forward) flags |= QTextDocument::FindBackward;
        QTextCursor base = ed->textCursor();
        if (forward && base.hasSelection()) base.setPosition(base.position());
        QTextCursor found = ed->document()->find(needle, base, flags);
        if (!found.isNull()) {
            ed->setTextCursor(found);
        } else {
            QTextCursor restart = QTextCursor(ed->document());
            if (forward) restart.movePosition(QTextCursor::Start);
            else restart.movePosition(QTextCursor::End);
            QTextCursor again = ed->document()->find(needle, restart, flags);
            if (!again.isNull()) ed->setTextCursor(again);
        }
    };

    // 高亮全部匹配（随输入/大小写选项实时刷新）
    auto highlightAll = [ed, findEdit, caseBox]() {
        QString needle = findEdit->text();
        QList<QTextEdit::ExtraSelection> sel;
        if (!needle.isEmpty()) {
            QTextDocument::FindFlags flags;
            if (caseBox->isChecked()) flags |= QTextDocument::FindCaseSensitively;
            QTextEdit::ExtraSelection base;
            base.format.setBackground(QColor(255, 234, 120));
            base.format.setForeground(QColor(20, 20, 20));
            QTextCursor cur = QTextCursor(ed->document());
            while (true) {
                QTextCursor f = ed->document()->find(needle, cur, flags);
                if (f.isNull()) break;
                QTextEdit::ExtraSelection s = base;
                s.cursor = f;
                sel.append(s);
                cur = f;
            }
        }
        ed->setExtraSelections(sel);
    };
    highlightAll();
    QObject::connect(findEdit, &QLineEdit::textChanged, dlg, highlightAll);
    QObject::connect(caseBox, &QCheckBox::toggled, dlg, [highlightAll](bool){ highlightAll(); });
    // 关闭对话框后清除高亮，并让编辑器恢复当前行高亮
    QObject::connect(dlg, &QObject::destroyed, ed, [ed]() {
        ed->setExtraSelections(QList<QTextEdit::ExtraSelection>());
        QTextCursor c = ed->textCursor();
        ed->setTextCursor(c);
    });

    QObject::connect(btnFind, &QPushButton::clicked, dlg, [findNext]() { findNext(true); });
    QObject::connect(btnRep, &QPushButton::clicked, dlg, [=]() {
        // 替换当前选中
        if (ed->textCursor().hasSelection()) {
            ed->textCursor().insertText(repEdit->text());
            ed->setFocus();
        }
        findNext(true);
    });
    QObject::connect(btnRepAll, &QPushButton::clicked, dlg, [ed, findEdit, repEdit, caseBox]() {
        QString needle = findEdit->text(), repl = repEdit->text();
        if (needle.isEmpty()) return;
        QTextDocument::FindFlags flags;
        if (caseBox->isChecked()) flags |= QTextDocument::FindCaseSensitively;
        int count = 0;
        QTextCursor cur = QTextCursor(ed->document());
        cur.beginEditBlock();
        while (true) {
            QTextCursor f = ed->document()->find(needle, cur, flags);
            if (f.isNull()) break;
            f.insertText(repl);
            cur = f;
            cur.setPosition(f.position());
            ++count;
        }
        cur.endEditBlock();
        if (count > 0) ed->setFocus();
    });
    QObject::connect(btnClose, &QPushButton::clicked, dlg, &QDialog::close);
    QObject::connect(findEdit, &QLineEdit::returnPressed, dlg, [findNext]() { findNext(true); });

    if (replace) repEdit->setFocus();
    else findEdit->setFocus();
    dlg->show();
}

void MainWindow::createCentralWidget() {
    auto *splitter = new QSplitter(Qt::Vertical, this);

    tabWidget_ = new QTabWidget(this);
    tabWidget_->setTabsClosable(true);
    tabWidget_->setMovable(true);
    tabWidget_->setDocumentMode(true);
    tabWidget_->setElideMode(Qt::ElideMiddle);

    auto *newTabBtn = new QToolButton(tabWidget_);
    newTabBtn->setText(QStringLiteral("+"));
    newTabBtn->setAutoRaise(true);
    newTabBtn->setFixedSize(QSize(22, 22));
    connect(newTabBtn, &QToolButton::clicked, this, &MainWindow::newFile);
    tabWidget_->setCornerWidget(newTabBtn, Qt::TopRightCorner);

    connect(tabWidget_, &QTabWidget::currentChanged, this, &MainWindow::currentTabChanged);
    connect(tabWidget_, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);

    output_ = new QPlainTextEdit(this);
    output_->setReadOnly(true);
    output_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    output_->setMaximumBlockCount(5000);

    // 底部两页：Output / Shell
    createShell();
    auto *bottomTabs = new QTabWidget(this);
    bottomTabs->setDocumentMode(true);
    bottomTabs->addTab(output_, QStringLiteral("Output"));
    bottomTabs->addTab(shell_holder_, QStringLiteral("Shell"));

    splitter->addWidget(tabWidget_);
    splitter->addWidget(bottomTabs);
    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({560, 160});

    setCentralWidget(splitter);
}

// ---------- 语言/主题切换 ----------

void MainWindow::goToLine() {
    CodeEditor *ed = currentEditor();
    if (!ed) return;
    const int total = ed->document()->blockCount();
    bool ok = false;
    const int line = QInputDialog::getInt(
        this,
        language_ == 1 ? QStringLiteral("跳转到行") : QStringLiteral("Go to Line"),
        language_ == 1 ? QStringLiteral("行号 (1-%1):").arg(total)
                       : QStringLiteral("Line number (1-%1):").arg(total),
        1, 1, total, 1, &ok);
    if (!ok) return;
    const int blockNumber = qBound(0, line - 1, total - 1);
    QTextBlock block = ed->document()->findBlockByNumber(blockNumber);
    QTextCursor c(block);
    c.movePosition(QTextCursor::StartOfBlock);
    ed->setTextCursor(c);
    ed->ensureCursorVisible();
    ed->setFocus();
}

void MainWindow::onLanguageChanged(int idx) {
    language_ = idx;
    retranslateUi();
    saveSettings();
}

void MainWindow::onThemeChanged(int idx) {
    theme_ = idx;
    applyTheme(idx);
    saveSettings();
}

void MainWindow::applyTheme(int idx) {
    if (idx == 1) {
        qApp->setStyleSheet(QString::fromLatin1(kDarkTheme));
    } else {
        qApp->setStyleSheet(QString::fromLatin1(kLightTheme));
    }
}

void MainWindow::retranslateUi() {
    // 仅 0=English / 1=中文
    if (language_ == 1) {
        // 中文
        actNew_->setText(tr("新建(&N)"));
        actOpen_->setText(tr("打开(&O)"));
        actSave_->setText(tr("保存(&S)"));
        actSaveAs_->setText(tr("另存为(&A)..."));
        actSaveAll_->setText(tr("全部保存"));
        actCompile_->setText(tr("编译(&B)"));
        actRun_->setText(tr("运行(&R)"));
        actCompileRun_->setText(tr("编译并运行(&N)"));
        actDebug_->setText(tr("调试(&D)"));
        actUndo_->setText(tr("撤销(&U)"));
        actRedo_->setText(tr("重做(&D)"));
        actCloseTab_->setText(tr("关闭标签"));
        actGoToLine_->setText(tr("跳转到行(&G)..."));

        actNew_->setStatusTip(tr("创建新的 Vortex 源文件"));
        actOpen_->setStatusTip(tr("打开已存在的 Vortex 源文件"));
        actSave_->setStatusTip(tr("保存当前文档"));
        actSaveAs_->setStatusTip(tr("将当前文档另存为新的文件"));
        actSaveAll_->setStatusTip(tr("保存所有打开的文档"));
        actCompile_->setStatusTip(tr("编译当前代码（检查词法/语法）(Ctrl+B)"));
        actRun_->setStatusTip(tr("用解释器运行当前 Vortex 代码 (Ctrl+R)"));
        actCompileRun_->setStatusTip(tr("编译为 exe 并运行 (Ctrl+F5)"));
        actDebug_->setStatusTip(tr("以 --debug 编译并用 gdb 调试 (F5)"));
        actUndo_->setStatusTip(tr("撤销上一步操作 (Ctrl+Z)"));
        actRedo_->setStatusTip(tr("重做被撤销的操作 (Ctrl+Shift+Z)"));
        actCloseTab_->setStatusTip(tr("关闭当前标签"));
        actGoToLine_->setStatusTip(tr("跳转到指定行号 (Ctrl+G)"));

        statusMsg_->setText(tr("就绪"));
        statusPos_->setText(tr("行: 1   列: 1"));
        output_->setPlaceholderText(tr("运行输出将显示在这里... (Ctrl+R 运行当前代码)"));
        toolBar_->window()->setWindowTitle(tr("Vortex 编辑器"));
    } else {
        // English
        actNew_->setText(QStringLiteral("New (&N)"));
        actOpen_->setText(QStringLiteral("Open (&O)"));
        actSave_->setText(QStringLiteral("Save (&S)"));
        actSaveAs_->setText(QStringLiteral("Save As... (&A)"));
        actSaveAll_->setText(QStringLiteral("Save All"));
        actCompile_->setText(QStringLiteral("Compile (&B)"));
        actRun_->setText(QStringLiteral("Run (&R)"));
        actCompileRun_->setText(QStringLiteral("Compile && Run (&N)"));
        actDebug_->setText(QStringLiteral("Debug (&D)"));
        actUndo_->setText(QStringLiteral("Undo (&U)"));
        actRedo_->setText(QStringLiteral("Redo (&D)"));
        actCloseTab_->setText(QStringLiteral("Close Tab"));
        actGoToLine_->setText(QStringLiteral("Go to Line (&G)..."));

        actNew_->setStatusTip(QStringLiteral("Create a new Vortex source file"));
        actOpen_->setStatusTip(QStringLiteral("Open an existing Vortex source file"));
        actSave_->setStatusTip(QStringLiteral("Save the current document"));
        actSaveAs_->setStatusTip(QStringLiteral("Save the current document as a new file"));
        actSaveAll_->setStatusTip(QStringLiteral("Save all open documents"));
        actCompile_->setStatusTip(QStringLiteral("Compile current code (lex/parse check) (Ctrl+B)"));
        actRun_->setStatusTip(QStringLiteral("Run current Vortex code via interpreter (Ctrl+R)"));
        actCompileRun_->setStatusTip(QStringLiteral("Compile to exe and run it (Ctrl+F5)"));
        actDebug_->setStatusTip(QStringLiteral("Compile with --debug and debug with gdb (F5)"));
        actUndo_->setStatusTip(QStringLiteral("Undo last action (Ctrl+Z)"));
        actRedo_->setStatusTip(QStringLiteral("Redo undone action (Ctrl+Shift+Z)"));
        actCloseTab_->setStatusTip(QStringLiteral("Close current tab"));
        actGoToLine_->setStatusTip(QStringLiteral("Jump to a line number (Ctrl+G)"));

        statusMsg_->setText(QStringLiteral("Ready"));
        statusPos_->setText(QStringLiteral("Ln: 1   Col: 1"));
        output_->setPlaceholderText(QStringLiteral("Run output will appear here... (Ctrl+R to run)"));
        setWindowTitle(QStringLiteral("Vortex Editor"));
    }
    // 刷新标签标题
    for (int i = 0; i < tabWidget_->count(); ++i)
        refreshTabTitle(i);
}

// ---------- 标签页辅助 ----------

CodeEditor *MainWindow::currentEditor() const {
    return qobject_cast<CodeEditor *>(tabWidget_->currentWidget());
}

CodeEditor *MainWindow::editorAt(int index) const {
    if (index < 0 || index >= tabWidget_->count()) return nullptr;
    return qobject_cast<CodeEditor *>(tabWidget_->widget(index));
}

int MainWindow::findTabByPath(const QString &filePath) const {
    if (filePath.isEmpty()) return -1;
    for (int i = 0; i < tabWidget_->count(); ++i) {
        auto *ed = editorAt(i);
        if (ed && ed->filePath() == filePath) return i;
    }
    return -1;
}

int MainWindow::addEditorTab(CodeEditor *editor, const QString &title) {
    QString t = title;
    if (t.isEmpty()) t = (language_ == 1) ? tr("未命名") : QStringLiteral("Untitled");
    const int idx = tabWidget_->addTab(editor, t);
    tabWidget_->setCurrentIndex(idx);
    connect(editor, &CodeEditor::tabTitleNeedsRefresh, this, [this, editor]() {
        int idx = tabWidget_->indexOf(editor);
        if (idx >= 0) refreshTabTitle(idx);
    });
    connect(editor, &CodeEditor::cursorPositionChanged, this, &MainWindow::cursorPositionChanged);
    connect(editor->document(), &QTextDocument::modificationChanged, this, &MainWindow::documentModifiedChanged);
    refreshTabTitle(idx);
    return idx;
}

void MainWindow::refreshTabTitle(int index) {
    auto *ed = editorAt(index);
    if (!ed) return;
    tabWidget_->setTabText(index, tabTitleFor(ed));
    tabWidget_->setTabToolTip(index, ed->filePath().isEmpty()
        ? (language_ == 1 ? tr("未保存的新文件") : QStringLiteral("Unsaved new file"))
        : QDir::toNativeSeparators(ed->filePath()));
    if (index == tabWidget_->currentIndex())
        setWindowTitle(QStringLiteral("%1 — Vortex Editor").arg(tabTitleFor(ed)));
}

QString MainWindow::tabTitleFor(CodeEditor *editor) const {
    const bool modified = editor->document()->isModified();
    QString base;
    if (editor->filePath().isEmpty()) {
        int untitledIdx = 1;
        for (int i = 0; i < tabWidget_->count(); ++i) {
            auto *other = editorAt(i);
            if (other == editor) break;
            if (other && other->filePath().isEmpty()) ++untitledIdx;
        }
        base = (language_ == 1) ? tr("未命名-%1").arg(untitledIdx)
                                : QStringLiteral("Untitled-%1").arg(untitledIdx);
    } else {
        base = QFileInfo(editor->filePath()).fileName();
    }
    return modified ? base + QStringLiteral(" *") : base;
}

void MainWindow::currentTabChanged(int index) {
    auto *ed = editorAt(index);
    if (!ed) {
        statusMsg_->setText(language_ == 1 ? tr("就绪") : QStringLiteral("Ready"));
        setWindowTitle(QStringLiteral("Vortex Editor"));
        return;
    }
    setWindowTitle(QStringLiteral("%1 — Vortex Editor").arg(tabTitleFor(ed)));
    cursorPositionChanged();
}

void MainWindow::documentModifiedChanged(bool) {
    auto *doc = qobject_cast<QTextDocument *>(sender());
    if (!doc) return;
    for (int i = 0; i < tabWidget_->count(); ++i) {
        auto *ed = editorAt(i);
        if (ed && ed->document() == doc) { refreshTabTitle(i); break; }
    }
}

void MainWindow::cursorPositionChanged() {
    auto *ed = currentEditor();
    if (!ed) {
        statusPos_->setText(language_ == 1 ? tr("行: -   列: -") : QStringLiteral("Ln: -   Col: -"));
        return;
    }
    const QTextCursor cursor = ed->textCursor();
    if (language_ == 1)
        statusPos_->setText(tr("行: %1   列: %2").arg(cursor.blockNumber() + 1).arg(cursor.columnNumber() + 1));
    else
        statusPos_->setText(QStringLiteral("Ln: %1   Col: %2").arg(cursor.blockNumber() + 1).arg(cursor.columnNumber() + 1));
}

// ---------- 文件操作 ----------

void MainWindow::newFile() {
    auto *ed = new CodeEditor(this);
    addEditorTab(ed);
    ed->setFocus();
    statusMsg_->setText(language_ == 1 ? tr("已创建新文件") : QStringLiteral("New file created"));
}

void MainWindow::openFile() {
    const QString path = QFileDialog::getOpenFileName(
        this,
        language_ == 1 ? tr("打开 Vortex 源文件") : QStringLiteral("Open Vortex Source File"),
        QString(),
        language_ == 1 ? tr("Vortex 源文件 (*.vt *.vtx *.vortex);;所有文件 (*)")
                       : QStringLiteral("Vortex Source (*.vt *.vtx *.vortex);;All Files (*)")
    );
    if (!path.isEmpty()) loadFile(path);
}

void MainWindow::openFiles(const QStringList &paths) {
    for (const QString &p : paths) loadFile(p);
}

bool MainWindow::loadFile(const QString &filePath) {
    const int exist = findTabByPath(filePath);
    if (exist >= 0) {
        tabWidget_->setCurrentIndex(exist);
        statusMsg_->setText((language_ == 1 ? tr("已打开: %1") : QStringLiteral("Opened: %1"))
                            .arg(QFileInfo(filePath).fileName()));
        return true;
    }
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this,
            language_ == 1 ? tr("打开失败") : QStringLiteral("Open Failed"),
            (language_ == 1 ? tr("无法读取文件 '%1':\n%2") : QStringLiteral("Cannot read file '%1':\n%2"))
                .arg(QDir::toNativeSeparators(filePath), file.errorString()));
        return false;
    }
    const QString content = QString::fromUtf8(file.readAll());
    auto *ed = new CodeEditor(this);
    ed->setPlainText(content);
    ed->setFilePath(filePath);
    ed->document()->setModified(false);
    addEditorTab(ed, QFileInfo(filePath).fileName());
    ed->setFocus();
    statusMsg_->setText((language_ == 1 ? tr("已加载: %1") : QStringLiteral("Loaded: %1"))
                        .arg(QDir::toNativeSeparators(filePath)));
    return true;
}

bool MainWindow::save() {
    auto *ed = currentEditor();
    if (!ed) return false;
    if (ed->filePath().isEmpty()) return saveAs();
    return saveFile(ed, ed->filePath());
}

bool MainWindow::saveAs() {
    auto *ed = currentEditor();
    if (!ed) return false;
    const QString defaultName = ed->filePath().isEmpty()
        ? QStringLiteral("untitled.vt") : ed->filePath();
    const QString path = QFileDialog::getSaveFileName(
        this,
        language_ == 1 ? tr("另存为") : QStringLiteral("Save As"),
        defaultName,
        language_ == 1 ? tr("Vortex 源文件 (*.vt *.vtx *.vortex);;所有文件 (*)")
                       : QStringLiteral("Vortex Source (*.vt *.vtx *.vortex);;All Files (*)")
    );
    if (path.isEmpty()) return false;
    return saveFile(ed, path);
}

bool MainWindow::saveAll() {
    bool ok = true;
    for (int i = 0; i < tabWidget_->count(); ++i) {
        auto *ed = editorAt(i);
        if (!ed || !ed->document()->isModified()) continue;
        if (ed->filePath().isEmpty()) { tabWidget_->setCurrentIndex(i); if (!saveAs()) { ok = false; continue; } }
        else if (!saveFile(ed, ed->filePath())) ok = false;
    }
    return ok;
}

bool MainWindow::saveFile(CodeEditor *editor, const QString &filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QMessageBox::warning(this,
            language_ == 1 ? tr("保存失败") : QStringLiteral("Save Failed"),
            (language_ == 1 ? tr("无法写入文件 '%1':\n%2") : QStringLiteral("Cannot write file '%1':\n%2"))
                .arg(QDir::toNativeSeparators(filePath), file.errorString()));
        return false;
    }
    file.write(editor->toPlainText().toUtf8());
    file.close();
    editor->setFilePath(filePath);
    editor->document()->setModified(false);
    const int idx = tabWidget_->indexOf(editor);
    if (idx >= 0) refreshTabTitle(idx);
    statusMsg_->setText((language_ == 1 ? tr("已保存: %1") : QStringLiteral("Saved: %1"))
                        .arg(QDir::toNativeSeparators(filePath)));
    return true;
}

bool MainWindow::maybeSave(CodeEditor *editor) {
    if (!editor || !editor->document()->isModified()) return true;
    const QString title = tabTitleFor(editor);
    const QMessageBox::StandardButton ret = QMessageBox::warning(
        this,
        language_ == 1 ? tr("未保存的修改") : QStringLiteral("Unsaved Changes"),
        (language_ == 1 ? tr("文档 '%1' 有未保存的修改。\n是否保存？") : QStringLiteral("Document '%1' has unsaved changes.\nSave?"))
            .arg(title),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel
    );
    switch (ret) {
    case QMessageBox::Save: {
        const int idx = tabWidget_->indexOf(editor);
        if (idx >= 0) tabWidget_->setCurrentIndex(idx);
        return editor->filePath().isEmpty() ? saveAs() : saveFile(editor, editor->filePath());
    }
    case QMessageBox::Discard: return true;
    default: return false;
    }
}

bool MainWindow::maybeSaveAll() {
    for (int i = tabWidget_->count() - 1; i >= 0; --i)
        if (!maybeSave(editorAt(i))) return false;
    return true;
}

void MainWindow::closeTab(int index) {
    auto *ed = editorAt(index);
    if (!ed) return;
    if (!maybeSave(ed)) return;
    tabWidget_->removeTab(index);
    ed->deleteLater();
    if (tabWidget_->count() == 0)
        setWindowTitle(QStringLiteral("Vortex Editor"));
}

// ---------- 撤销 / 重做 ----------

void MainWindow::undo() {
    auto *ed = currentEditor();
    if (!ed || !ed->isUndoRedoEnabled()) return;
    ed->undo();
    statusMsg_->setText(language_ == 1 ? tr("已撤销一步") : QStringLiteral("Undo"));
}

void MainWindow::redo() {
    auto *ed = currentEditor();
    if (!ed || !ed->isUndoRedoEnabled()) return;
    ed->redo();
    statusMsg_->setText(language_ == 1 ? tr("已重做一步") : QStringLiteral("Redo"));
}

// ---------- 输出面板 ----------

void MainWindow::appendOutput(const QString &text, bool isError) {
    if (text.isEmpty()) return;
    QTextCursor c = output_->textCursor();
    c.movePosition(QTextCursor::End);
    output_->setTextCursor(c);
    if (isError) {
        QTextCharFormat errFmt;
        errFmt.setForeground(QColor(theme_ == 1 ? "#f38ba8" : "#ff0000"));
        c.insertText(text, errFmt);
    } else {
        c.insertText(text);
    }
    output_->ensureCursorVisible();
}

void MainWindow::clearOutput() { output_->clear(); }

// ---------- 编译（仅检查词法/语法） ----------

bool MainWindow::ensureSaved() {
    auto *ed = currentEditor();
    if (!ed) return false;
    // 未保存的修改或从未建路径（空白新文件）→ 先存
    if (ed->document()->isModified()) return save();       // save() 无路径时自动弹另存为
    if (ed->filePath().isEmpty()) return saveAs();          // 空白新文件也需要真实路径才能编译
    return true;
}

void MainWindow::compileCurrent() {
    auto *ed = currentEditor();
    if (!ed) {
        statusMsg_->setText(language_ == 1 ? tr("没有可编译的文档") : QStringLiteral("No document to compile"));
        return;
    }
    if (!ensureSaved()) return;
    clearOutput();
    const QString name = ed->filePath().isEmpty() ? tabTitleFor(ed) : QFileInfo(ed->filePath()).fileName();
    appendOutput((language_ == 1 ? tr("=== 编译: %1 ===\n") : QStringLiteral("=== Compile: %1 ===\n")).arg(name));

    const std::string src = ed->toPlainText().toStdString();
    vortex::Lexer lex(src);
    auto toks = lex.tokenize();
    if (!lex.errors().empty()) {
        for (const auto &e : lex.errors())
            appendOutput(QString::fromStdString("[Lex] " + e + "\n"), true);
    }

    bool parseOk = false;
    if (lex.errors().empty()) {
        vortex::Parser parser(toks);
        try {
            auto prog = parser.parse_program();
            parseOk = prog != nullptr;
        } catch (const std::runtime_error &e) {
            appendOutput(QString::fromStdString(name.toStdString() + ": [Parse] " + e.what() + "\n"), true);
        }
        if (!parser.errors().empty()) {
            for (const auto &e : parser.errors())
                appendOutput(QString::fromStdString(name.toStdString() + ": [Parse] " + e + "\n"), true);
            parseOk = false;
        }
    }

    appendOutput((language_ == 1 ? tr("=== 编译: %1 ===\n") : QStringLiteral("=== Compile: %1 ===\n")).arg(name));
    if (parseOk) {
        // 生成本地 .vt 文件
        QString inPath = ed->filePath();
        if (inPath.isEmpty()) {
            inPath = QDir::temp().absoluteFilePath("vortex_" + QString::number(QCoreApplication::applicationPid()) + ".vt");
        }
        QFile f(inPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&f); ts << ed->toPlainText(); f.flush(); f.close();
        }

        // 找到 vortexcc.exe（同目录或 PATH）
        QString cc = QCoreApplication::applicationDirPath() + "/vortexcc.exe";
        if (!QFileInfo::exists(cc)) cc = QStringLiteral("vortexcc");

        // 输出 exe 路径：与源同目录同名 .exe
        QString outPath = inPath;
        if (outPath.endsWith(".vt", Qt::CaseInsensitive)) outPath.chop(3);
        outPath += ".exe";

        QProcess proc;
        proc.start(cc, {QStringLiteral("build"), inPath, QStringLiteral("-o"), outPath});
        proc.waitForFinished(-1);
        const QString pout = QString::fromUtf8(proc.readAllStandardOutput());
        const QString perr = QString::fromUtf8(proc.readAllStandardError());
        if (!pout.isEmpty()) appendOutput(pout, false);
        if (!perr.isEmpty()) appendOutput(perr, true);
        if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
            appendOutput((language_ == 1 ? tr("=== 已生成可执行文件: %1 ===\n")
                                         : QStringLiteral("=== EXE created: %1 ===\n")).arg(outPath), false);
            statusMsg_->setText(language_ == 1 ? tr("编译成功，已生成 exe") : QStringLiteral("Compiled to exe"));
        } else {
            statusMsg_->setText(language_ == 1 ? tr("编译为 exe 失败") : QStringLiteral("Compile to exe failed"));
        }
    }
    appendOutput((language_ == 1 ? tr("=== 编译结束: %1 ===\n") : QStringLiteral("=== Compile Done: %1 ===\n")).arg(name));
}

// ---------- 生成 exe 的公共逻辑 ----------

bool MainWindow::buildToExe(const QString &srcPath, QString &outPath, QString &diag, bool debug) {
    // 找到 vortexcc.exe（应用同目录或 PATH）
    QString cc = QCoreApplication::applicationDirPath() + "/vortexcc.exe";
    if (!QFileInfo::exists(cc)) cc = QStringLiteral("vortexcc");

    // 输出 exe 路径：与源同目录同名 .exe
    outPath = srcPath;
    if (outPath.endsWith(".vt", Qt::CaseInsensitive)) outPath.chop(3);
    outPath += ".exe";

    QStringList args = {QStringLiteral("build"), srcPath, QStringLiteral("-o"), outPath};
    if (debug) args << QStringLiteral("--debug");

    QProcess proc;
    proc.start(cc, args);
    if (!proc.waitForStarted()) {
        diag = (language_ == 1 ? tr("无法启动 vortexcc: %1") : QStringLiteral("Cannot start vortexcc: %1"))
                   .arg(cc);
        return false;
    }
    proc.waitForFinished(-1);
    const QString pout = QString::fromUtf8(proc.readAllStandardOutput());
    const QString perr = QString::fromUtf8(proc.readAllStandardError());
    diag = pout;
    if (!perr.isEmpty()) diag += perr;
    return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
}

// ---------- 编译并运行 ----------

void MainWindow::compileRunCurrent() {
    auto *ed = currentEditor();
    if (!ed) {
        statusMsg_->setText(language_ == 1 ? tr("没有可运行的文档") : QStringLiteral("No document to run"));
        return;
    }
    if (!ensureSaved()) return;
    clearOutput();
    const QString name = ed->filePath().isEmpty() ? tabTitleFor(ed) : QFileInfo(ed->filePath()).fileName();
    appendOutput((language_ == 1 ? tr("=== 编译并运行: %1 ===\n") : QStringLiteral("=== Compile & Run: %1 ===\n")).arg(name));

    QString inPath = ed->filePath();
    if (inPath.isEmpty())
        inPath = QDir::temp().absoluteFilePath("vortex_" + QString::number(QCoreApplication::applicationPid()) + ".vt");
    QFile f(inPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream ts(&f); ts << ed->toPlainText(); f.flush(); f.close();
    }

    QString outPath, diag;
    if (!buildToExe(inPath, outPath, diag, false)) {
        appendOutput(diag, true);
        statusMsg_->setText(language_ == 1 ? tr("编译失败") : QStringLiteral("Compile failed"));
        return;
    }
    appendOutput(diag, false);
    appendOutput((language_ == 1 ? tr("=== 运行: %1 ===\n") : QStringLiteral("=== Running: %1 ===\n")).arg(outPath));

    QProcess runProc;
    runProc.setWorkingDirectory(QFileInfo(outPath).absolutePath());
    runProc.start(outPath);
    if (!runProc.waitForStarted()) {
        appendOutput((language_ == 1 ? tr("无法启动程序: %1\n") : QStringLiteral("Cannot start program: %1\n")).arg(outPath), true);
        statusMsg_->setText(language_ == 1 ? tr("运行失败") : QStringLiteral("Run failed"));
        return;
    }
    runProc.waitForFinished(-1);
    const QString stdoutText = QString::fromUtf8(runProc.readAllStandardOutput());
    const QString stderrText = QString::fromUtf8(runProc.readAllStandardError());
    if (!stdoutText.isEmpty()) appendOutput(stdoutText, false);
    if (!stderrText.isEmpty()) appendOutput(stderrText, true);
    const int code = runProc.exitCode();
    appendOutput((language_ == 1 ? tr("=== 退出代码: %1 — %2 ===\n")
                                 : QStringLiteral("=== Exit code: %1 — %2 ===\n"))
                     .arg(code).arg(runProc.exitStatus() == QProcess::NormalExit ? QStringLiteral("normal")
                                                                                 : QStringLiteral("crash")));
    statusMsg_->setText(language_ == 1 ? tr("程序已退出，代码 %1").arg(code)
                                       : QStringLiteral("Program exited with code %1").arg(code));
}

// ---------- 调试（--debug 编译后用 gdb） ----------

void MainWindow::debugCurrent() {
    auto *ed = currentEditor();
    if (!ed) {
        statusMsg_->setText(language_ == 1 ? tr("没有可调试的文档") : QStringLiteral("No document to debug"));
        return;
    }
    if (!ensureSaved()) return;
    clearOutput();
    const QString name = ed->filePath().isEmpty() ? tabTitleFor(ed) : QFileInfo(ed->filePath()).fileName();
    appendOutput((language_ == 1 ? tr("=== 调试: %1 ===\n") : QStringLiteral("=== Debug: %1 ===\n")).arg(name));

    QString inPath = ed->filePath();
    if (inPath.isEmpty())
        inPath = QDir::temp().absoluteFilePath("vortex_dbg_" + QString::number(QCoreApplication::applicationPid()) + ".vt");
    QFile f(inPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream ts(&f); ts << ed->toPlainText(); f.flush(); f.close();
    }

    QString outPath, diag;
    if (!buildToExe(inPath, outPath, diag, true)) {
        appendOutput(diag, true);
        statusMsg_->setText(language_ == 1 ? tr("编译失败（--debug）") : QStringLiteral("Compile failed (--debug)"));
        return;
    }
    if (!diag.isEmpty()) appendOutput(diag, false);

    // 调试器：优先环境变量 VORTEX_DEBUGGER，否则从 PATH 找 lldb
    QString dbg = QString::fromLocal8Bit(qgetenv("VORTEX_DEBUGGER"));
    if (dbg.isEmpty()) dbg = QStringLiteral("lldb");

    // lldb 批处理：run → 崩溃则打印调用栈 → 退出
    QStringList args = {QStringLiteral("-b"), QStringLiteral("-o"), QStringLiteral("run"),
                        QStringLiteral("-o"), QStringLiteral("bt"),
                        QStringLiteral("--"), outPath};
    QProcess dbgProc;
    dbgProc.setWorkingDirectory(QFileInfo(outPath).absolutePath());
    dbgProc.start(dbg, args);
    if (!dbgProc.waitForStarted()) {
        appendOutput((language_ == 1 ? tr("无法启动 lldb: %1\n提示: 设置环境变量 VORTEX_DEBUGGER 指向 lldb.exe。\n")
                                     : QStringLiteral("Cannot start lldb: %1\nHint: set env VORTEX_DEBUGGER to lldb.exe.\n"))
                         .arg(dbg), true);
        statusMsg_->setText(language_ == 1 ? tr("调试失败") : QStringLiteral("Debug failed"));
        return;
    }
    dbgProc.waitForFinished(-1);
    const QString dout = QString::fromUtf8(dbgProc.readAllStandardOutput());
    const QString derr = QString::fromUtf8(dbgProc.readAllStandardError());
    if (!dout.isEmpty()) appendOutput(dout, false);
    if (!derr.isEmpty()) appendOutput(derr, true);
    statusMsg_->setText(language_ == 1 ? tr("调试结束") : QStringLiteral("Debug finished"));
}

void MainWindow::runCurrent() {
    auto *ed = currentEditor();
    if (!ed) {
        statusMsg_->setText(language_ == 1 ? tr("没有可运行的文档") : QStringLiteral("No document to run"));
        return;
    }
    if (!ensureSaved()) return;
    clearOutput();
    const QString name = ed->filePath().isEmpty() ? tabTitleFor(ed) : QFileInfo(ed->filePath()).fileName();
    appendOutput((language_ == 1 ? tr("=== 开始运行: %1 ===\n") : QStringLiteral("=== Running: %1 ===\n")).arg(name));

    const std::string srcStd = ed->toPlainText().toStdString();
    const std::string nameStd = name.toStdString();

    std::ostringstream outStream, errStream;

    vortex::Lexer lex(srcStd);
    auto toks = lex.tokenize();
    if (!lex.errors().empty())
        for (const auto &e : lex.errors()) errStream << "[Lex] " << e << "\n";

    std::unique_ptr<vortex::Program> prog;
    bool parseOk = false;
    if (lex.errors().empty()) {
        vortex::Parser parser(toks);
        try {
            prog = parser.parse_program();
            parseOk = prog != nullptr;
        } catch (const std::runtime_error &e) {
            errStream << nameStd << ": [Parse] " << e.what() << "\n";
        }
        if (!parser.errors().empty()) {
            for (const auto &e : parser.errors())
                errStream << nameStd << ": [Parse] " << e << "\n";
            parseOk = false;
        }
    }

    if (parseOk && prog) {
        vortex::Interpreter interp;
        interp.print_output = [&outStream](const std::string &s) { outStream << s; };
        interp.read_input = [&outStream, &errStream](const std::string &prompt) -> std::string {
            outStream << prompt;
            errStream << "\n[Note] Console input not available in GUI mode; returning empty string.\n";
            return {};
        };
        try {
            interp.run(*prog);
        } catch (const std::exception &e) {
            errStream << nameStd << ": [Runtime] " << e.what() << "\n";
        } catch (...) {
            errStream << nameStd << ": [Runtime] Unknown exception\n";
        }
    }

    const QString stdoutText = QString::fromStdString(outStream.str());
    const QString stderrText = QString::fromStdString(errStream.str());
    if (!stdoutText.isEmpty()) appendOutput(stdoutText, false);
    if (!stderrText.isEmpty()) appendOutput(stderrText, true);
    appendOutput((language_ == 1 ? tr("=== 运行结束: %1 ===\n") : QStringLiteral("=== Done: %1 ===\n")).arg(name));

    if (stderrText.isEmpty())
        statusMsg_->setText(language_ == 1 ? tr("运行完成") : QStringLiteral("Run finished"));
    else
        statusMsg_->setText(language_ == 1 ? tr("运行出现错误") : QStringLiteral("Errors occurred"));
}

// ---------- 关闭窗口 ----------

void MainWindow::closeEvent(QCloseEvent *event) {
    if (!maybeSaveAll()) { event->ignore(); return; }
    saveSettings();
    event->accept();
}
