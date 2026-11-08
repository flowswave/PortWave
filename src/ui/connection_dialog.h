#pragma once
#include "infrastructure/settings.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QSpinBox>

namespace portwave {
class ConnectionDialog final : public QDialog {
    Q_OBJECT
  public:
    explicit ConnectionDialog(SettingsRepository& repository, QWidget* parent = nullptr);
    QVector<ConnectionConfig> selected() const;

  private:
    void load(int index);
    void store();
    bool persist();
    SettingsRepository& repository_;
    QVector<ConnectionConfig> configs_;
    int index_ = -1;
    QListWidget* list_;
    QLineEdit *name_, *host_, *logDir_;
    QComboBox *kind_, *port_, *baud_, *dataBits_, *parity_, *stopBits_, *flow_;
    QSpinBox *tcpPort_, *lines_, *rotateSize_, *simulationInterval_;
    QCheckBox *echo_, *upgrade_, *newline_, *autoLog_, *rotate_;
};
} // namespace portwave
