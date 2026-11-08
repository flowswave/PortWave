#include "infrastructure/log_workspace_store.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>

namespace portwave {
QJsonObject LogWorkspaceStore::load() {
    const auto bytes = QSettings().value("logs/workspaces_v2").toByteArray();
    if (bytes.size() > 1024 * 1024)
        return {};
    const auto state = QJsonDocument::fromJson(bytes).object();
    if (state.value("version").toInt() != 2 || state.value("workspaces").toArray().size() > 128)
        return {};
    return state;
}
bool LogWorkspaceStore::save(const QJsonObject& state, QString& error) {
    const auto bytes = QJsonDocument(state).toJson(QJsonDocument::Compact);
    if (bytes.size() > 1024 * 1024 || state.value("workspaces").toArray().size() > 128) {
        error = "日志工作区状态超过保存上限";
        return false;
    }
    QSettings settings;
    settings.setValue("logs/workspaces_v2", bytes);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        error = "无法保存日志工作区设置";
        return false;
    }
    return true;
}
} // namespace portwave
