#include "infrastructure/settings.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>

namespace portwave {
LogOpenPreferences SettingsRepository::loadLogOpenPreferences() const {
    QSettings settings;
    LogOpenPreferences result;
    const auto action = settings.value("logs/open/action", "portwave").toString();
    if (action == "external")
        result.action = LogOpenAction::ExternalFile;
    else if (action == "folder")
        result.action = LogOpenAction::Folder;
    result.customProgram = settings.value("logs/open/customProgram", false).toBool();
    result.program = settings.value("logs/open/program").toString();
    return result;
}
bool SettingsRepository::saveLogOpenPreferences(const LogOpenPreferences& preferences, QString& error) const {
    QSettings settings;
    settings.setValue("logs/open/action", preferences.action == LogOpenAction::ExternalFile ? "external"
                                          : preferences.action == LogOpenAction::Folder     ? "folder"
                                                                                            : "portwave");
    settings.setValue("logs/open/customProgram", preferences.customProgram);
    settings.setValue("logs/open/program", preferences.program.trimmed());
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        error = "无法保存日志打开方式";
        return false;
    }
    return true;
}
QJsonObject ConnectionConfig::toJson() const {
    QJsonObject result{{"id", id},
                       {"name", name},
                       {"kind", kind},
                       {"port", port},
                       {"host", host},
                       {"baud", baud},
                       {"tcpPort", tcpPort},
                       {"dataBits", dataBits},
                       {"parity", parity},
                       {"stopBits", stopBits},
                       {"flow", flow},
                       {"autoLog", autoLog},
                       {"plainTextLog", plainTextLog},
                       {"rotate", rotate},
                       {"echo", echo},
                       {"upgrade", upgrade},
                       {"newline", newline},
                       {"logDirectory", logDirectory},
                       {"firmware", firmware},
                       {"rotateMiB", rotateMiB},
                       {"bufferLines", bufferLines}};
    result.insert("rotateKiB", rotateKiB);
    result.insert("displayMode", displayMode);
    result.insert("simulationIntervalMs", simulationIntervalMs);
    result.insert("timestampInterval", timestampInterval);
    result.insert("timestampDisplay", timestampDisplay);
    result.insert("timestampLog", timestampLog);
    result.insert("wrap", wrap);
    result.insert("showErrors", showErrors);
    result.insert("pauseReceive", pauseReceive);
    result.insert("pauseDisplay", pauseDisplay);
    result.insert("sendHex", sendHex);
    result.insert("sendCR", sendCR);
    result.insert("sendLF", sendLF);
    result.insert("sendCurrentLine", sendCurrentLine);
    result.insert("advanceLine", advanceLine);
    result.insert("sendIntervalMs", sendIntervalMs);
    result.insert("sendIntervalUnit", sendIntervalUnit);
    result.insert("automaticSend", automaticSend);
    result.insert("sendDraft", sendDraft);
    result.insert("commandGroup", commandGroup);
    return result;
}
ConnectionConfig ConnectionConfig::fromJson(const QJsonObject& o) {
    ConnectionConfig c;
    c.id = o["id"].toString(QUuid::createUuid().toString(QUuid::WithoutBraces));
    c.name = o["name"].toString("连接1");
    c.kind = o["kind"].toString("serial");
    c.port = o["port"].toString();
    c.host = o["host"].toString("127.0.0.1");
    c.baud = qBound(50, o["baud"].toInt(115200), 4000000);
    c.tcpPort = qBound(1, o["tcpPort"].toInt(9000), 65535);
    c.dataBits = qBound(5, o["dataBits"].toInt(8), 8);
    c.parity = o["parity"].toInt();
    c.stopBits = qBound(1, o["stopBits"].toInt(1), 3);
    c.flow = o["flow"].toInt();
    c.autoLog = o["autoLog"].toBool();
    c.plainTextLog = o["plainTextLog"].toBool(true);
    c.rotate = o["rotate"].toBool(false);
    c.echo = o["echo"].toBool();
    c.upgrade = o["upgrade"].toBool();
    c.newline = o["newline"].toBool(true);
    c.logDirectory = o["logDirectory"].toString();
    c.firmware = o["firmware"].toString();
    c.rotateMiB = qBound(1, o["rotateMiB"].toInt(10), 1024);
    c.bufferLines = qBound(100, o["bufferLines"].toInt(500), 100000);
    c.rotateKiB = qBound(1, o["rotateKiB"].toInt(c.rotateMiB * 1024), 1048576);
    c.simulationIntervalMs = qBound(1, o["simulationIntervalMs"].toInt(10), 60000);
    c.displayMode = qBound(0, o["displayMode"].toInt(), 2);
    c.timestampInterval = qBound(0, o["timestampInterval"].toInt(), 86400);
    c.timestampDisplay = o["timestampDisplay"].toBool(true);
    c.timestampLog = o["timestampLog"].toBool(true);
    c.wrap = o["wrap"].toBool(true);
    c.showErrors = o["showErrors"].toBool(true);
    c.pauseReceive = o["pauseReceive"].toBool();
    c.pauseDisplay = o["pauseDisplay"].toBool();
    c.sendHex = o["sendHex"].toBool();
    c.sendCR = o["sendCR"].toBool(true);
    c.sendLF = o["sendLF"].toBool();
    c.sendCurrentLine = o["sendCurrentLine"].toBool(true);
    c.advanceLine = o["advanceLine"].toBool();
    c.sendIntervalMs = qBound(10, o["sendIntervalMs"].toInt(1000), 3600000);
    c.sendIntervalUnit = qBound(0, o["sendIntervalUnit"].toInt(), 1);
    c.automaticSend = o["automaticSend"].toBool();
    c.sendDraft = o["sendDraft"].toString();
    c.commandGroup = o["commandGroup"].toString();
    return c;
}
QString SettingsRepository::dataDirectory() {
    auto dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dir);
    return dir;
}
QVector<ConnectionConfig> SettingsRepository::loadConnections() const {
    QFile file(dataDirectory() + "/connections.json");
    QVector<ConnectionConfig> configs;
    if (!file.open(QIODevice::ReadOnly))
        return configs;
    if (file.size() > 1024LL * 1024) {
        diagnostic("settings.too_large");
        return configs;
    }
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isArray()) {
        diagnostic("settings.invalid");
        return configs;
    }
    for (const auto& value : doc.array())
        if (value.isObject())
            configs.push_back(ConnectionConfig::fromJson(value.toObject()));
    return configs;
}
bool SettingsRepository::saveConnections(const QVector<ConnectionConfig>& configs, QString& error) const {
    QJsonArray array;
    for (const auto& c : configs)
        array.push_back(c.toJson());
    QSaveFile file(dataDirectory() + "/connections.json");
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(array).toJson()) < 0 || !file.commit()) {
        error = file.errorString();
        return false;
    }
    return true;
}
bool SettingsRepository::updateConnection(const ConnectionConfig& config, QString& error) const {
    auto configs = loadConnections();
    for (auto& existing : configs) {
        if (existing.id != config.id)
            continue;
        if (existing.toJson() == config.toJson())
            return true;
        existing = config;
        return saveConnections(configs, error);
    }
    return true;
}
void diagnostic(const QString& event, const QJsonObject& fields) {
    auto dir = SettingsRepository::dataDirectory() + "/diagnostics";
    QDir().mkpath(dir);
    QFile file(dir + "/runtime.jsonl");
    if (file.size() > 1024LL * 1024) {
        QFile::remove(dir + "/runtime.previous.jsonl");
        file.rename(dir + "/runtime.previous.jsonl");
        file.setFileName(dir + "/runtime.jsonl");
    }
    if (file.open(QIODevice::Append)) {
        auto obj = fields;
        obj["event"] = event;
        obj["utc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n');
    }
}
} // namespace portwave
