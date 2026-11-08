#pragma once
#include "features/log_workspace.h"
#include "features/snap_layout.h"
#include "features/split_grip.h"
#include "features/terminal_pane.h"
#include "ui/theme.h"
#include <QLabel>
#include <QMainWindow>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QToolBar>

namespace portwave {
class WindowCaption;
class MainWindow final : public QMainWindow {
    Q_OBJECT
  public:
    MainWindow(SettingsRepository& repository, ThemeManager& theme);
    ~MainWindow() override;
    void addTestConnection();
    void openFile(const QString& path);

  protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* object, QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

  private:
    struct Record {
        Session* session;
        TerminalPane* pane;
        QMdiSubWindow* window;
        bool detached{};
        bool powerPending{true};
        int controlsHeight{200};
    };
    void addConnection(const ConnectionConfig& config);
    void newConnection();
    void settings();
    void restyle(bool dark);
    void applyTerminalAppearance(TerminalPane* pane, bool dark);
    Record* current();
    void closeRecord(QMdiSubWindow* window);
    void operation(const QString& operation);
    void rebuildShortcuts();
    void saveConfiguration(const ConnectionConfig& config);
    void syncConnectionTabs();
    void updateActivePaneOutline();
    void dockRecord(QMdiSubWindow* window);
    void updateToolbarState();
    void updateConnectionStatus();
    SettingsRepository& repository_;
    ThemeManager& theme_;
    QMdiArea* mdi_;
    QTabBar* tabs_;
    QStackedWidget* controls_;
    QStackedWidget* fileStatuses_;
    QSplitter* workspaceSplitter_;
    QPointer<QMdiSubWindow> lastActive_;
    bool senderPinned_{true};
    int pinnedSenderHeight_{200};
    SnapLayout* snap_;
    SplitGrip* split_;
    QToolBar* toolbar_;
    WindowCaption* caption_{};
    QLabel* connectionStatus_{};
    LogWorkspace* logs_;
    QVector<Record> records_;
    QVector<QPair<QAction*, QString>> icons_;
    bool closing_{};
    int testCount_{};
    QVector<QShortcut*> shortcuts_;
};
} // namespace portwave
