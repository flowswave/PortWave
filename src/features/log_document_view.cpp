#include "features/log_document_view.h"
#include "core/ansi.h"
#include "editor/scintilla_editor.h"
#include "features/stable_result_delegate.h"
#include "features/window_caption.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <functional>

namespace portwave {
struct LogReadState {
    std::shared_ptr<EditorLoadBuffer> buffer;
    AnsiDecoder decoder;
    QVector<EditorAnsiSpan> styles;
    qsizetype utf16Position{};
    bool largeFile{};
    bool tailMode{};
    QByteArray tail;

    bool append(QByteArrayView chunk, QString& error) {
        QByteArray output;
        output.reserve(chunk.size());
        for (const auto& run :
             decoder.feed(std::string_view(chunk.data(), static_cast<std::size_t>(chunk.size())))) {
            if (run.kind == TerminalRun::Kind::Text) {
                if (!largeFile) {
                    const auto length =
                        QString::fromUtf8(run.utf8.data(), static_cast<qsizetype>(run.utf8.size())).size();
                    if (run.style.foreground >= 0 || run.style.background >= 0 || run.style.bold)
                        styles.push_back({utf16Position, length, run.style.foreground, run.style.background,
                                          run.style.bold});
                    utf16Position += length;
                }
                output.append(run.utf8.data(), static_cast<qsizetype>(run.utf8.size()));
            } else if (run.kind == TerminalRun::Kind::Newline) {
                output += '\n';
                if (!largeFile)
                    ++utf16Position;
            }
        }
        if (tailMode) {
            tail += output;
            return true;
        }
        return output.isEmpty() || buffer->appendUtf8(output, error);
    }
};
namespace {
class LogResultsTree final : public QTreeWidget {
  public:
    using QTreeWidget::QTreeWidget;
    std::function<void()> copyRequested;
    std::function<void()> findRequested;

  protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->matches(QKeySequence::Copy) && copyRequested) {
            copyRequested();
            event->accept();
            return;
        }
        if (event->matches(QKeySequence::Find) && findRequested) {
            findRequested();
            event->accept();
            return;
        }
        QTreeWidget::keyPressEvent(event);
    }
};

} // namespace
LogDocumentView::LogDocumentView(QWidget* parent) : QMainWindow(parent) {
    readingBackground_ = QColor("#171b20");
    readingForeground_ = QColor("#b8c3d3");
    setAttribute(Qt::WA_DeleteOnClose, false);
    setObjectName("logDocumentView");
    resize(1100, 760);
    auto bind = [this](const QString& id, const QString& name, QKeySequence shortcut, auto callback) {
        auto* action = new QAction(name, this);
        action->setShortcut(shortcut);
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(action);
        connect(action, &QAction::triggered, this, callback);
        if (!id.isEmpty())
            shortcutActions_.insert(id, action);
    };
    bind("log_open", "打开日志…", QKeySequence::Open, [this] { chooseFiles(); });
    bind("search", "查找 / 标记…", QKeySequence::Find, [this] { search(); });
    bind("log_refresh", "手动刷新", QKeySequence(Qt::Key_F5), [this] { refreshCurrent(); });
    bind({}, "取消加载", QKeySequence(Qt::Key_Escape), [this] { cancelLoading(); });
    bind("log_close", "关闭当前标签", QKeySequence::Close, [this] { closeDocument(tabs_->currentIndex()); });
    for (bool reverse : {false, true})
        bind(reverse ? "find_previous" : "find_next", reverse ? "查找上一处" : "查找下一处",
             QKeySequence(reverse ? Qt::SHIFT | Qt::Key_F3 : Qt::Key_F3), [this, reverse] {
                 if (!finder_)
                     search();
                 if (finder_)
                     finder_->findNext(reverse);
             });
    auto* splitter = splitter_ = new QSplitter(Qt::Vertical);
    tabs_ = new QTabWidget;
    tabs_->setObjectName("logDocumentTabs");
    tabs_->setTabsClosable(false);
    tabs_->setMovable(true);
    tabs_->setDocumentMode(true);
    tabs_->setElideMode(Qt::ElideMiddle);
    tabs_->setIconSize(QSize(18, 18));
    tabs_->tabBar()->setObjectName("logFileTabs");
    tabs_->tabBar()->installEventFilter(this);
    tabs_->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tabs_->tabBar(), &QWidget::customContextMenuRequested, this, [this](QPoint point) {
        const int index = tabs_->tabBar()->tabAt(point);
        QMenu menu(this);
        menu.addAction("打开日志…", this, &LogDocumentView::chooseFiles);
        menu.addAction("关闭当前标签", this, [this, index] { closeDocument(index); })->setEnabled(index >= 0);
        menu.addAction("关闭全部未固定标签", this, [this] {
            for (int i = tabs_->count() - 1; i >= 0; --i)
                if (!documents_[static_cast<std::size_t>(i)].pinned)
                    closeDocument(i);
        });
        menu.addSeparator();
        menu.addAction("查找 / 标记…", this, &LogDocumentView::search);
        menu.exec(tabs_->tabBar()->mapToGlobal(point));
    });
    connect(tabs_->tabBar(), &QTabBar::tabMoved, this, [this](int from, int to) {
        if (from >= 0 && to >= 0 && from < static_cast<int>(documents_.size()) &&
            to < static_cast<int>(documents_.size())) {
            auto moved = std::move(documents_[static_cast<std::size_t>(from)]);
            documents_.erase(documents_.begin() + from);
            documents_.insert(documents_.begin() + to, std::move(moved));
            updateReaderStatus();
        }
    });
    resultPanel_ = new QWidget;
    resultPanel_->setObjectName("logResultsPanel");
    auto* resultLayout = new QVBoxLayout(resultPanel_);
    resultLayout->setContentsMargins(0, 0, 0, 0);
    resultLayout->setSpacing(0);
    auto* resultHeader = new QWidget;
    resultHeader->setObjectName("logResultsHeader");
    auto* headerLayout = new QHBoxLayout(resultHeader);
    headerLayout->setContentsMargins(8, 2, 4, 2);
    resultSummary_ = new QLabel("搜索结果");
    headerLayout->addWidget(resultSummary_, 1);
    auto* closeResults = new QToolButton;
    closeResults->setObjectName("closeLogResults");
    closeResults->setText(QString::fromUtf8("×"));
    closeResults->setToolTip("关闭查找结果");
    connect(closeResults, &QToolButton::clicked, resultPanel_, &QWidget::hide);
    headerLayout->addWidget(closeResults);
    resultLayout->addWidget(resultHeader);
    auto* resultsTree = new LogResultsTree;
    results_ = resultsTree;
    results_->setItemDelegate(new StableResultDelegate(results_));
    results_->setColumnCount(1);
    results_->setHeaderLabels({"搜索结果"});
    results_->setHeaderHidden(true);
    results_->header()->setStretchLastSection(true);
    results_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    results_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(results_, &QWidget::customContextMenuRequested, this, &LogDocumentView::showResultsMenu);
    resultsTree->copyRequested = [this] { copySelectedResults(); };
    resultsTree->findRequested = [this] { findInResults(); };
    readingSplit_ = new QSplitter(Qt::Horizontal);
    readingSplit_->setObjectName("logReadingSplit");
    readingSplit_->addWidget(tabs_);
    readingSplit_->setStretchFactor(0, 1);
    readingSplit_->setChildrenCollapsible(false);
    splitter->addWidget(readingSplit_);
    resultLayout->addWidget(results_);
    splitter->addWidget(resultPanel_);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 0);
    resultPanel_->hide();
    setCentralWidget(splitter);
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &LogDocumentView::closeDocument);
    statusBar()->setObjectName("logStatusBar");
    statusBar()->setSizeGripEnabled(false);
    // QStatusBar hides normal widgets while showMessage() is active. Keep the
    // whole reader footer permanent, with a separate label for those messages.
    auto* footer = new QWidget;
    footer->setObjectName("logReaderFooter");
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(0, 0, 0, 0);
    footerLayout->setSpacing(6);
    autoRefresh_ = new QCheckBox("自动刷新");
    autoRefresh_->setObjectName("logAutoRefresh");
    footerLayout->addWidget(autoRefresh_);
    auto* manual = new QPushButton("手动刷新");
    manual->setObjectName("logManualRefresh");
    manual->setAutoDefault(false);
    manual->setFixedHeight(28);
    autoRefresh_->setFixedHeight(28);
    autoRefresh_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    footerLayout->setAlignment(Qt::AlignVCenter);
    footerLayout->addWidget(manual);
    auto* message = new QLabel;
    message->setObjectName("logReaderMessage");
    message->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    message->setMinimumWidth(0);
    footerLayout->addWidget(message, 1);
    location_ = new QLabel;
    location_->setObjectName("logReaderLocation");
    location_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    footerLayout->addWidget(location_);
    statusBar()->addPermanentWidget(footer, 1);
    connect(statusBar(), &QStatusBar::messageChanged, message, [message](const QString& text) {
        message->setText(text);
        message->setToolTip(text);
    });
    connect(manual, &QPushButton::clicked, this, &LogDocumentView::refreshCurrent);
    connect(autoRefresh_, &QCheckBox::toggled, this, [this](bool enabled) {
        int index = tabs_->currentIndex();
        if (index >= 0)
            documents_[static_cast<std::size_t>(index)].autoRefresh = enabled;
    });
    connect(tabs_, &QTabWidget::currentChanged, this, [this] {
        if (finder_)
            finder_->cancelSearch();
        updateReaderStatus();
    });
    statusTimer_.setInterval(150);
    connect(&statusTimer_, &QTimer::timeout, this, &LogDocumentView::updateReaderStatus);
    statusTimer_.start();
    refreshTimer_.setInterval(1000);
    connect(&refreshTimer_, &QTimer::timeout, this, [this] {
        const int index = tabs_->currentIndex();
        if (!isVisible() || index < 0 || loading_)
            return;
        const auto& doc = documents_[static_cast<std::size_t>(index)];
        const QFileInfo info(doc.path);
        if (doc.autoRefresh && info.exists() &&
            (info.size() != doc.diskSize || info.lastModified().toMSecsSinceEpoch() != doc.diskModified))
            refreshCurrent();
    });
    refreshTimer_.start();
    connect(&files_, &FileService::loaded, this, [this](const FileResult& result) {
        loading_ = false;
        loadingPath_.clear();
        auto loaded = std::exchange(loadingDocument_, {});
        auto loadedEditor = std::move(loadingEditor_);
        if (!result.error.isEmpty()) {
            statusBar()->showMessage(result.error);
            if (result.sourceChanged)
                for (auto& document : documents_)
                    if (document.path == result.path)
                        document.autoRefresh = false;
            updateReaderStatus();
            loadNext();
            return;
        }
        for (std::size_t i = 0; i < documents_.size(); ++i)
            if (documents_[i].path == result.path) {
                auto& document = documents_[i];
                QString error;
                try {
                    if (document.ansiStyles.capacity() - document.ansiStyles.size() < loaded->styles.size())
                        document.ansiStyles.reserve(qMax(document.ansiStyles.size() + loaded->styles.size(),
                                                         document.ansiStyles.capacity() * 2));
                } catch (const std::bad_alloc&) {
                    statusBar()->showMessage("内存不足，无法追加日志颜色；已有内容保留。");
                    loadNext();
                    return;
                }
                if (!document.editor->appendLogUtf8(loaded->tail, loaded->styles, error)) {
                    statusBar()->showMessage(error);
                    loadNext();
                    return;
                }
                document.ansiStyles += loaded->styles;
                if (!loaded->tail.isEmpty())
                    ++document.revision;
                loaded->styles.clear();
                loaded->tail.clear();
                document.readState = loaded;
                document.cursor = result.cursor;
                document.diskSize = result.cursor.offset;
                document.diskModified = result.modified;
                document.lineEnding = result.lineEnding;
                statusBar()->clearMessage();
                updateReaderStatus();
                if (result.cursor.offset < result.snapshotSize && !pendingFiles_.contains(result.path))
                    pendingFiles_.push_back(result.path);
                loadNext();
                return;
            }
        if (loaded->tailMode) {
            loadNext(); // Its tab was closed while the detached read was running.
            return;
        }
        auto editor = std::move(loadedEditor);
        QElapsedTimer attachTimer;
        attachTimer.start();
        QString error;
        if (!editor->adoptLoadBuffer(*loaded->buffer, error)) {
            statusBar()->showMessage(error);
            loadNext();
            return;
        }
        editor->widget()->setProperty("loadAdoptMs", attachTimer.restart());
        editor->setTheme(dark_);
        editor->setReadOnly(true);
        editor->setWrap(readingWrap_);
        editor->setNumbers(readingNumbers_);
        editor->setAppearance(readingFont_, readingBackground_, readingForeground_);
        editor->widget()->setProperty("loadOptionsMs", attachTimer.restart());
        auto ansiStyles = std::move(loaded->styles);
        editor->setAnsiStyles(ansiStyles, readingColors_);
        editor->widget()->setProperty("loadStyleMs", attachTimer.restart());
        auto* widget = editor->widget();
        documents_.push_back({result.path, std::move(editor), 1, std::move(ansiStyles)});
        loaded->buffer.reset();
        documents_.back().readState = loaded;
        documents_.back().cursor = result.cursor;
        documents_.back().diskSize = result.cursor.offset;
        documents_.back().diskModified = result.modified;
        documents_.back().lineEnding = result.lineEnding;
        tabs_->addTab(widget, documentIcon(), QFileInfo(result.path).fileName());
        tabs_->setTabToolTip(tabs_->count() - 1, result.path);
        restoreDocumentState(tabs_->count() - 1);
        decorateTab(tabs_->count() - 1);
        tabs_->setCurrentIndex(tabs_->count() - 1);
        widget->setProperty("loadPublishMs", attachTimer.restart());
        statusBar()->clearMessage();
        updateReaderStatus();
        if (pendingHit_ && pendingHit_->path.compare(result.path, Qt::CaseInsensitive) == 0) {
            auto hit = *pendingHit_;
            pendingHit_.reset();
            activateHit(hit);
        }
        loadNext();
    });
    connect(results_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        if (item->data(0, Qt::UserRole).canConvert<SearchHit>())
            activateHit(qvariant_cast<SearchHit>(item->data(0, Qt::UserRole)));
    });
    clearResultsEachSearch_ = QSettings().value("logs/search/clearEachSearch", false).toBool();
}
void LogDocumentView::chooseFiles() {
    auto files = QFileDialog::getOpenFileNames(this, "打开日志", {},
                                               "日志与文本 (*.log *.txt *.cpp *.h *.json);;所有文件 (*)");
    for (const auto& path : files)
        openPath(path);
}
LogDocumentView::~LogDocumentView() {
    statusTimer_.stop();
    refreshTimer_.stop();
    disconnect(tabs_, nullptr, this, nullptr);
    disconnect(tabs_->tabBar(), nullptr, this, nullptr);
    tabs_->tabBar()->removeEventFilter(this);
    if (finder_)
        finder_->cancelSearch();
    files_.cancel();
    if (searchCancel_)
        searchCancel_->store(true);
}
void LogDocumentView::closeEvent(QCloseEvent* event) {
    cancelLoading();
    if (finder_)
        finder_->close();
    QMainWindow::closeEvent(event);
}
void LogDocumentView::cancelLoading() {
    if (!loading_)
        return;
    files_.cancel();
    pendingFiles_.clear();
    loadingDocument_.reset();
    loadingEditor_.reset();
    loadingPath_.clear();
    loading_ = false;
    statusBar()->showMessage("已取消日志加载", 3000);
}
bool LogDocumentView::eventFilter(QObject* watched, QEvent* event) {
    if (watched == tabs_->tabBar() && event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::MiddleButton) {
            closeDocument(tabs_->tabBar()->tabAt(mouse->position().toPoint()));
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}
void LogDocumentView::openPath(const QString& path) {
    show();
    raise();
    const auto canonical = QFileInfo(path).absoluteFilePath();
    auto recent = QSettings().value("logs/recent").toStringList();
    recent.removeAll(canonical);
    recent.prepend(canonical);
    while (recent.size() > 100)
        recent.removeLast();
    QSettings().setValue("logs/recent", recent);
    for (std::size_t i = 0; i < documents_.size(); ++i)
        if (documents_[i].path.compare(canonical, Qt::CaseInsensitive) == 0) {
            tabs_->setCurrentIndex(static_cast<int>(i));
            return;
        }
    if (!pendingFiles_.contains(canonical))
        pendingFiles_.push_back(canonical);
    loadNext();
}
void LogDocumentView::loadNext() {
    if (loading_)
        return;
    if (pendingFiles_.isEmpty()) {
        if (!restoreActivePath_.isEmpty()) {
            for (std::size_t i = 0; i < documents_.size(); ++i)
                if (documents_[i].path == restoreActivePath_)
                    tabs_->setCurrentIndex(static_cast<int>(i));
            restoreActivePath_.clear();
        }
        return;
    }
    loading_ = true;
    loadingPath_ = pendingFiles_.takeFirst();
    for (auto& document : documents_)
        if (document.path == loadingPath_) {
            if (!document.readState) {
                loading_ = false;
                statusBar()->showMessage("此页面缺少读取进度，请关闭标签后重新打开日志。");
                loadNext();
                return;
            }
            // The worker mutates only a detached checkpoint. Errors/cancellation
            // discard it, so retry never skips or duplicates source bytes.
            auto state = std::make_shared<LogReadState>(*document.readState);
            state->tailMode = true;
            state->tail.clear();
            state->styles.clear();
            loadingDocument_ = state;
            files_.open(
                loadingPath_,
                [state](QByteArrayView bytes, QString& error) { return state->append(bytes, error); },
                document.cursor, true);
            statusBar()->showMessage("正在读取新增日志…");
            return;
        }
    // A performance threshold, never an opening limit. Large documents omit
    // per-byte style storage and wrapping, as in Notepad++ large-file mode.
    const auto expectedBytes = QFileInfo(loadingPath_).size();
    const bool largeFile = expectedBytes >= 200LL * 1024 * 1024;
    loadingEditor_ = makeEditor(tabs_);
    auto state = std::make_shared<LogReadState>();
    state->largeFile = largeFile;
    QString error;
    state->buffer = loadingEditor_->createLoadBuffer(largeFile, error, expectedBytes);
    if (!state->buffer) {
        loading_ = false;
        loadingPath_.clear();
        loadingEditor_.reset();
        statusBar()->showMessage(error);
        loadNext();
        return;
    }
    loadingDocument_ = state;
    files_.open(
        loadingPath_, [state](QByteArrayView bytes, QString& error) { return state->append(bytes, error); },
        {}, true);
    statusBar()->showMessage(largeFile ? "正在打开日志（大文件模式：关闭颜色与自动换行）…" : "正在打开日志…");
}
void LogDocumentView::setTheme(bool dark) {
    for (int index = 0; index < tabs_->count(); ++index)
        tabs_->setTabIcon(index, documentIcon());
    for (auto* pin : findChildren<QToolButton*>("logTabPin"))
        pin->setIcon(pinIcon(pin->isChecked()));
    dark_ = dark;
    if (!readingAppearanceCustom_) {
        readingBackground_ = appearanceColor("surface", dark, QColor(dark ? "#171b20" : "#ffffff"));
        readingForeground_ = appearanceColor("text", dark, QColor(dark ? "#b8c3d3" : "#253345"));
    }
    for (auto& d : documents_)
        d.editor->setTheme(dark);
    for (auto& d : documents_)
        d.editor->setAppearance(readingFont_, readingBackground_, readingForeground_);
}
void LogDocumentView::addExample() {
    auto editor = makeEditor(tabs_, true);
    editor->setTheme(dark_);
    editor->setAppearance(readingFont_, readingBackground_, readingForeground_);
    editor->setText("// PortWave · C++20 / Qt 6 / QScintilla\n#include <iostream>\n\nint main() {\n    "
                    "std::cout << \"Hello, PortWave!\\n\";\n}\n");
    auto* widget = editor->widget();
    documents_.push_back({{}, std::move(editor)});
    tabs_->addTab(widget, documentIcon(), "C++20 编辑示例");
    decorateTab(tabs_->count() - 1);
    show();
}
EditorInterface* LogDocumentView::currentEditor() const {
    const auto index = tabs_->currentIndex();
    return index >= 0 && static_cast<std::size_t>(index) < documents_.size()
               ? documents_[static_cast<std::size_t>(index)].editor.get()
               : nullptr;
}
QVector<SearchDocument> LogDocumentView::searchSources(bool all) const {
    QVector<SearchDocument> sources;
    for (std::size_t i = 0; i < documents_.size(); ++i) {
        if (!all && static_cast<int>(i) != tabs_->currentIndex())
            continue;
        const auto& document = documents_[i];
        SearchDocument source;
        source.id = document.path.isEmpty() ? "example:" + QString::number(i) : document.path;
        source.path = document.path.isEmpty() ? "C++20 编辑示例" : document.path;
        source.text = document.editor->text();
        source.revision = document.revision;
        source.anchor = document.editor->position();
        auto selection = document.editor->selection();
        source.selectionStart = selection.first;
        source.selectionEnd = selection.second;
        sources.push_back(std::move(source));
    }
    return sources;
}
bool LogDocumentView::activateHit(const SearchHit& hit) {
    for (std::size_t i = 0; i < documents_.size(); ++i) {
        auto& document = documents_[i];
        const auto id = document.path.isEmpty() ? "example:" + QString::number(i) : document.path;
        if (id.compare(hit.documentId, Qt::CaseInsensitive) != 0)
            continue;
        if (document.editor->text().mid(hit.position, hit.length) != hit.matched) {
            statusBar()->showMessage("此检索结果已失效，请重新检索");
            return false;
        }
        tabs_->setCurrentIndex(static_cast<int>(i));
        document.editor->selectRange(hit.position, hit.length);
        show();
        raise();
        return true;
    }
    if (QFileInfo::exists(hit.path)) {
        pendingHit_ = hit;
        openPath(hit.path);
        return true;
    }
    statusBar()->showMessage("结果对应的文件不存在");
    return false;
}
void LogDocumentView::applyMarks(const QVector<TextMark>& marks, bool clearPrevious) {
    if (auto* editor = currentEditor())
        editor->mark(marks, clearPrevious);
}
void LogDocumentView::setReadingOptions(bool wrap, bool numbers, bool colors) {
    readingColors_ = colors;
    readingWrap_ = wrap;
    readingNumbers_ = numbers;
    for (auto& document : documents_) {
        document.editor->setWrap(wrap);
        document.editor->setNumbers(numbers);
        document.editor->setAnsiStyles(document.ansiStyles, readingColors_);
    }
}
void LogDocumentView::setReadingAppearance(const QFont& font, const QColor& background,
                                           const QColor& foreground) {
    readingAppearanceCustom_ = true;
    readingFont_ = font;
    readingBackground_ = background;
    readingForeground_ = foreground;
    if (finder_)
        finder_->setMarkBackground(background);
    for (auto& document : documents_)
        document.editor->setAppearance(font, background, foreground);
}
void LogDocumentView::showFindOptions() {
    search();
}
void LogDocumentView::search() {
    if (!finder_) {
        finder_ = new FindDialog([this](bool all) { return searchSources(all); },
                                 [this](const SearchHit& hit) { return activateHit(hit); }, false,
                                 preferenceKey_, this);
        finder_->setLogHistoryEnabled(!clearResultsEachSearch_);
        finder_->setShortcutOverrides(shortcutOverrides_);
        connect(finder_, &FindDialog::resultsChanged, this, &LogDocumentView::appendResults);
        connect(finder_, &FindDialog::allSearchFinished, this, [this](int total, int searchedDocuments) {
            if (resultGroup_) {
                const auto options = finder_->options();
                const auto mode = options.mode == 2 ? "正则表达式" : options.mode == 1 ? "扩展" : "普通";
                const int files = resultFiles_.size();
                resultSummary_->setText(
                    QString("搜索结果   %1 · %2 个文件 · %3 处匹配").arg(resultQuery_).arg(files).arg(total));
                resultGroup_->setText(0, QString("搜索“%1”（%2 个文件中匹配 %3 次，共检索 %4 个文件）[%5]")
                                             .arg(resultQuery_)
                                             .arg(files)
                                             .arg(total)
                                             .arg(searchedDocuments)
                                             .arg(mode));
                if (total == 0) {
                    auto* empty = new QTreeWidgetItem(resultGroup_, {"未找到匹配内容"});
                    empty->setFlags(empty->flags() & ~Qt::ItemIsSelectable);
                    resultGroup_->setExpanded(true);
                }
                results_->scrollToItem(resultGroup_, QAbstractItemView::PositionAtTop);
            }
            const bool wasHidden = resultPanel_->isHidden();
            resultPanel_->show();
            if (wasHidden) {
                const int available = splitter_->height();
                const int resultsHeight = qBound(154, available / 5, 220);
                splitter_->setSizes({qMax(0, available - resultsHeight), resultsHeight});
            }
        });
        connect(finder_, &FindDialog::marksRequested, this, &LogDocumentView::applyMarks);
        connect(finder_, &FindDialog::clearMarksRequested, this, [this] {
            if (auto* editor = currentEditor())
                editor->clearMarks();
        });
        connect(finder_, &FindDialog::copyMarksRequested, this, [this] {
            if (auto* editor = currentEditor())
                QApplication::clipboard()->setText(editor->markedText());
        });
    }
    QString selected;
    if (restoredFindOptions_) {
        finder_->setOptions(*restoredFindOptions_);
        restoredFindOptions_.reset();
    }
    if (auto* editor = currentEditor()) {
        auto range = editor->selection();
        selected = editor->text().mid(range.first, range.second - range.first);
    }
    finder_->setMarkBackground(readingBackground_);
    finder_->openFind(selected);
}
void LogDocumentView::appendResults(const QVector<SearchHit>& hits, const QString& query, bool append) {
    if (hits.isEmpty()) {
        for (auto& document : documents_) {
            document.editor->highlightMatches({});
        }
        if (!append) {
            results_->clear();
            resultGroup_ = nullptr;
            resultFiles_.clear();
        }
        resultQuery_ = query;
        resultGroup_ = new QTreeWidgetItem(results_, {QString("搜索“%1” · 正在检索…").arg(query)});
        resultFiles_.clear();
        resultGroup_->setExpanded(true);
        focusResultGroup();
        while (results_->topLevelItemCount() > 20)
            delete results_->takeTopLevelItem(0);
        return;
    }
    if (!resultGroup_) {
        resultGroup_ = new QTreeWidgetItem(results_, {query});
        resultGroup_->setExpanded(true);
        focusResultGroup();
    }
    QHash<QString, QVector<TextMark>> overlays;
    for (const auto& hit : hits) {
        for (auto& document : documents_)
            if (document.path == hit.documentId && document.revision == hit.revision)
                overlays[document.path].push_back({hit.position, hit.length, 0, false});
        auto* file = resultFiles_.value(hit.documentId, nullptr);
        if (!file) {
            file = new QTreeWidgetItem(resultGroup_, {QFileInfo(hit.path).fileName()});
            file->setData(0, Qt::UserRole + 1, hit.path);
            file->setToolTip(0, hit.path);
            file->setExpanded(true);
            resultFiles_.insert(hit.documentId, file);
        }
        auto* item = new QTreeWidgetItem(file, {QString("行 %1: %2").arg(hit.line + 1).arg(hit.preview)});
        item->setData(0, Qt::UserRole, QVariant::fromValue(hit));
    }
    for (auto& document : documents_)
        if (overlays.contains(document.path))
            document.editor->highlightMatches(overlays.value(document.path), true);
}

void LogDocumentView::showResultsMenu(const QPoint& position) {
    const auto point = results_->viewport()->mapToGlobal(position);
    if (auto* item = results_->itemAt(position); item && !item->isSelected())
        results_->setCurrentItem(item);
    QMenu menu(this);
    menu.addAction("在此搜索结果中查找…", this, &LogDocumentView::findInResults);
    auto* markAll = menu.addMenu("标记全部结果");
    markAll->setEnabled(results_->currentItem() != nullptr);
    for (int color = 0; color < 8; ++color) {
        auto* action = markAll->addAction(markSwatch(color, dark_, readingBackground_),
                                          markAppearance(color, dark_, readingBackground_).name);
        action->setObjectName("markAllResults" + QString::number(color));
        connect(action, &QAction::triggered, this, [this, color] {
            auto* group = results_->currentItem();
            if (!group)
                return;
            while (group->parent())
                group = group->parent();
            QHash<QString, QVector<TextMark>> byFile;
            for (QTreeWidgetItemIterator it(group); *it; ++it) {
                auto* item = *it;
                auto* root = item;
                while (root->parent())
                    root = root->parent();
                if (root != group)
                    break;
                if (item->data(0, Qt::UserRole).canConvert<SearchHit>()) {
                    const auto hit = qvariant_cast<SearchHit>(item->data(0, Qt::UserRole));
                    for (const auto& document : documents_)
                        if (document.path == hit.documentId && document.revision == hit.revision)
                            byFile[document.path].push_back({hit.position, hit.length, color, false});
                }
            }
            int total = 0;
            for (auto& document : documents_)
                if (byFile.contains(document.path)) {
                    document.editor->mark(byFile[document.path], false);
                    total += byFile[document.path].size();
                }
            statusBar()->showMessage(QString("已标记 %1 处；失效或未打开文件的结果不写入正文").arg(total),
                                     4000);
        });
    }

    menu.addSeparator();
    menu.addAction("全部折叠", results_, &QTreeWidget::collapseAll);
    menu.addAction("全部展开", results_, &QTreeWidget::expandAll);
    menu.addSeparator();
    menu.addAction("复制选中行", this, &LogDocumentView::copySelectedResults)
        ->setEnabled(!results_->selectedItems().isEmpty());
    QStringList paths;
    for (auto* item : results_->selectedItems()) {
        if (item->data(0, Qt::UserRole).canConvert<SearchHit>())
            paths.push_back(qvariant_cast<SearchHit>(item->data(0, Qt::UserRole)).path);
        else if (!item->data(0, Qt::UserRole + 1).toString().isEmpty())
            paths.push_back(item->data(0, Qt::UserRole + 1).toString());
        else if (!item->parent())
            for (int i = 0; i < item->childCount(); ++i)
                paths.push_back(item->child(i)->data(0, Qt::UserRole + 1).toString());
    }
    paths.removeDuplicates();
    menu.addAction("复制选择的文件路径", this,
                   [paths] { QApplication::clipboard()->setText(paths.join('\n')); })
        ->setEnabled(!paths.isEmpty());
    menu.addAction("全选", results_, &QTreeWidget::selectAll);
    menu.addAction("全部清除", this, [this] {
        results_->clear();
        resultGroup_ = nullptr;
        resultFiles_.clear();
    });
    menu.addAction("打开选择的文件路径", this,
                   [paths] {
                       for (const auto& path : paths)
                           if (QFileInfo::exists(path))
                               QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
                   })
        ->setEnabled(!paths.isEmpty());
    menu.addAction("清除此组搜索结果", this, &LogDocumentView::removeCurrentResultGroup)
        ->setEnabled(results_->currentItem() != nullptr);
    menu.addSeparator();
    auto* wrap = menu.addAction("自动换行");
    wrap->setCheckable(true);
    wrap->setChecked(results_->wordWrap());
    connect(wrap, &QAction::toggled, results_, &QTreeWidget::setWordWrap);
    auto* clearEach = menu.addAction("每次搜索时清理");
    clearEach->setCheckable(true);
    clearEach->setChecked(clearResultsEachSearch_);
    connect(clearEach, &QAction::toggled, this, [this](bool enabled) {
        clearResultsEachSearch_ = enabled;
        QSettings().setValue("logs/search/clearEachSearch", enabled);
        if (finder_)
            finder_->setLogHistoryEnabled(!enabled);
    });
    menu.exec(point);
}

void LogDocumentView::copySelectedResults() {
    QStringList lines;
    for (auto* item : results_->selectedItems()) {
        if (item->data(0, Qt::UserRole).canConvert<SearchHit>()) {
            const auto hit = qvariant_cast<SearchHit>(item->data(0, Qt::UserRole));
            lines.push_back(QString("行 %1: %2").arg(hit.line + 1).arg(hit.preview));
        } else {
            lines.push_back(item->text(0));
        }
    }
    QApplication::clipboard()->setText(lines.join('\n'));
}

void LogDocumentView::findInResults() {
    bool accepted = false;
    const auto query = QInputDialog::getText(this, "在搜索结果中查找", "查找文字：", QLineEdit::Normal,
                                             resultFindText_, &accepted);
    if (!accepted || query.isEmpty())
        return;
    resultFindText_ = query;
    QList<QTreeWidgetItem*> items;
    for (QTreeWidgetItemIterator it(results_); *it; ++it)
        items.push_back(*it);
    if (items.isEmpty())
        return;
    const int start = items.indexOf(results_->currentItem()) + 1;
    for (int offset = 0; offset < items.size(); ++offset) {
        auto* item = items[(start + offset) % items.size()];
        if (!item->text(0).contains(query, Qt::CaseInsensitive))
            continue;
        for (auto* parent = item->parent(); parent; parent = parent->parent())
            parent->setExpanded(true);
        results_->setCurrentItem(item);
        results_->scrollToItem(item);
        return;
    }
    statusBar()->showMessage("搜索结果中未找到：" + query, 5000);
}

void LogDocumentView::removeCurrentResultGroup() {
    auto* root = results_->currentItem();
    while (root && root->parent())
        root = root->parent();
    if (!root)
        return;
    if (root == resultGroup_) {
        resultGroup_ = nullptr;
        resultFiles_.clear();
    }
    const int index = results_->indexOfTopLevelItem(root);
    if (index >= 0)
        delete results_->takeTopLevelItem(index);
}
void LogDocumentView::closeDocument(int index) {
    if (index < 0 || index >= static_cast<int>(documents_.size()))
        return;
    if (finder_)
        finder_->cancelSearch();
    {
        QSignalBlocker blocker(tabs_);
        tabs_->removeTab(index);
        documents_.erase(documents_.begin() + index);
    }
    updateReaderStatus();
}
void LogDocumentView::decorateTab(int index) {
    if (index < 0 || index >= static_cast<int>(documents_.size()))
        return;
    auto* widget = tabs_->widget(index);
    auto* controls = new QWidget(tabs_->tabBar());
    auto* row = new QHBoxLayout(controls);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    auto* pin = new QToolButton;
    pin->setObjectName("logTabPin");
    pin->setToolTip("固定此日志标签");
    pin->setCheckable(true);
    pin->setChecked(documents_[static_cast<std::size_t>(index)].pinned);
    pin->setIcon(pinIcon(pin->isChecked()));
    pin->setFixedSize(22, 26);
    row->addWidget(pin);
    auto* close = new QToolButton;
    close->setObjectName("closeLogDocument");
    close->setToolTip("关闭日志标签");
    close->setText("×");
    close->setFixedSize(22, 26);
    row->addWidget(close);
    tabs_->tabBar()->setTabButton(index, QTabBar::RightSide, controls);
    connect(close, &QToolButton::clicked, this, [this, widget] { closeDocument(tabs_->indexOf(widget)); });
    connect(pin, &QToolButton::toggled, this, [this, widget, pin](bool enabled) {
        int index = tabs_->indexOf(widget);
        if (index < 0)
            return;
        documents_[static_cast<std::size_t>(index)].pinned = enabled;
        pin->setIcon(pinIcon(enabled));
        int destination = 0;
        for (int i = 0; i < static_cast<int>(documents_.size()); ++i)
            if (documents_[static_cast<std::size_t>(i)].pinned)
                tabs_->tabBar()->moveTab(i, destination++);
    });
}
void LogDocumentView::refreshCurrent() {
    const int index = tabs_->currentIndex();
    if (index < 0 || loading_)
        return;
    const auto path = documents_[static_cast<std::size_t>(index)].path;
    if (!path.isEmpty() && !pendingFiles_.contains(path))
        pendingFiles_.push_back(path);
    loadNext();
}
void LogDocumentView::updateReaderStatus() {
    const int index = tabs_->currentIndex();
    const bool valid = index >= 0 && index < static_cast<int>(documents_.size());
    autoRefresh_->setEnabled(valid);
    const QSignalBlocker blocker(autoRefresh_);
    autoRefresh_->setChecked(valid && documents_[static_cast<std::size_t>(index)].autoRefresh);
    if (!valid) {
        location_->clear();
        return;
    }
    const auto& document = documents_[static_cast<std::size_t>(index)];
    const auto view = document.editor->viewState();
    location_->setText(QString("行 %1, 列 %2    UTF-8    %3    %4 行")
                           .arg(view.line + 1)
                           .arg(view.column + 1)
                           .arg(document.lineEnding)
                           .arg(QLocale().toString(view.lineCount)));
    location_->setToolTip(document.path);
}
void LogDocumentView::setPreferenceKey(const QString& key) {
    preferenceKey_ = key;
    if (finder_)
        finder_->setPreferenceKey(key);
}
void LogDocumentView::setShortcutOverrides(const QHash<QString, QKeySequence>& overrides) {
    shortcutOverrides_ = overrides;
    for (auto it = shortcutActions_.cbegin(); it != shortcutActions_.cend(); ++it) {
        const auto sequence = overrides.value(it.key(), it.value()->shortcut());
        it.value()->setShortcut(sequence);
        it.value()->setEnabled(!sequence.isEmpty());
    }
    if (finder_)
        finder_->setShortcutOverrides(overrides);
}
void LogDocumentView::focusResultGroup() {
    if (!results_)
        return;
    // Newest results stay open; older groups fold so the tree does not keep growing
    // with every search while still keeping their hits available.
    for (int index = 0; index < results_->topLevelItemCount(); ++index) {
        auto* item = results_->topLevelItem(index);
        item->setExpanded(item == resultGroup_);
    }
}
void LogDocumentView::hideFinder() {
    if (finder_)
        finder_->close();
}
QJsonObject LogDocumentView::snapshot() const {
    QJsonArray files;
    QSet<QString> captured;
    for (const auto& doc : documents_) {
        if (doc.path.isEmpty())
            continue;
        const auto view = doc.editor->viewState();
        files.append(QJsonObject{{"path", doc.path},
                                 {"pinned", doc.pinned},
                                 {"line", view.line},
                                 {"column", view.column},
                                 {"scroll", view.firstVisibleLine},
                                 {"horizontal", view.horizontalOffset}});
        captured.insert(doc.path);
    }
    for (auto it = restoreStates_.cbegin(); it != restoreStates_.cend(); ++it)
        if (!captured.contains(it.key())) {
            files.append(it.value());
            captured.insert(it.key());
        }
    for (const auto& path : pendingFiles_)
        if (!captured.contains(path)) {
            files.append(QJsonObject{{"path", path}});
            captured.insert(path);
        }
    if (!loadingPath_.isEmpty() && !captured.contains(loadingPath_))
        files.append(QJsonObject{{"path", loadingPath_}});
    QString active = restoreActivePath_;
    if (tabs_->currentIndex() >= 0 && tabs_->currentIndex() < static_cast<int>(documents_.size()))
        active = documents_[static_cast<std::size_t>(tabs_->currentIndex())].path;
    QJsonObject result{{"files", files},
                       {"active", active},
                       {"wrap", readingWrap_},
                       {"numbers", readingNumbers_},
                       {"colors", readingColors_},
                       {"font", readingFont_.toString()},
                       {"background", readingBackground_.name()},
                       {"foreground", readingForeground_.name()}};
    if (finder_ || restoredFindOptions_) {
        const auto options = finder_ ? finder_->options() : *restoredFindOptions_;
        result["find"] = QJsonObject{{"query", options.query},        {"mode", options.mode},
                                     {"case", options.caseSensitive}, {"whole", options.wholeWord},
                                     {"dotall", options.dotAll},      {"backward", options.backward},
                                     {"wrap", options.wrap}};
    }
    return result;
}
void LogDocumentView::restore(const QJsonObject& state) {
    readingWrap_ = state.value("wrap").toBool(true);
    readingNumbers_ = state.value("numbers").toBool(true);
    readingColors_ = state.value("colors").toBool(true);
    if (state.contains("font"))
        readingFont_.fromString(state.value("font").toString());
    if (state.contains("background"))
        readingBackground_ = QColor(state.value("background").toString());
    if (state.contains("foreground"))
        readingForeground_ = QColor(state.value("foreground").toString());
    for (const auto& value : state.value("files").toArray()) {
        const auto row = value.toObject();
        const auto path = row.value("path").toString();
        if (!path.isEmpty() && QFileInfo::exists(path)) {
            restoreStates_.insert(QFileInfo(path).absoluteFilePath(), row);
            openPath(path);
        }
    }
    restoreActivePath_ = state.value("active").toString();
    if (state.contains("find")) {
        const auto row = state.value("find").toObject();
        restoredFindOptions_ = SearchOptions{row.value("query").toString(), row.value("mode").toInt(),
                                             row.value("case").toBool(),    row.value("whole").toBool(),
                                             row.value("dotall").toBool(),  row.value("backward").toBool(),
                                             row.value("wrap").toBool(true)};
    }
}
void LogDocumentView::restoreDocumentState(int index) {
    auto& doc = documents_[static_cast<std::size_t>(index)];
    if (!restoreStates_.contains(doc.path))
        return;
    const auto row = restoreStates_.take(doc.path);
    doc.pinned = row.value("pinned").toBool();
    doc.editor->restoreViewState({row.value("line").toInt(), row.value("column").toInt(), 0,
                                  row.value("scroll").toInt(), row.value("horizontal").toInt()});
}
void LogDocumentView::cloneFrom(const LogDocumentView& source) {
    readingWrap_ = source.readingWrap_;
    readingNumbers_ = source.readingNumbers_;
    readingColors_ = source.readingColors_;
    readingFont_ = source.readingFont_;
    readingBackground_ = source.readingBackground_;
    readingForeground_ = source.readingForeground_;
    for (const auto& doc : source.documents_) {
        auto editor = makeEditor(tabs_);
        editor->setTheme(dark_);
        editor->setReadOnly(true);
        editor->setWrap(readingWrap_);
        editor->setNumbers(readingNumbers_);
        editor->setAppearance(readingFont_, readingBackground_, readingForeground_);
        editor->setText(doc.editor->text());
        editor->setAnsiStyles(doc.ansiStyles, readingColors_);
        editor->mark(doc.editor->marks(), true);
        editor->restoreViewState(doc.editor->viewState());
        auto* widget = editor->widget();
        documents_.push_back({doc.path, std::move(editor), doc.revision, doc.ansiStyles, doc.pinned, false,
                              doc.diskSize, doc.diskModified, doc.lineEnding, doc.cursor, doc.readState});
        tabs_->addTab(widget, documentIcon(), QFileInfo(doc.path).fileName());
        tabs_->setTabToolTip(tabs_->count() - 1, doc.path);
        decorateTab(tabs_->count() - 1);
    }
    tabs_->setCurrentIndex(source.tabs_->currentIndex());
    for (int i = 0; i < source.results_->topLevelItemCount(); ++i)
        results_->addTopLevelItem(source.results_->topLevelItem(i)->clone());
    resultPanel_->setVisible(!source.resultPanel_->isHidden());
    splitter_->setSizes(source.splitter_->sizes());
    if (source.finder_) {
        restoredFindOptions_ = source.finder_->options();
    } else if (source.restoredFindOptions_) {
        restoredFindOptions_ = source.restoredFindOptions_;
    }
}
} // namespace portwave
