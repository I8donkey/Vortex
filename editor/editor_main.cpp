/**
 * @file editor_main.cpp
 * @brief Vortex Editor 入口
 *
 * 用法：
 *   vortex_editor                     # 启动编辑器，带一个空白标签页
 *   vortex_editor file1.vt            # 启动并打开指定文件
 *   vortex_editor a.vt b.vt c.vt      # 启动并打开多个文件
 */

#include "MainWindow.h"

#include <QApplication>
#include <QStringList>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QApplication::setApplicationName(QStringLiteral("Vortex Editor"));
    QApplication::setApplicationDisplayName(QStringLiteral("Vortex Editor"));
    QApplication::setOrganizationName(QStringLiteral("Vortex Lang"));

    MainWindow win;
    win.show();

    // 收集命令行里除第一个（程序路径）以外的参数，作为待打开的文件
    const QStringList args = app.arguments();
    if (args.size() > 1) {
        QStringList files;
        for (int i = 1; i < args.size(); ++i)
            files.append(args.at(i));
        win.openFiles(files);
    }

    return app.exec();
}
