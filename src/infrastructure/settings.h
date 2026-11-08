#pragma once
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace portwave {
enum class LogOpenAction { ExternalFile, PortWave, Folder };
struct LogOpenPreferences {
    LogOpenAction action = LogOpenAction::PortWave;
    bool customProgram = false;
    QString program;
};
struct ConnectionConfig {
    QString id, name = "连接1", kind = "serial", port, host = "127.0.0.1";
    int simulationIntervalMs = 10; // Per generated line, not per UI update.
    int baud = 115200, tcpPort = 9000, dataBits = 8, parity = 0, stopBits = 1, flow = 0;
    bool autoLog = false, rotate = false, echo = false, upgrade = false, newline = true;
    bool plainTextLog = true;
    QString logDirectory, firmware;
    int rotateMiB = 10, bufferLines = 500;
    int rotateKiB = 10240, displayMode = 0, timestampInterval = 0, sendIntervalMs = 1000;
    bool timestampDisplay = true, timestampLog = true, wrap = true, showErrors = true;
    bool pauseReceive = false, pauseDisplay = false, sendHex = false, sendCR = true, sendLF = false;
    bool sendCurrentLine = true, advanceLine = false;
    bool automaticSend = false;
    int sendIntervalUnit = 0;
    QString sendDraft, commandGroup;
    QJsonObject toJson() const;
    static ConnectionConfig fromJson(const QJsonObject& object);
};
// GUI-thread settings repository; isolated from the Python application's profile.
class SettingsRepository {
  public:
    // GUI-thread, application-wide value snapshots (no retained references).
    // Missing/invalid action falls back to PortWave; empty program uses the OS association.
    LogOpenPreferences loadLogOpenPreferences() const;
    // GUI-thread commit to the C++ profile. Returns false with an error on sync failure.
    bool saveLogOpenPreferences(const LogOpenPreferences& preferences, QString& error) const;
    QVector<ConnectionConfig> loadConnections() const;
    bool saveConnections(const QVector<ConnectionConfig>& configs, QString& error) const;
    // GUI-thread call; atomically replaces one connection by id, without merging unrelated profile data.
    bool updateConnection(const ConnectionConfig& config, QString& error) const;
    static QString dataDirectory();
};
void diagnostic(const QString& event, const QJsonObject& fields = {}); // Metadata only, never RX/TX content.
} // namespace portwave
Q_DECLARE_METATYPE(portwave::ConnectionConfig)
