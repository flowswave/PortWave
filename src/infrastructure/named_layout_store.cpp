#include "infrastructure/named_layout_store.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

namespace portwave {
bool NamedLayoutStore::save(QSettings& settings, const QString& name, const NamedLayoutSnapshot& snapshot,
                            QString& error) {
    error.clear();
    if (name.trimmed().isEmpty() || name.contains('/') || name.contains('\\') ||
        snapshot.mainGeometry.isEmpty() || snapshot.windows.size() > 128) {
        error = "布局名称或窗口快照无效";
        return false;
    }
    QJsonArray windows;
    for (const auto& window : snapshot.windows)
        windows.push_back(QJsonObject{{"id", window.connectionId},
                                      {"geometry", QString::fromLatin1(window.geometry.toBase64())},
                                      {"maximized", window.maximized}});
    const QJsonObject value{{"geometry", QString::fromLatin1(snapshot.mainGeometry.toBase64())},
                            {"active", snapshot.activeConnectionId},
                            {"windows", windows}};
    settings.setValue("layouts/saved/" + name, QJsonDocument(value).toJson(QJsonDocument::Compact));
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        error = "布局设置写入失败";
        return false;
    }
    return true;
}

std::optional<NamedLayoutSnapshot> NamedLayoutStore::load(const QSettings& settings, const QString& name) {
    const auto bytes = settings.value("layouts/saved/" + name).toByteArray();
    if (bytes.isEmpty())
        return std::nullopt;
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return NamedLayoutSnapshot{bytes, {}, {}, true};

    const auto object = document.object();
    if (!object.value("geometry").isString() || !object.value("active").isString() ||
        !object.value("windows").isArray() || object.value("windows").toArray().size() > 128)
        return std::nullopt;
    NamedLayoutSnapshot result;
    result.mainGeometry = QByteArray::fromBase64(object.value("geometry").toString().toLatin1());
    result.activeConnectionId = object.value("active").toString();
    for (const auto& value : object.value("windows").toArray()) {
        if (!value.isObject())
            return std::nullopt;
        const auto row = value.toObject();
        if (!row.value("id").isString() || !row.value("geometry").isString() ||
            !row.value("maximized").isBool())
            return std::nullopt;
        const auto id = row.value("id").toString();
        const auto geometry = QByteArray::fromBase64(row.value("geometry").toString().toLatin1());
        if (id.isEmpty() || geometry.isEmpty() ||
            std::any_of(result.windows.cbegin(), result.windows.cend(),
                        [&id](const auto& window) { return window.connectionId == id; }))
            return std::nullopt;
        result.windows.push_back({id, geometry, row.value("maximized").toBool()});
    }
    if (result.mainGeometry.isEmpty())
        return std::nullopt;
    return result;
}
} // namespace portwave
