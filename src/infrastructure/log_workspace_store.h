#pragma once
#include <QJsonObject>
namespace portwave {
// GUI-thread bounded settings storage. Only paths and view preferences are
// accepted; returned objects own their data. Errors leave prior settings intact.
class LogWorkspaceStore {
  public:
    static QJsonObject load();
    static bool save(const QJsonObject& state, QString& error);
};
} // namespace portwave
