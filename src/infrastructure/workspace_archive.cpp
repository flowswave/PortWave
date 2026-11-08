#include "infrastructure/workspace_archive.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QVariant>
#include <limits>
#include <miniz.h>
#include <miniz_zip.h>

namespace portwave {
namespace {
constexpr qsizetype kMaximumArchiveBytes = 64 * 1024 * 1024;
constexpr char kManifestName[] = "workspace.json";

QJsonValue encode(const QVariant& value, bool& ok) {
    if (value.metaType().id() == QMetaType::QByteArray)
        return QJsonObject{{"$qbytes", QString::fromLatin1(value.toByteArray().toBase64())}};
    const auto json = QJsonValue::fromVariant(value);
    if (json.isUndefined())
        ok = false;
    return json;
}

QVariant decode(const QJsonValue& value, bool& ok) {
    if (value.isObject()) {
        const auto object = value.toObject();
        if (object.size() == 1 && object.contains("$qbytes")) {
            if (!object.value("$qbytes").isString()) {
                ok = false;
                return {};
            }
            const auto encoded = object.value("$qbytes").toString().toLatin1();
            const auto bytes = QByteArray::fromBase64Encoding(encoded);
            if (bytes.decodingStatus != QByteArray::Base64DecodingStatus::Ok) {
                ok = false;
                return {};
            }
            return bytes.decoded;
        }
        QVariantMap map;
        for (auto it = object.begin(); it != object.end(); ++it)
            map.insert(it.key(), decode(it.value(), ok));
        return map;
    }
    if (value.isArray()) {
        QVariantList list;
        for (const auto& item : value.toArray())
            list.push_back(decode(item, ok));
        return list;
    }
    return value.toVariant();
}

QJsonObject snapshot(QSettings& settings, bool& ok) {
    QJsonObject result;
    const auto keys = settings.allKeys();
    for (const auto& key : keys) {
        // Log documents and search-session state refer to machine-local files and are never portable.
        if (key == "logs/workspaces_v1" || key == "logs/workspaces_v2" || key == "logs/browser_session_v1" ||
            key == "logs/browser_find_v1")
            continue;
        result.insert(key, encode(settings.value(key), ok));
    }
    return result;
}

bool validateConnections(const QJsonArray& rows, QString& error) {
    if (rows.size() > 1024) {
        error = "工作区连接数量超过限制";
        return false;
    }
    QSet<QString> ids;
    for (const auto& row : rows) {
        if (!row.isObject()) {
            error = "连接配置格式不正确";
            return false;
        }
        const auto object = row.toObject();
        const auto config = ConnectionConfig::fromJson(object);
        const auto kind = object.value("kind").toString();
        if (config.id.trimmed().isEmpty() || config.name.trimmed().isEmpty() || ids.contains(config.id) ||
            (kind != "serial" && kind != "tcp" && kind != "test") || config.baud < 50 ||
            config.baud > 4000000 || config.tcpPort < 1 || config.tcpPort > 65535 || config.dataBits < 5 ||
            config.dataBits > 8 ||
            (config.parity != 0 && config.parity != 2 && config.parity != 3 && config.parity != 4 &&
             config.parity != 5) ||
            config.stopBits < 1 || config.stopBits > 3 || config.flow < 0 || config.flow > 2) {
            error = "连接配置含有无效字段或重复 ID";
            return false;
        }
        ids.insert(config.id);
    }
    return true;
}

QJsonObject pythonConnection(const QJsonObject& config) {
    const auto kind = config.value("kind").toString();
    const auto parity = config.value("parity").toInt();
    const auto flow = config.value("flow").toInt();
    const auto stopBits = config.value("stopBits").toInt(1);
    return {{"id", config.value("id")},
            {"name", config.value("name")},
            {"type", kind == "serial" ? "COM"
                     : kind == "tcp"  ? "TCP"
                                      : "TEST"},
            {"port", config.value("port")},
            {"host", config.value("host")},
            {"tcp_port", config.value("tcpPort")},
            {"baudrate", config.value("baud")},
            {"data_bits", QString::number(config.value("dataBits").toInt(8))},
            {"stop_bits", stopBits == 3 ? "1.5" : QString::number(stopBits)},
            {"parity", parity == 2   ? "偶"
                       : parity == 3 ? "奇"
                       : parity == 4 ? "Space"
                       : parity == 5 ? "Mark"
                                     : "无"},
            {"flow", flow == 1   ? "RTS/CTS"
                     : flow == 2 ? "XON/XOFF"
                                 : "无"},
            {"buffer_lines", config.value("bufferLines")},
            {"auto_save", config.value("autoLog")},
            {"save_dir", config.value("logDirectory")},
            {"input_echo", config.value("echo")},
            {"upgrade_mode", config.value("upgrade")},
            {"newline_mode", config.value("newline")},
            {"timestamp_interval", config.value("timestampInterval")},
            {"save_by_size", config.value("rotate")},
            {"save_size_kb", config.value("rotateKiB")}};
}

QJsonArray importConnections(const QJsonObject& settings, const QJsonObject& manifest, QString& error) {
    if (manifest.value("connections").isArray())
        return manifest.value("connections").toArray();
    const auto value = settings.value("connections/json");
    if (!value.isString())
        return {};
    QJsonParseError parseError;
    const auto parsed = QJsonDocument::fromJson(value.toString().toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isArray()) {
        error = "工作区连接配置 JSON 格式不正确";
        return {};
    }
    QJsonArray converted;
    for (const auto& row : parsed.array()) {
        if (!row.isObject()) {
            error = "工作区连接配置条目格式不正确";
            return {};
        }
        auto legacy = row.toObject();
        if (legacy.contains("kind")) {
            converted.push_back(legacy);
            continue;
        }
        const auto parity = legacy.value("parity").toString();
        const auto flow = legacy.value("flow").toString();
        const auto stop = legacy.value("stop_bits").toString();
        converted.push_back(QJsonObject{{"id", legacy.value("id")},
                                        {"name", legacy.value("name")},
                                        {"kind", legacy.value("type").toString() == "COM"    ? "serial"
                                                 : legacy.value("type").toString() == "TCP"  ? "tcp"
                                                 : legacy.value("type").toString() == "TEST" ? "test"
                                                                                             : "invalid"},
                                        {"port", legacy.value("port")},
                                        {"host", legacy.value("host")},
                                        {"tcpPort", legacy.value("tcp_port")},
                                        {"baud", legacy.value("baudrate")},
                                        {"dataBits", legacy.value("data_bits").toString().toInt()},
                                        {"parity", parity == "偶"      ? 2
                                                   : parity == "奇"    ? 3
                                                   : parity == "Space" ? 4
                                                   : parity == "Mark"  ? 5
                                                                       : 0},
                                        {"stopBits", stop == "1.5" ? 3 : stop.toInt()},
                                        {"flow", flow == "RTS/CTS"    ? 1
                                                 : flow == "XON/XOFF" ? 2
                                                                      : 0},
                                        {"autoLog", legacy.value("auto_save")},
                                        {"rotate", legacy.value("save_by_size")},
                                        {"echo", legacy.value("input_echo")},
                                        {"upgrade", legacy.value("upgrade_mode")},
                                        {"newline", legacy.value("newline_mode")},
                                        {"logDirectory", legacy.value("save_dir")},
                                        {"rotateKiB", legacy.value("save_size_kb")},
                                        {"bufferLines", legacy.value("buffer_lines")},
                                        {"timestampInterval", legacy.value("timestamp_interval")}});
    }
    return converted;
}

QJsonObject legacyLayout(const QJsonArray& rows, const QJsonObject& settings) {
    QJsonArray panes;
    int index = 0;
    for (const auto& value : rows) {
        const auto config = value.toObject();
        const auto legacyConfig = pythonConnection(config);
        panes.push_back(QJsonObject{{"id", config.value("id")},
                                    {"config", legacyConfig},
                                    {"connected", false},
                                    {"floating", false},
                                    {"rect", QJsonArray{40 + index * 24, 40 + index * 24, 720, 480}},
                                    {"state", 0},
                                    {"send_visible", false},
                                    {"send_height", 150},
                                    {"send_settings", QJsonObject{}}});
        ++index;
    }
    const auto geometry = settings.value("window/geometry");
    const QString geometryBase64 =
        geometry.isObject() ? geometry.toObject().value("$qbytes").toString() : QString{};
    const QJsonObject current{{"panes", panes},
                              {"active", QJsonValue::Null},
                              {"zorder", QJsonArray{}},
                              {"area", QJsonArray{1280, 760}},
                              {"window_size", QJsonArray{1280, 820}},
                              {"pinned", false},
                              {"send_height", 150},
                              {"compact_send", true},
                              {"preferences", QJsonObject{}},
                              {"geometry", geometryBase64}};
    return {{"version", 1}, {"geometry", geometryBase64}, {"current", current}};
}

QString zipError(const mz_zip_archive& zip) {
    const auto* reason = mz_zip_get_error_string(mz_zip_peek_last_error(const_cast<mz_zip_archive*>(&zip)));
    return reason ? QString::fromLatin1(reason) : QStringLiteral("未知 ZIP 错误");
}
} // namespace

bool WorkspaceArchive::write(const QString& path, QSettings& settings,
                             const QVector<ConnectionConfig>& connections, QString& error) {
    error.clear();
    bool encoded = true;
    auto values = snapshot(settings, encoded);
    if (!encoded) {
        error = "设置中包含不支持的值类型";
        return false;
    }
    QJsonArray rows;
    for (const auto& config : connections)
        rows.push_back(config.toJson());
    if (!validateConnections(rows, error))
        return false;
    QJsonArray legacyRows;
    for (const auto& row : rows)
        legacyRows.push_back(pythonConnection(row.toObject()));
    values.insert("connections/json",
                  QString::fromUtf8(QJsonDocument(legacyRows).toJson(QJsonDocument::Compact)));
    values.insert(
        "workspace/layout_v1",
        QString::fromUtf8(QJsonDocument(legacyLayout(rows, values)).toJson(QJsonDocument::Compact)));
    const QJsonObject manifest{{"format", "SerialWave.Workspace"},
                               {"version", 1},
                               {"release", QCoreApplication::applicationVersion()},
                               {"build", "cpp"},
                               {"settings", values},
                               {"connections", rows},
                               {"include_logs", false},
                               {"files", QJsonArray{}},
                               {"missing", QJsonArray{}}};
    const auto raw = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    if (raw.size() > kMaximumArchiveBytes) {
        error = "工作区设置超过 64 MiB，请减少保留的检索结果后再导出";
        return false;
    }

    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) {
        error = "无法初始化 ZIP 写入器：" + zipError(zip);
        return false;
    }
    void* archiveData = nullptr;
    size_t archiveSize = 0;
    const bool added =
        mz_zip_writer_add_mem(&zip, kManifestName, raw.constData(), static_cast<size_t>(raw.size()),
                              static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION));
    const bool finalized = added && mz_zip_writer_finalize_heap_archive(&zip, &archiveData, &archiveSize);
    const QString zipFailure = zipError(zip);
    mz_zip_writer_end(&zip);
    if (!finalized || !archiveData || archiveSize > static_cast<size_t>(kMaximumArchiveBytes)) {
        if (archiveData)
            mz_free(archiveData);
        error = "无法生成工作区包：" + zipFailure;
        return false;
    }

    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) ||
        output.write(static_cast<const char*>(archiveData), static_cast<qint64>(archiveSize)) !=
            static_cast<qint64>(archiveSize) ||
        !output.commit()) {
        error = output.errorString();
        mz_free(archiveData);
        return false;
    }
    mz_free(archiveData);
    return true;
}

