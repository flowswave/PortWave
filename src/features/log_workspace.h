#pragma once
#include "features/log_document_view.h"
#include <QJsonObject>
#include <QMainWindow>
#include <QMenu>
#include <QPointer>
#include <QStackedWidget>
#include <QTabBar>
#include <QToolButton>
#include <vector>

namespace portwave {
// GUI-thread log host. It owns independent workspace pages and their floating
// shells; switching or moving pages never changes terminal send targets. File I/O
// stays asynchronous in LogDocumentView. Error messages are displayed locally.
class LogWorkspace final : public QMainWindow {
    Q_OBJECT
  public:
    explicit LogWorkspace(QWidget* parent = nullptr);
    ~LogWorkspace() override;
    void openPath(const QString& path);
    void openConnectionLog(const QString& connectionId, const QString& name, const QString& path);
    void registerConnection(const QString& connectionId, const QString& name);
    void activateConnection(const QString& connectionId, const QString& name);
    void setTheme(bool dark);
    void search();
    void addExample();
    QVector<SearchDocument> searchSources(bool all) const;
    bool activateHit(const SearchHit& hit);
    void applyMarks(const QVector<TextMark>& marks, bool clearPrevious);
    void setReadingOptions(bool wrap, bool numbers, bool colors);
    void setReadingAppearance(const QFont& font, const QColor& background, const QColor& foreground);
    // GUI-only: sequences supplied by the UI layer, applied to every workspace page
    // (including pages created later). Empty sequences disable that action.
    void setShortcutOverrides(const QHash<QString, QKeySequence>& overrides);
    void showFindOptions() { search(); }
    // Page pointers are borrowed until host destruction. These GUI operations
    // retain loaded editor objects; create returns an empty id at the 128-page cap.
    LogDocumentView* activeView() const;
    QString createWorkspace(const QString& name, bool copyCurrent = false);
    void activateWorkspace(const QString& id);
    void renameWorkspace(const QString& id, const QString& name);
    void detachWorkspace(const QString& id);
    void restoreSingleWindow();
    void saveWorkspaces();
  signals:
    void workspaceOperationRequested(const QString& operation);

  protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    struct Workspace {
        QString id, name, connectionId;
        LogDocumentView* view{};
        QPointer<QMainWindow> floating;
        QJsonObject saved;
        bool restored{}, automaticName{};
        bool restoreFloating{};
        QByteArray floatingGeometry;
    };
    Workspace* current();
    void activateIndex(int index);
    void rebuildTabs();
    void fillMenu();
    void promptCreate(bool copy);
    void dockWorkspace(const QString& id);
    std::vector<Workspace> workspaces_;
    QTabBar* workspaceTabs_;
    QStackedWidget* pages_;
    QToolButton* menuButton_;
    QMenu* menu_;
    bool dark_{true}, switching_{};
    bool customReadingAppearance_{};
    int activeIndex_{-1};
    QPoint dragStart_;
    QString dragWorkspaceId_;
    QFont readingFont_{"Consolas", 12};
    QColor readingBackground_{"#171b20"}, readingForeground_{"#b8c3d3"};
    bool wrap_{true}, numbers_{true}, colors_{true};
    QHash<QString, QKeySequence> shortcutOverrides_;
};
} // namespace portwave
