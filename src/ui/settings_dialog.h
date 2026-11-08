#pragma once
#include "features/log_workspace.h"
#include "features/terminal_pane.h"
#include "ui/theme.h"
#include <QButtonGroup>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QGridLayout>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QMap>
#include <QPointer>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace portwave {
class ColorButton;
// GUI-only staged settings editor. Apply validates every changed public setting
// before updating repositories and live panes; Cancel leaves staged values untouched.
class SettingsDialog final : public QDialog {
    Q_OBJECT
  public:
    SettingsDialog(SettingsRepository& repository, ThemeManager& theme, const QVector<TerminalPane*>& panes,
                   LogWorkspace* logs, QWidget* parent = nullptr, QString category = {},
                   const QString& initialConnection = {});
    bool apply();
  signals:
    void actionRequested(const QString& action);
    void shortcutsChanged();
    void findPreferencesChanged(); // GUI thread, preferences committed; no layout/style changes.

  protected:
    void accept() override;

  private:
    QGridLayout* section(QVBoxLayout* page, const QString& title);
    void field(QGridLayout* section, const QString& label, QWidget* widget, const QString& key = {});
    void action(QVBoxLayout* page, const QString& text, const QString& id);
    void dirty();
    void storeConnection();
    void loadConnection();
    void changeConnection(int index);
    void resetPage();
    void updateConnectionType();
    void refreshCommands();
    void captureAppearance(int mode);
    void loadAppearance(int mode);
    SettingsRepository& repository_;
    ThemeManager& theme_;
    QVector<QPointer<TerminalPane>> panes_;
    QPointer<LogWorkspace> logs_;
    QMap<QString, ConnectionConfig> drafts_;
    QMap<QString, QWidget*> fields_;
    QMap<QString, QKeySequenceEdit*> keys_;
    QMap<int, QJsonObject> appearanceDrafts_, appearanceBaseline_;
    QMap<QString, QString> keysBaseline_;
    QComboBox *connections_, *workspaces_, *themeChoice_, *scrollMode_, *commandGroups_;
    QComboBox *controlStyle_{}, *layoutPreset_{};
    QWidget* serialSection_{};
    QStackedWidget* pages_;
    QVector<QPushButton*> categories_;
    QLineEdit* settingsSearch_;
    QButtonGroup *logOpenAction_{}, *logOpenProgramMode_{};
    QLineEdit* logOpenProgram_{};
    QLabel *heading_, *description_, *scopeHint_, *message_;
    QPushButton *cancelButton_, *applyButton_, *okButton_;
    QFontComboBox *font_, *logFont_;
    QDoubleSpinBox* sendInterval_;
    QComboBox* sendUnit_;
    QSpinBox *fontSize_, *logFontSize_;
    QCheckBox *smoothing_, *logWrap_, *logNumbers_, *logColors_, *autoSend_;
    QMap<QString, QCheckBox*> findOptions_;
    QComboBox* findMode_;
    ColorButton *background_, *foreground_, *active_, *reading_;
    QLabel* preview_;
    QTableWidget* commands_;
    int connectionIndex_ = -1, appearanceMode_{};
    bool loading_ = true, dirty_{};
};
} // namespace portwave
