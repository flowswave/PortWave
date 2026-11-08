#pragma once
#include "editor/editor_interface.h"
#include "features/find_dialog.h"
#include "services/file_service.h"
#include <QAction>
#include <QHash>
#include <QJsonObject>
#include <QMainWindow>
#include <QSplitter>
#include <QTabWidget>
#include <QTreeWidget>
#include <memory>
#include <optional>
#include <vector>

namespace portwave {
struct LogReadState;
// Independent document window. Its focus never mutates terminal send targets.
class LogDocumentView final : public QMainWindow {
    Q_OBJECT
  public:
    explicit LogDocumentView(QWidget* parent = nullptr);
    ~LogDocumentView() override;
    void openPath(const QString& path);
    void setTheme(bool dark);
    void search();
    void addExample();
    QVector<SearchDocument> searchSources(bool all) const;
    bool activateHit(const SearchHit& hit);
    void applyMarks(const QVector<TextMark>& marks, bool clearPrevious);
    void setReadingOptions(bool wrap, bool numbers, bool colors);
    void setReadingAppearance(const QFont& font, const QColor& background, const QColor& foreground);
    void showFindOptions();
    // GUI-thread view state; snapshots contain paths/preferences, never log text.
    // restore loads files asynchronously, cloneFrom reuses loaded text in memory.
    QJsonObject snapshot() const;
    void restore(const QJsonObject& state);
    void cloneFrom(const LogDocumentView& source);
    void setPreferenceKey(const QString& key);
    // GUI-only: sequences supplied by the UI layer (empty disables that action).
    // Forwarded to the find dialog that this window owns.
    void setShortcutOverrides(const QHash<QString, QKeySequence>& overrides);
    void hideFinder();
    void chooseFiles();
    void refreshCurrent();
    // GUI-thread, non-blocking cancellation; existing documents stay intact.
    void cancelLoading();

  protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    struct Document {
        QString path;
        std::unique_ptr<EditorInterface> editor;
        int revision = 1;
        QVector<EditorAnsiSpan> ansiStyles;
        bool pinned{}, autoRefresh{};
        qint64 diskSize{-1}, diskModified{};
        QString lineEnding = "Unix (LF)";
        FileReadCursor cursor;
        std::shared_ptr<LogReadState> readState;
    };
    void loadNext();
    void closeDocument(int index);
    void decorateTab(int index);
    void updateReaderStatus();
    void restoreDocumentState(int index);
    void appendResults(const QVector<SearchHit>& hits, const QString& query, bool append);
    // GUI-only: expand the current search group and fold every older result group.
    void focusResultGroup();
    void showResultsMenu(const QPoint& position);
    void copySelectedResults();
    void findInResults();
    void removeCurrentResultGroup();
    EditorInterface* currentEditor() const;
    QTabWidget* tabs_;
    QSplitter* readingSplit_{};
    QTreeWidget* results_;
    QWidget* resultPanel_;
    QLabel* resultSummary_{};
    QSplitter* splitter_;
    QCheckBox* autoRefresh_;
    QLabel* location_;
    QTimer statusTimer_, refreshTimer_;
    FileService files_;
    // GUI owns the hidden view; worker owns only the detached load state.
    std::unique_ptr<EditorInterface> loadingEditor_;
    std::shared_ptr<LogReadState> loadingDocument_;
    std::vector<Document> documents_;
    bool dark_ = true;
    bool readingColors_ = true;
    bool readingWrap_ = true, readingNumbers_ = true, readingAppearanceCustom_{};
    QFont readingFont_{"Consolas", 12};
    QColor readingBackground_, readingForeground_;
    std::shared_ptr<std::atomic_bool> searchCancel_;
    QStringList pendingFiles_;
    QString loadingPath_;
    bool loading_{};
    FindDialog* finder_{};
    QTreeWidgetItem* resultGroup_{};
    QHash<QString, QTreeWidgetItem*> resultFiles_;
    QString resultQuery_;
    QString resultFindText_;
    bool clearResultsEachSearch_{};
    std::optional<SearchHit> pendingHit_;
    QString preferenceKey_ = "search/log/default";
    QHash<QString, QAction*> shortcutActions_;
    QHash<QString, QKeySequence> shortcutOverrides_;
    QHash<QString, QJsonObject> restoreStates_;
    QString restoreActivePath_;
    std::optional<SearchOptions> restoredFindOptions_;
};
} // namespace portwave