bool WorkspaceArchive::captureSettings(QSettings& settings, QJsonObject& values, QString& error) {
    error.clear();
    bool encoded = true;
    values = snapshot(settings, encoded);
    if (!encoded) {
        error = "设置中包含不支持的值类型";
        return false;
    }
    return true;
}

bool WorkspaceArchive::read(const QString& path, QJsonObject& settings, QJsonArray& connections,
                            QString& error) {
    error.clear();
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly) || input.size() <= 0 || input.size() > kMaximumArchiveBytes) {
        error = input.isOpen() ? "工作区压缩包为空或超过 64 MiB" : input.errorString();
        return false;
    }
    const auto bytes = input.readAll();
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, bytes.constData(), static_cast<size_t>(bytes.size()), 0)) {
        error = "无法读取工作区 ZIP：" + zipError(zip);
        return false;
    }
    const auto count = mz_zip_reader_get_num_files(&zip);
    const auto index = mz_zip_reader_locate_file(&zip, kManifestName, nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE);
    mz_zip_archive_file_stat info{};
    const bool validEntry = count == 1 && index == 0 &&
                            mz_zip_reader_file_stat(&zip, static_cast<mz_uint>(index), &info) &&
                            !mz_zip_reader_is_file_encrypted(&zip, static_cast<mz_uint>(index)) &&
                            info.m_uncomp_size <= static_cast<mz_uint64>(kMaximumArchiveBytes) &&
                            mz_zip_validate_file(&zip, static_cast<mz_uint>(index), 0);
    if (!validEntry) {
        error = "工作区包必须仅包含有效的 workspace.json 清单";
        mz_zip_reader_end(&zip);
        return false;
    }
    size_t extractedSize = 0;
    void* extracted = mz_zip_reader_extract_to_heap(&zip, static_cast<mz_uint>(index), &extractedSize, 0);
    const QString zipFailure = zipError(zip);
    mz_zip_reader_end(&zip);
    if (!extracted || extractedSize > static_cast<size_t>(kMaximumArchiveBytes)) {
        if (extracted)
            mz_free(extracted);
        error = "无法解压工作区清单：" + zipFailure;
        return false;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(
        QByteArray(static_cast<const char*>(extracted), static_cast<qsizetype>(extractedSize)), &parseError);
    mz_free(extracted);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error = "工作区清单不是有效 JSON";
        return false;
    }
    const auto manifest = document.object();
    if (manifest.value("format") != "SerialWave.Workspace" || manifest.value("version").toInt(-1) != 1 ||
        !manifest.value("settings").isObject() || !manifest.value("files").isArray() ||
        !manifest.value("files").toArray().isEmpty()) {
        error = "不支持此工作区版本，或工作区包含本版本不支持的附件";
        return false;
    }
    settings = manifest.value("settings").toObject();
    connections = importConnections(settings, manifest, error);
    if (!error.isEmpty())
        return false;
    if (!validateConnections(connections, error))
        return false;
    bool decoded = true;
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        if (it.key().isEmpty() || it.key().contains(QChar::Null)) {
            error = "工作区设置键无效";
            return false;
        }
        decode(it.value(), decoded);
        if (!decoded) {
            error = "工作区包含损坏的二进制设置值";
            return false;
        }
    }
    return true;
}

bool WorkspaceArchive::replaceSettings(QSettings& destination, const QJsonObject& values, QString& error) {
    error.clear();
    QMap<QString, QVariant> decoded;
    bool valid = true;
    for (auto it = values.begin(); it != values.end(); ++it)
        decoded.insert(it.key(), decode(it.value(), valid));
    if (!valid) {
        error = "工作区设置包含无法读取的值";
        return false;
    }
    destination.clear();
    for (auto it = decoded.cbegin(); it != decoded.cend(); ++it)
        destination.setValue(it.key(), it.value());
    destination.sync();
    if (destination.status() != QSettings::NoError) {
        error = "无法写入工作区设置";
        return false;
    }
    return true;
}
} // namespace portwave
