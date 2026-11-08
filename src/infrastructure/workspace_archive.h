#pragma once

#include "infrastructure/settings.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>
#include <QString>

namespace portwave {
// GUI-thread workspace package I/O. Packages use the Python tool's ZIP/manifest shape,
// contain settings and connection profiles only, and never include open log documents.
// Calls are synchronous and bounded to a 64 MiB manifest/package; errors are returned in error.
class WorkspaceArchive final {
  public:
    static bool write(const QString& path, QSettings& settings, const QVector<ConnectionConfig>& connections,
                      QString& error);
    static bool captureSettings(QSettings& settings, QJsonObject& values, QString& error);
    static bool read(const QString& path, QJsonObject& settings, QJsonArray& connections, QString& error);
    // Replaces all QSettings keys. Caller must snapshot values if a later repository write can fail.
    static bool replaceSettings(QSettings& destination, const QJsonObject& values, QString& error);
};
} // namespace portwave
