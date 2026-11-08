#include "features/log_workspace.h"
#include "features/window_caption.h"
#include "infrastructure/log_workspace_store.h"
#include <QApplication>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QUuid>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <functional>

namespace portwave {
namespace {
class FloatingLogWindow final : public QMainWindow {
  public:
    using QMainWindow::QMainWindow;
    std::function<void()> returnToHost;

  protected:
    void closeEvent(QCloseEvent* event) override {
        event->ignore();
        if (returnToHost)
            returnToHost();
    }
};
} // namespace
LogWorkspace::LogWorkspace(QWidget* parent) : QMainWindow(parent) {
    setWindowIcon(QApplication::windowIcon());
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setObjectName("logWorkspaceHost");
    setProperty("captionText", "PortWave · 日志工作区");
    resize(1100, 760);
    setMinimumSize(480, 300);
    auto* shell = new QWidget;
    auto* layout = new QVBoxLayout(shell);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);
    auto* caption = new WindowCaption(this, "默认工作区", 32);
    layout->addWidget(caption);
    auto* workspaceRow = new QWidget;
    workspaceRow->setObjectName("logWorkspaceRow");
    workspaceRow->setFixedHeight(40);
    auto* tabs = new QHBoxLayout(workspaceRow);
    tabs->setContentsMargins(10, 0, 6, 0);
    tabs->setSpacing(0);
    workspaceTabs_ = new QTabBar;
    workspaceTabs_->setObjectName("logWorkspaceTabs");
    workspaceTabs_->setExpanding(false);
    workspaceTabs_->setMovable(true);
    workspaceTabs_->setDrawBase(false);
    workspaceTabs_->setElideMode(Qt::ElideMiddle);
    workspaceTabs_->setUsesScrollButtons(true);
    workspaceTabs_->installEventFilter(this);
    tabs->addWidget(workspaceTabs_, 1);
    menuButton_ = new QToolButton;
    menuButton_->setObjectName("workspaceMenuButton");
    menuButton_->setText("⌄");
    menuButton_->setIconSize(QSize(14, 14));
    menuButton_->setIcon(QIcon(":/assets/step-down-dark.svg"));
    menuButton_->setToolTip("日志工作区");
    menuButton_->setFixedSize(26, 26);
    menuButton_->setPopupMode(QToolButton::InstantPopup);
    menu_ = new QMenu(this);
    menu_->setObjectName("logWorkspaceMenu");
    menuButton_->setMenu(menu_);
    tabs->addWidget(menuButton_);
    layout->addWidget(workspaceRow);
    pages_ = new QStackedWidget;
    pages_->setObjectName("logWorkspacePages");
    layout->addWidget(pages_, 1);
    setCentralWidget(shell);
    connect(menu_, &QMenu::aboutToShow, this, &LogWorkspace::fillMenu);
    connect(workspaceTabs_, &QTabBar::currentChanged, this, &LogWorkspace::activateIndex);
    connect(workspaceTabs_, &QTabBar::tabMoved, this, [this](int from, int to) {
        auto moved = std::move(workspaces_[static_cast<std::size_t>(from)]);
        workspaces_.erase(workspaces_.begin() + from);
        workspaces_.insert(workspaces_.begin() + to, std::move(moved));
        activeIndex_ = workspaceTabs_->currentIndex();
    });
    workspaceTabs_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(workspaceTabs_, &QWidget::customContextMenuRequested, this, [this](QPoint point) {
        const int index = workspaceTabs_->tabAt(point);
        if (index < 0)
            return;
        const auto id = workspaces_[static_cast<std::size_t>(index)].id;
        QMenu menu(this);
        menu.addAction("在新窗口打开", this, [this, id] { detachWorkspace(id); });
        menu.addAction("恢复单窗口", this, &LogWorkspace::restoreSingleWindow);
        menu.addSeparator();
        menu.addAction("重命名工作区…", this, [this, id] {
            bool ok = false;
            const auto name = QInputDialog::getText(this, "重命名工作区", "工作区名称：", QLineEdit::Normal,
                                                    windowTitle(), &ok);
            if (ok)
                renameWorkspace(id, name);
        });
        menu.addAction("保存 / 更新工作区与布局", this, &LogWorkspace::saveWorkspaces);
        menu.exec(workspaceTabs_->mapToGlobal(point));
    });
    const auto state = LogWorkspaceStore::load();
    for (const auto& value : state.value("workspaces").toArray()) {
        const auto row = value.toObject();
        const auto id = createWorkspace(row.value("name").toString());
        if (id.isEmpty())
            break;
        auto& workspace = workspaces_.back();
        workspace.id = row.value("id").toString(id);
        workspace.connectionId = row.value("connection").toString();
        workspace.automaticName = row.value("automaticName").toBool();
        workspace.saved = row.value("state").toObject();
        workspace.restored = false;
        workspace.restoreFloating = row.value("floating").toBool();
        workspace.floatingGeometry =
            QByteArray::fromBase64(row.value("floatingGeometry").toString().toLatin1());
        workspace.view->setPreferenceKey("search/log/" + workspace.id);
    }
    if (workspaces_.empty())
        createWorkspace("默认工作区");
    const auto active = state.value("active").toString();
    rebuildTabs();
    if (!active.isEmpty())
        activateWorkspace(active);
    else
        activateIndex(0);
    if (state.contains("geometry"))
        restoreGeometry(QByteArray::fromBase64(state.value("geometry").toString().toLatin1()));
}
LogWorkspace::~LogWorkspace() {
    disconnect(workspaceTabs_, nullptr, this, nullptr);
    for (auto& workspace : workspaces_)
        if (workspace.floating)
            static_cast<FloatingLogWindow*>(workspace.floating.data())->returnToHost = {};
}
LogWorkspace::Workspace* LogWorkspace::current() {
    return activeIndex_ >= 0 && activeIndex_ < static_cast<int>(workspaces_.size())
               ? &workspaces_[static_cast<std::size_t>(activeIndex_)]
               : nullptr;
}
LogDocumentView* LogWorkspace::activeView() const {
    return activeIndex_ >= 0 && activeIndex_ < static_cast<int>(workspaces_.size())
               ? workspaces_[static_cast<std::size_t>(activeIndex_)].view
               : nullptr;
}
QString LogWorkspace::createWorkspace(const QString& name, bool copyCurrent) {
    if (workspaces_.size() >= 128 || name.trimmed().isEmpty())
        return {};
    auto* source = activeView();
    const auto connection = current() ? current()->connectionId : QString{};
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto* view = new LogDocumentView;
    view->setPreferenceKey("search/log/" + id);
    view->setTheme(dark_);
    view->setReadingOptions(wrap_, numbers_, colors_);
    view->setReadingAppearance(readingFont_, readingBackground_, readingForeground_);
    view->setShortcutOverrides(shortcutOverrides_);
    pages_->addWidget(view);
    workspaces_.push_back({id, name.trimmed(), connection, view, {}, {}, true, false});
    if (copyCurrent && source)
        view->cloneFrom(*source);
    rebuildTabs();
    activateWorkspace(id);
    return id;
}
void LogWorkspace::setShortcutOverrides(const QHash<QString, QKeySequence>& overrides) {
    shortcutOverrides_ = overrides;
    for (auto& workspace : workspaces_)
        if (workspace.view)
            workspace.view->setShortcutOverrides(overrides);
}
void LogWorkspace::activateIndex(int index) {
    if (switching_ || index < 0 || index >= static_cast<int>(workspaces_.size()))
        return;
    if (auto* previous = activeView(); previous && activeIndex_ != index)
        previous->hideFinder();
    activeIndex_ = index;
    auto& workspace = workspaces_[static_cast<std::size_t>(index)];
    if (!workspace.floating)
        pages_->setCurrentWidget(workspace.view);
    if (!workspace.restored) {
        workspace.restored = true;
        workspace.view->restore(workspace.saved);
        // A lazy workspace snapshot must not overwrite the current theme.
        if (customReadingAppearance_)
            workspace.view->setReadingAppearance(readingFont_, readingBackground_, readingForeground_);
    }
    setWindowTitle(workspace.name);
    if (workspace.floating) {
        workspace.floating->show();
        workspace.floating->raise();
    }
}
void LogWorkspace::activateWorkspace(const QString& id) {
    for (int i = 0; i < static_cast<int>(workspaces_.size()); ++i)
        if (workspaces_[static_cast<std::size_t>(i)].id == id) {
            workspaceTabs_->setCurrentIndex(i);
            activateIndex(i);
            return;
        }
}
void LogWorkspace::rebuildTabs() {
    const QSignalBlocker blocker(workspaceTabs_);
    while (workspaceTabs_->count())
        workspaceTabs_->removeTab(0);
    for (const auto& workspace : workspaces_) {
        const int index = workspaceTabs_->addTab(workspace.name);
        workspaceTabs_->setTabData(index, workspace.id);
        workspaceTabs_->setTabVisible(index, !workspace.floating);
    }
    if (activeIndex_ >= 0 && activeIndex_ < workspaceTabs_->count())
        workspaceTabs_->setCurrentIndex(activeIndex_);
}
void LogWorkspace::registerConnection(const QString& id, const QString& name) {
    bool found = false, renamed = false;
    for (auto& workspace : workspaces_)
        if (workspace.connectionId == id) {
            found = true;
            if (workspace.automaticName && workspace.name != name) {
                workspace.name = name;
                renamed = true;
            }
        }
    if (found) {
        if (!renamed)
            return;
        rebuildTabs();
        if (current())
            setWindowTitle(current()->name);
        return;
    }
    if (workspaces_.size() == 1 && workspaces_[0].connectionId.isEmpty() &&
        workspaces_[0].name == "默认工作区" && workspaces_[0].view->searchSources(true).isEmpty()) {
        workspaces_[0].connectionId = id;
        workspaces_[0].name = name;
        workspaces_[0].automaticName = true;
        rebuildTabs();
        setWindowTitle(name);
        return;
    }
    const auto previous = current() ? current()->id : QString{};
    createWorkspace(name);
    if (auto* workspace = current()) {
        workspace->connectionId = id;
        workspace->automaticName = true;
    }
    if (!previous.isEmpty())
        activateWorkspace(previous);
}
void LogWorkspace::activateConnection(const QString& id, const QString& name) {
    registerConnection(id, name);
    for (const auto& workspace : workspaces_)
        if (workspace.connectionId == id) {
            activateWorkspace(workspace.id);
            return;
        }
}
void LogWorkspace::openConnectionLog(const QString& id, const QString& name, const QString& path) {
    activateConnection(id, name);
    openPath(path);
}
void LogWorkspace::openPath(const QString& path) {
    show();
    raise();
    if (activeView())
        activeView()->openPath(path);
}
void LogWorkspace::search() {
    show();
    if (activeView())
        activeView()->search();
}
void LogWorkspace::addExample() {
    show();
    if (activeView())
        activeView()->addExample();
}
QVector<SearchDocument> LogWorkspace::searchSources(bool all) const {
    return activeView() ? activeView()->searchSources(all) : QVector<SearchDocument>{};
}
bool LogWorkspace::activateHit(const SearchHit& hit) {
    return activeView() && activeView()->activateHit(hit);
}
void LogWorkspace::applyMarks(const QVector<TextMark>& marks, bool clear) {
    if (activeView())
        activeView()->applyMarks(marks, clear);
}
void LogWorkspace::setTheme(bool dark) {
    menuButton_->setIcon(QIcon(dark ? ":/assets/step-down-dark.svg" : ":/assets/step-down-light.svg"));
    dark_ = dark;
    if (!customReadingAppearance_) {
        readingBackground_ = appearanceColor("surface", dark, QColor(dark ? "#171b20" : "#ffffff"));
        readingForeground_ = appearanceColor("text", dark, QColor(dark ? "#b8c3d3" : "#253345"));
    }
    for (auto& workspace : workspaces_) {
        workspace.view->setTheme(dark);
        workspace.view->setReadingAppearance(readingFont_, readingBackground_, readingForeground_);
    }
}
void LogWorkspace::setReadingOptions(bool wrap, bool numbers, bool colors) {
    wrap_ = wrap;
    numbers_ = numbers;
    colors_ = colors;
    if (activeView())
        activeView()->setReadingOptions(wrap, numbers, colors);
}
void LogWorkspace::setReadingAppearance(const QFont& font, const QColor& background,
                                        const QColor& foreground) {
    customReadingAppearance_ = true;
    readingFont_ = font;
    readingBackground_ = background;
    readingForeground_ = foreground;
    for (auto& workspace : workspaces_)
        workspace.view->setReadingAppearance(font, background, foreground);
}
void LogWorkspace::renameWorkspace(const QString& id, const QString& name) {
    if (name.trimmed().isEmpty())
        return;
    for (auto& workspace : workspaces_)
        if (workspace.id == id) {
            workspace.name = name.trimmed();
            workspace.automaticName = false;
            if (workspace.floating)
                workspace.floating->setWindowTitle(workspace.name);
        }
    rebuildTabs();
    if (current())
        setWindowTitle(current()->name);
    saveWorkspaces();
}
void LogWorkspace::promptCreate(bool copy) {
    bool ok = false;
    QString suggested = current() ? current()->name : "工作区";
    int suffix = 2;
    auto exists = [this](const QString& name) {
        return std::any_of(workspaces_.begin(), workspaces_.end(),
                           [&](const auto& workspace) { return workspace.name == name; });
    };
    const auto base = suggested;
    while (exists(suggested))
        suggested = base + QString(" (%1)").arg(suffix++);
    const auto name = QInputDialog::getText(this, copy ? "另存为新工作区" : "新建工作区", "工作区名称：",
                                            QLineEdit::Normal, suggested, &ok);
    if (ok && !createWorkspace(name, copy).isEmpty())
        saveWorkspaces();
}
void LogWorkspace::fillMenu() {
    menu_->clear();
    auto* open = menu_->addAction(folderIcon(), "打开日志…\tCtrl+O", this, [this] {
        if (activeView())
            activeView()->chooseFiles();
    });
    open->setObjectName("openWorkspaceLogs");
    menu_->addSeparator();
    menu_->addAction("新建工作区…", this, [this] { promptCreate(false); });
    menu_->addAction("另存为新工作区…", this, [this] { promptCreate(true); });
    menu_->addAction("保存 / 更新工作区与布局", this, &LogWorkspace::saveWorkspaces);
    menu_->addAction("重命名当前工作区…", this, [this] {
        if (!current())
            return;
        const auto id = current()->id;
        bool ok = false;
        const auto name = QInputDialog::getText(this, "重命名工作区", "工作区名称：", QLineEdit::Normal,
                                                current()->name, &ok);
        if (ok)
            renameWorkspace(id, name);
    });
    menu_->addAction("恢复单窗口", this, &LogWorkspace::restoreSingleWindow);
    menu_->addSeparator();
    auto* whole = menu_->addMenu("整个工作区");
    for (const auto& item : QList<QPair<QString, QString>>{{"导出整个工作区…", "export_workspace"},
                                                           {"导入整个工作区…", "import_workspace"},
                                                           {"备份当前工作区", "backup_workspace"},
                                                           {"打开备份文件夹", "open_backup_directory"}})
        whole->addAction(item.first, this, [this, operation = item.second] {
            saveWorkspaces();
            emit workspaceOperationRequested(operation);
        });
}
void LogWorkspace::saveWorkspaces() {
    QJsonArray rows;
    for (auto& workspace : workspaces_) {
        const auto state = workspace.restored ? workspace.view->snapshot() : workspace.saved;
        rows.append(QJsonObject{
            {"id", workspace.id},
            {"name", workspace.name},
            {"connection", workspace.connectionId},
            {"automaticName", workspace.automaticName},
            {"state", state},
            {"floating", bool(workspace.floating) || workspace.restoreFloating},
            {"floatingGeometry", QString::fromLatin1((workspace.floating ? workspace.floating->saveGeometry()
                                                                         : workspace.floatingGeometry)
                                                         .toBase64())}});
    }
    const QJsonObject state{{"version", 2},
                            {"workspaces", rows},
                            {"active", current() ? current()->id : QString{}},
                            {"geometry", QString::fromLatin1(saveGeometry().toBase64())}};
    QString error;
    if (!LogWorkspaceStore::save(state, error))
        QMessageBox::warning(this, "日志工作区保存失败", error);
}
void LogWorkspace::detachWorkspace(const QString& id) {
    activateWorkspace(id);
    for (auto& workspace : workspaces_)
        if (workspace.id == id) {
            if (workspace.floating) {
                workspace.floating->raise();
                return;
            }
            auto* shell = new FloatingLogWindow(this);
            shell->setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
            shell->setWindowIcon(windowIcon());
            shell->setWindowTitle(workspace.name);
            shell->setProperty("logWorkspaceIdentity", workspace.id);
            shell->resize(size());
            if (!workspace.floatingGeometry.isEmpty())
                shell->restoreGeometry(workspace.floatingGeometry);
            shell->setMenuWidget(new WindowCaption(shell, workspace.name, 32));
            pages_->removeWidget(workspace.view);
            shell->setCentralWidget(workspace.view);
            shell->returnToHost = [this, id] { dockWorkspace(id); };
            workspace.floating = shell;
            workspace.restoreFloating = false;
            shell->show();
            rebuildTabs();
            return;
        }
}
void LogWorkspace::dockWorkspace(const QString& id) {
    for (auto& workspace : workspaces_)
        if (workspace.id == id && workspace.floating) {
            auto* shell = workspace.floating.data();
            workspace.floatingGeometry = shell->saveGeometry();
            shell->takeCentralWidget();
            pages_->addWidget(workspace.view);
            workspace.floating = nullptr;
            workspace.restoreFloating = false;
            shell->hide();
            shell->deleteLater();
            rebuildTabs();
            activateWorkspace(id);
            show();
            return;
        }
}
void LogWorkspace::restoreSingleWindow() {
    QStringList ids;
    for (const auto& workspace : workspaces_)
        if (workspace.floating)
            ids.push_back(workspace.id);
    for (const auto& id : ids)
        dockWorkspace(id);
    showNormal();
}
bool LogWorkspace::eventFilter(QObject* watched, QEvent* event) {
    if (watched == workspaceTabs_) {
        auto* mouse = dynamic_cast<QMouseEvent*>(event);
        if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton) {
            dragStart_ = mouse->globalPosition().toPoint();
            const int index = workspaceTabs_->tabAt(mouse->position().toPoint());
            dragWorkspaceId_ = index >= 0 ? workspaceTabs_->tabData(index).toString() : QString{};
        } else if (event->type() == QEvent::MouseMove && !dragWorkspaceId_.isEmpty() &&
                   mouse->buttons().testFlag(Qt::LeftButton)) {
            if ((mouse->globalPosition().toPoint() - dragStart_).manhattanLength() >
                    QApplication::startDragDistance() &&
                (mouse->position().y() < -16 || mouse->position().y() > workspaceTabs_->height() + 16)) {
                const auto id = dragWorkspaceId_;
                dragWorkspaceId_.clear();
                detachWorkspace(id);
                for (auto& workspace : workspaces_)
                    if (workspace.id == id && workspace.floating) {
                        workspace.floating->move(mouse->globalPosition().toPoint() - QPoint(100, 16));
                        if (workspace.floating->windowHandle())
                            workspace.floating->windowHandle()->startSystemMove();
                    }
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonRelease)
            dragWorkspaceId_.clear();
    }
    return QMainWindow::eventFilter(watched, event);
}
void LogWorkspace::closeEvent(QCloseEvent* event) {
    for (auto& workspace : workspaces_) {
        workspace.view->hideFinder();
        if (workspace.floating)
            workspace.floating->hide();
    }
    saveWorkspaces();
    for (auto& workspace : workspaces_)
        workspace.view->cancelLoading();
    QMainWindow::closeEvent(event);
}
void LogWorkspace::keyPressEvent(QKeyEvent* event) {
    if (event->matches(QKeySequence::Find)) {
        search();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Open)) {
        if (activeView())
            activeView()->chooseFiles();
        event->accept();
        return;
    }
    QMainWindow::keyPressEvent(event);
}
void LogWorkspace::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    const auto active = current() ? current()->id : QString{};
    QStringList pending;
    for (const auto& workspace : workspaces_)
        if (workspace.restoreFloating)
            pending.push_back(workspace.id);
    for (const auto& id : pending) {
        detachWorkspace(id);
        for (auto& workspace : workspaces_)
            if (workspace.id == id && workspace.floating && !workspace.floatingGeometry.isEmpty())
                workspace.floating->restoreGeometry(workspace.floatingGeometry);
    }
    for (auto& workspace : workspaces_)
        if (workspace.floating)
            workspace.floating->show();
    if (!active.isEmpty())
        activateWorkspace(active);
}
} // namespace portwave
