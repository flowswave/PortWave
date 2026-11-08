#pragma once
#include "editor/editor_interface.h"
#include "services/search_service.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QHash>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSet>
#include <QShortcut>
#include <QSlider>
#include <QStringList>
#include <QTabBar>
#include <QTimer>
#include <QTreeWidget>
#include <functional>

namespace portwave {
class FindDialog final : public QDialog {
    Q_OBJECT
  public:
    using Provider = std::function<QVector<SearchDocument>(bool)>;
    using Activator = std::function<bool(const SearchHit&)>;
    // GUI-thread modeless floating window, owned by parent. Providers/activators
    // are invoked on the GUI thread and must outlive this dialog. Search errors
    // appear in its status; cache/log window geometry is persisted separately.
    FindDialog(Provider provider, Activator activate, bool cacheMode, QString preferenceKey,
               QWidget* parent = nullptr);
    void openFind(const QString& selected = {});
    // GUI-only display palette; invalid background uses current theme defaults.
    void setMarkBackground(const QColor& background);
    // GUI-thread view transitions only. Closing also cancels pending searches.
    void reject() override;
    void setPreferenceKey(const QString& key);
    // GUI thread only; controls whether completed log searches append history.
    void setLogHistoryEnabled(bool enabled) { logHistoryEnabled_ = enabled; }
    // GUI-only: sequences supplied by the UI layer (empty disables that action).
    void setShortcutOverrides(const QHash<QString, QKeySequence>& overrides);
    // GUI-only match-history accessors for the query drop-down.
    void clearSearchHistory();
    SearchOptions options() const;
    void setOptions(const SearchOptions& options);
    void searchAll(bool allOpened = false);
    // GUI-thread directional lookup in the current provider snapshot; asynchronous
    // and cancellable through cancelSearch(), with errors shown in this dialog.
    void findNext(bool reverse = false) { run(Operation::Next, false, reverse); }
    // GUI thread only; invalidates callbacks from the current search without
    // blocking. The dialog and its result history remain owned by the caller.
    void cancelSearch() { service_.cancel(); }
    void reloadPreferences() { loadPreferences(); }
    void tickLive() {
        if (live_->isChecked() && isVisible() && !interactingWithResults() && !liveTimer_->isActive())
            liveTimer_->start();
    }
  signals:
    void resultsChanged(const QVector<portwave::SearchHit>& hits, const QString& query, bool append);
    void marksRequested(const QVector<portwave::TextMark>& marks, bool clearPrevious);
    void clearMarksRequested();
    void copyMarksRequested();
    void allSearchFinished(int total, int searchedDocuments);

  protected:
    bool eventFilter(QObject* object, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    enum class Operation { Next, All, Count, Mark };
    void run(Operation operation, bool allOpened = false, bool reverseOverride = false,
             QVector<SearchDocument> external = {});
    void finish(const SearchBatch& batch);
    void chooseFiles();
    void renderHits(const QVector<SearchHit>& hits, bool append);
    void savePreferences();
    void loadPreferences();
    // GUI-only: rebuilds the drop-down from history_ plus a final clear entry that
    // wipes every remembered query for the current preference key.
    void rebuildHistoryItems();
    bool interactingWithResults() const {
        return results_ && (results_->hasFocus() || !results_->selectedItems().isEmpty());
    }
    void updateOpacity();
    QString geometryKey() const;
    void saveWindowGeometry();
    void restoreWindowGeometry();
    Provider provider_;
    Activator activate_;
    SearchService service_;
    QString preferenceKey_;
    QColor markBackground_;
    bool cacheMode_{}, reverse_{}, append_{}, restoringGeometry_{};
    bool logHistoryEnabled_{true};
    Operation operation_ = Operation::All;
    QVector<SearchHit> hits_;
    // Targets already listed in the current result list (document + line), so a
    // repeated live/keep search does not append the same hit twice.
    QSet<QString> shownHits_;
    QStringList history_;
    int searchedDocuments_{};
    SearchDocument source_;
    QTabBar* tabs_;
    QComboBox *query_{}, *markColor_{};
    QCheckBox *case_, *whole_, *wrap_, *backward_, *dotall_, *keep_, *live_, *bookmark_, *clearPrevious_,
        *selectionOnly_, *transparent_{};
    QRadioButton *normal_, *extended_, *regex_, *inactive_, *always_;
    QSlider* opacity_;
    QTreeWidget* results_{};
    QLabel *status_, *target_;
    QWidget *markOptions_, *findActions_, *markActions_, *extra_, *transparency_;
    QTimer* liveTimer_{};
    QShortcut *nextShortcut_{}, *previousShortcut_{};
};
} // namespace portwave
