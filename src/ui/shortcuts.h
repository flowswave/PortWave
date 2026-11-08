#pragma once
#include "ui/keyboard_shortcuts.h"
#include <QKeySequence>
#include <QSettings>
#include <QString>
#include <QVector>

namespace portwave {
// GUI-thread shortcut catalog shared by runtime bindings and the settings page.
// The ID doubles as the persisted key ("shortcuts/<id>"): an explicitly stored
// empty value means the operation is unbound, while a missing key uses fallback.
// Feature-level owners receive their sequences from the UI layer, so features do
// not read the shortcut namespace themselves.
enum class ShortcutScope { MainWindow, LogWindow, FindDialog, SendEditor };
struct ShortcutDefinition {
    const char* id;
    const char* label;
    ShortcutScope scope;
    QKeySequence fallback;
};
inline const QVector<ShortcutDefinition>& shortcutDefinitions() {
    static const QVector<ShortcutDefinition> definitions{
        {"search", "查找（串口缓存 / 日志）", ShortcutScope::MainWindow, QKeySequence(QKeySequence::Find)},
        {"log_search", "日志检索窗口", ShortcutScope::MainWindow, logSearchShortcut()},
        {"find_next", "查找下一处", ShortcutScope::FindDialog, QKeySequence(Qt::Key_F3)},
        {"find_previous", "查找上一处", ShortcutScope::FindDialog, QKeySequence(Qt::SHIFT | Qt::Key_F3)},
        {"log_refresh", "刷新当前日志", ShortcutScope::LogWindow, QKeySequence(Qt::Key_F5)},
        {"log_open", "打开日志文件", ShortcutScope::LogWindow, QKeySequence(QKeySequence::Open)},
        {"log_close", "关闭日志标签", ShortcutScope::LogWindow, QKeySequence(QKeySequence::Close)},
        {"send_draft", "发送草稿", ShortcutScope::SendEditor, QKeySequence(Qt::CTRL | Qt::Key_Return)},
        {"new_connection", "新建连接", ShortcutScope::MainWindow, {}},
        {"reconnect", "重连当前连接", ShortcutScope::MainWindow, {}},
        {"disconnect", "断开当前连接", ShortcutScope::MainWindow, {}},
        {"send_panel", "显示 / 隐藏发送窗口", ShortcutScope::MainWindow, {}},
        {"display_settings", "显示设置", ShortcutScope::MainWindow, {}},
        {"display_text", "文本显示", ShortcutScope::MainWindow, {}},
        {"display_hex", "HEX 显示", ShortcutScope::MainWindow, {}},
        {"display_mixed", "混合显示", ShortcutScope::MainWindow, {}},
        {"timestamp_display", "时间标签(界面)", ShortcutScope::MainWindow, {}},
        {"timestamp_log", "时间标签(log)", ShortcutScope::MainWindow, {}},
        {"pause_receive", "暂停接收", ShortcutScope::MainWindow, {}},
        {"pause_display", "暂停显示", ShortcutScope::MainWindow, {}},
        {"error_display", "错误显示", ShortcutScope::MainWindow, {}},
        {"auto_wrap", "自动换行", ShortcutScope::MainWindow, {}},
        {"clear", "清空", ShortcutScope::MainWindow, {}},
        {"clean_reconnect", "清屏并重连", ShortcutScope::MainWindow, QKeySequence(Qt::CTRL | Qt::Key_D)},
    };
    return definitions;
}
// GUI thread only. Reads the current profile without writing it.
inline QKeySequence configuredShortcut(const QString& id) {
    QSettings settings;
    const QString key = "shortcuts/" + id;
    if (settings.contains(key))
        return QKeySequence(settings.value(key).toString());
    for (const auto& definition : shortcutDefinitions())
        if (id == QLatin1String(definition.id))
            return definition.fallback;
    return {};
}
} // namespace portwave
