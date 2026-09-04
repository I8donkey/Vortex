// ============================================================
// gui_module.cpp — GUI 模块实现（基于 Qt 控件）
// ============================================================
#include "gui_module.h"
#include "interpreter.h"

#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QVariant>
#include <functional>
#include <sstream>

namespace vortex {

// namespace 级辅助：避免功能 lambda 捕获局部变量导致悬空引用
template <class T>
inline ValuePtr gui_wrap(const char* kind, T ptr) {
    auto res = std::make_shared<OpaqueResource>(kind, ptr);
    return Value::make_opaque(res);
}

inline QWidget* gui_to_widget(const ValuePtr& v) {
    if (!v->opaque_rep) return nullptr;
    auto& res = *v->opaque_rep;
    if (res.kind == "gui_window" || res.kind == "gui_widget") {
        auto gw = std::any_cast<std::shared_ptr<GuiWidget>>(res.payload);
        return gw->widget;
    }
    return nullptr;
}

inline std::shared_ptr<GuiWidget> gui_to_gw(const ValuePtr& v) {
    if (!v->opaque_rep) throw RuntimeError("gui: not a widget handle");
    return std::any_cast<std::shared_ptr<GuiWidget>>(v->opaque_rep->payload);
}

void register_gui_module(std::unordered_map<std::string, ValuePtr>& std_modules) {
    auto mod = Value::make_module();
    auto& u = *mod->module_rep;

    auto mk_fn = [](const std::string& mname, const std::string& name, size_t min_a, size_t max_a,
                    std::function<ValuePtr(const ValueVec&)> fn) {
        auto fv = std::make_shared<FunctionValue>();
        fv->name = name; fv->is_builtin = true;
        fv->builtin_fn = [mname, name, min_a, max_a, fn](const ValueVec& args, Environment&) -> ValuePtr {
            if (args.size() < min_a || (max_a != (size_t)-1 && args.size() > max_a))
                throw RuntimeError(mname + "." + name + " expects " +
                    std::to_string(min_a) + "~" + std::to_string(max_a) + " args, got " +
                    std::to_string(args.size()));
            return fn(args);
        };
        auto v = Value::make_none(); v->type = ValueType::Function; v->fn_rep = fv;
        return v;
    };

    auto add = [&](const std::string& n, size_t a0, size_t a1,
                   std::function<ValuePtr(const ValueVec&)> f) {
        u[n] = mk_fn("gui", n, a0, a1, std::move(f));
    };

    // ==== 窗口 ====
    add("window", 1, 3, [&](const ValueVec& a) -> ValuePtr {
        QString title = QString::fromStdString(a[0]->to_string());
        int w = a.size() >= 2 ? (int)value_to_int(a[1])->int_val : 400;
        int h = a.size() >= 3 ? (int)value_to_int(a[2])->int_val : 300;
        auto win = new QMainWindow();
        win->setWindowTitle(title);
        win->resize(w, h);
        auto central = new QWidget();
        win->setCentralWidget(central);
        auto gw = std::make_shared<GuiWidget>();
        gw->widget = central;  // 返回 central widget 以便添加布局/子控件
        // 把 QMainWindow 也存起来方便 show()
        win->setProperty("vortex_main", QVariant::fromValue(reinterpret_cast<qlonglong>(win)));
        return gui_wrap("gui_window", gw);
    });

    // ==== 控件创建 ====
    add("label", 2, 2, [&](const ValueVec& a) -> ValuePtr {
        QWidget* parent = gui_to_widget(a[0]);
        auto lbl = new QLabel(QString::fromStdString(a[1]->to_string()), parent);
        auto gw = std::make_shared<GuiWidget>();
        gw->widget = lbl;
        return gui_wrap("gui_widget", gw);
    });
    add("button", 2, 2, [&](const ValueVec& a) -> ValuePtr {
        QWidget* parent = gui_to_widget(a[0]);
        auto btn = new QPushButton(QString::fromStdString(a[1]->to_string()), parent);
        auto gw = std::make_shared<GuiWidget>();
        gw->widget = btn;
        return gui_wrap("gui_widget", gw);
    });
    add("input", 1, 2, [&](const ValueVec& a) -> ValuePtr {
        QWidget* parent = gui_to_widget(a[0]);
        auto le = new QLineEdit(parent);
        if (a.size() >= 2) le->setText(QString::fromStdString(a[1]->to_string()));
        auto gw = std::make_shared<GuiWidget>();
        gw->widget = le;
        return gui_wrap("gui_widget", gw);
    });

    // ==== 布局 ====
    add("layout_v", 1, 1, [&](const ValueVec& a) -> ValuePtr {
        QWidget* parent = gui_to_widget(a[0]);
        auto layout = new QVBoxLayout(parent);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(6);
        return Value::make_none();
    });
    add("layout_h", 1, 1, [&](const ValueVec& a) -> ValuePtr {
        QWidget* parent = gui_to_widget(a[0]);
        auto layout = new QHBoxLayout(parent);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(6);
        return Value::make_none();
    });
    add("add_widget", 2, 2, [&](const ValueVec& a) -> ValuePtr {
        QWidget* parent = gui_to_widget(a[0]);
        QWidget* child = gui_to_widget(a[1]);
        if (parent && child) {
            auto layout = parent->layout();
            if (layout) layout->addWidget(child);
        }
        return Value::make_none();
    });

    // ==== 属性 ====
    add("set_text", 2, 2, [&](const ValueVec& a) -> ValuePtr {
        auto gw = gui_to_gw(a[0]);
        if (gw && gw->widget) {
            auto btn = qobject_cast<QPushButton*>(gw->widget);
            if (btn) { btn->setText(QString::fromStdString(a[1]->to_string())); return Value::make_none(); }
            auto lbl = qobject_cast<QLabel*>(gw->widget);
            if (lbl) { lbl->setText(QString::fromStdString(a[1]->to_string())); return Value::make_none(); }
            auto le = qobject_cast<QLineEdit*>(gw->widget);
            if (le) { le->setText(QString::fromStdString(a[1]->to_string())); return Value::make_none(); }
        }
        return Value::make_none();
    });
    add("get_text", 1, 1, [&](const ValueVec& a) -> ValuePtr {
        auto gw = gui_to_gw(a[0]);
        if (gw && gw->widget) {
            auto btn = qobject_cast<QPushButton*>(gw->widget);
            if (btn) return Value::make_str(btn->text().toStdString());
            auto lbl = qobject_cast<QLabel*>(gw->widget);
            if (lbl) return Value::make_str(lbl->text().toStdString());
            auto le = qobject_cast<QLineEdit*>(gw->widget);
            if (le) return Value::make_str(le->text().toStdString());
        }
        return Value::make_str("");
    });

    // ==== 事件 ====
    add("on_click", 2, 2, [&](const ValueVec& a) -> ValuePtr {
        auto gw = gui_to_gw(a[0]);
        if (!gw || !gw->widget) throw RuntimeError("gui.on_click: not a widget");
        auto btn = qobject_cast<QPushButton*>(gw->widget);
        if (!btn) throw RuntimeError("gui.on_click: not a button");
        if (a[1]->type != ValueType::Function) throw RuntimeError("gui.on_click: expects a function");
        auto fn = a[1]->fn_rep;
        extern Interpreter* g_gui_active_interpreter;
        auto interp = g_gui_active_interpreter;
        if (!interp) throw RuntimeError("gui.on_click: no interpreter bound");
        // 用 lambda 捕获 fn 和 interp
        QObject::connect(btn, &QPushButton::clicked, btn, [interp, fn]() {
            try {
                ValueVec empty;
                if (fn->builtin_fn) {
                    Environment env(&interp->globals());
                    fn->builtin_fn(empty, env);
                } else if (fn->def) {
                    interp->call_user_function(fn.get(), empty);
                }
            } catch (...) {}
        });
        return Value::make_none();
    });

    // ==== 窗口操作 ====
    add("show", 1, 1, [&](const ValueVec& a) -> ValuePtr {
        auto gw = gui_to_gw(a[0]);
        if (gw && gw->widget) {
            // 找到 QMainWindow 父
            auto mainWin = qobject_cast<QMainWindow*>(gw->widget->parentWidget());
            if (mainWin) mainWin->show();
            else gw->widget->show();
        }
        return Value::make_none();
    });
    add("hide", 1, 1, [&](const ValueVec& a) -> ValuePtr {
        auto gw = gui_to_gw(a[0]);
        if (gw && gw->widget) gw->widget->hide();
        return Value::make_none();
    });

    // ==== 消息框 ====
    add("msgbox", 2, 3, [&](const ValueVec& a) -> ValuePtr {
        QString title = QString::fromStdString(a[0]->to_string());
        QString text  = QString::fromStdString(a[1]->to_string());
        QMessageBox::information(nullptr, title, text);
        return Value::make_none();
    });

    // ==== 事件循环 ====
    add("exec", 0, 0, [&](const ValueVec&) -> ValuePtr {
        if (qApp) qApp->exec();
        return Value::make_none();
    });
    add("quit", 0, 0, [&](const ValueVec&) -> ValuePtr {
        if (qApp) qApp->quit();
        return Value::make_none();
    });

    std_modules["gui"] = mod;
}

// 解释器绑定指针
Interpreter* g_gui_active_interpreter = nullptr;

} // namespace vortex
