#pragma once

#include <QSettings>
#include <QString>
#include <QVector>
#include <optional>

namespace portwave {
struct NamedWindowGeometry {
    QString connectionId;
    QByteArray geometry;
    bool maximized{};
};
struct NamedLayoutSnapshot {
    QByteArray mainGeometry;
    QString activeConnectionId;
    QVector<NamedWindowGeometry> windows;
    bool legacyGeometryOnly{};
};

// GUI-thread persistence adapter for named MDI layouts. QSettings owns storage; callers own snapshots.
// Invalid/missing snapshots return nullopt; save failures are surfaced through the settings status.
class NamedLayoutStore final {
  public:
    static bool save(QSettings& settings, const QString& name, const NamedLayoutSnapshot& snapshot,
                     QString& error);
    static std::optional<NamedLayoutSnapshot> load(const QSettings& settings, const QString& name);
};
} // namespace portwave
