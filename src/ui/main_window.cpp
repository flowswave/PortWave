#include "ui/main_window.h"
#include "features/connection_tabs.h"
#include "features/snap_layout.h"
#include "features/split_grip.h"
#include "features/window_caption.h"
#include "infrastructure/named_layout_store.h"
#include "infrastructure/workspace_archive.h"
#include "ui/connection_dialog.h"
#include "ui/keyboard_shortcuts.h"
#include "ui/settings_dialog.h"
#include "ui/shortcuts.h"
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QWindow>

namespace portwave {
namespace {
QIcon connectionStatusIcon(bool connected, bool dark) {
    QPixmap pixmap(8, 8);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setPen(Qt::NoPen);
    painter.setBrush(appearanceColor(connected ? "status.connected" : "status.disconnected", dark,
                                     QColor(connected ? "#98c379" : "#e06c75")));
    painter.drawEllipse(1, 1, 5, 5);
    return QIcon(pixmap);
}
} // namespace
MainWindow::MainWindow(SettingsRepository& repository, ThemeManager& theme)
    : repository_(repository), theme_(theme) {
    setWindowTitle("PortWave");
    setWindowFlag(Qt::FramelessWindowHint);
    resize(1280, 820);
    setMinimumSize(850, 560);
    setMouseTracking(true);
    senderPinned_ = QSettings().value("sender/pinned", true).toBool();
    auto* header = new QWidget;
    header->setObjectName("shellHeader");
    auto* top = new QVBoxLayout(header);
    top->setContentsMargins(1, 1, 1, 0);
    top->setSpacing(0);
    caption_ = new WindowCaption(this, "PortWave");
    top->addWidget(caption_);
    toolbar_ = new QToolBar;
    toolbar_->setMovable(false);
    toolbar_->setIconSize(QSize(20, 20));
    toolbar_->setFixedHeight(theme_.profile().controls.toolbarHeight);
    toolbar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    top->addWidget(toolbar_);
    setMenuWidget(header);
    auto action = [this](const QString& icon, const QString& name, auto fn) {
        auto* a = toolbar_->addAction(IconManager::icon(icon, theme_.dark()), name);
        a->setObjectName("toolbar_" + icon);
        a->setToolTip(name);
        connect(a, &QAction::triggered, this, fn);
        icons_.push_back({a, icon});
        return a;
    };
    action("plug", "新建连接", [this] { newConnection(); });
    action("power", "断开", [this] {
        if (auto* r = current(); r && !r->powerPending) {
            if (r->session->connected()) {
                r->powerPending = true;
                r->session->stop();
                updateToolbarState();
            } else {
                r->powerPending = true;
                updateToolbarState();
                r->session->reconnect();
            }
        }
    });
    action("clear", "清空", [this] {
        if (auto* r = current())
            r->pane->terminal()->resetTerminal();
    });
    action("search", "查找", [this] {
        if (auto* r = current())
            r->pane->find();
    });
    action("log", "打开日志", [this] {
        if (auto* record = current())
            logs_->activateConnection(record->session->config().id, record->session->config().name);
        logs_->show();
        logs_->raise();
    });
    action("panels", "布局", [this] {
        QMenu menu;
        menu.addAction("显示 / 隐藏发送窗口", this, [this] {
            if (auto* r = current())
                r->pane->toggleSender();
        });
        menu.addAction("连接配置…", this, [this] { newConnection(); });
        menu.addSeparator();
        menu.addAction("层叠窗口", mdi_, &QMdiArea::cascadeSubWindows);
        menu.addAction("平铺窗口", mdi_, &QMdiArea::tileSubWindows);
        menu.addAction("最大化当前窗口", this, [this] {
            if (auto* r = current())
                r->window->showMaximized();
        });
        menu.exec(QCursor::pos());
    });
    auto* stretch = new QWidget;
    stretch->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar_->addWidget(stretch);
    auto* settingsAction = action("settings", "设置", [this] { settings(); });
    if (auto* button = qobject_cast<QToolButton*>(toolbar_->widgetForAction(settingsAction)))
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto* central = new QWidget;
    auto* body = new QVBoxLayout(central);
    body->setContentsMargins(1, 0, 1, 1);
    body->setSpacing(0);
    tabs_ = new ConnectionTabs;
    tabs_->setObjectName("connectionTabs");
    connect(static_cast<ConnectionTabs*>(tabs_), &ConnectionTabs::newConnectionRequested, this,
            &MainWindow::newConnection);
    tabs_->setExpanding(false);
    tabs_->setMovable(false);
    tabs_->setDrawBase(false);
    tabs_->setMinimumHeight(theme_.profile().controls.tabHeight);

    body->addWidget(tabs_);
    mdi_ = new QMdiArea;
    mdi_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mdi_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mdi_->setOption(QMdiArea::DontMaximizeSubWindowOnActivation);
    auto* workspaceSplitter = workspaceSplitter_ = new QSplitter(Qt::Vertical);
    workspaceSplitter->setObjectName("workspaceSplitter");
    controls_ = new QStackedWidget;
    controls_->setObjectName("sharedConnectionControls");
    controls_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    workspaceSplitter->addWidget(mdi_);
    workspaceSplitter->addWidget(controls_);
    workspaceSplitter->setStretchFactor(0, 1);
    workspaceSplitter->setStretchFactor(1, 0);
    workspaceSplitter->setCollapsible(1, false);
    body->addWidget(workspaceSplitter, 1);
    connectionStatus_ = new QLabel;
    connectionStatus_->setObjectName("mainConnectionStatus");
    connectionStatus_->setContentsMargins(8, 0, 8, 0);
    connectionStatus_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    statusBar()->addWidget(connectionStatus_);
    fileStatuses_ = new QStackedWidget;
    fileStatuses_->setObjectName("mainLogFileStatusStack");
    fileStatuses_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusBar()->addWidget(fileStatuses_, 1);
    fileStatuses_->hide();
    controls_->hide();
    snap_ = new SnapLayout(
        mdi_, tabs_,
        [this](int index) -> QMdiSubWindow* {
            return index >= 0 && index < records_.size() ? records_[index].window : nullptr;
        },
        this);
    // Pure view overlay: adjacent-pane split handles (hover 1 s, drag to resize).
    split_ = new SplitGrip(mdi_, this);
    connect(snap_, &SnapLayout::draggingChanged, this, [this](bool dragging) {
        for (auto& record : records_)
            record.pane->setInteractionPaused(dragging);
    });
    connect(snap_, &SnapLayout::paneDocked, this, [this](QMdiSubWindow* window) {
        for (auto& record : records_)
            if (record.window == window) {
                record.detached = false;
                record.pane->setDetached(false);
                break;
            }
        syncConnectionTabs();
    });
    connect(snap_, &SnapLayout::paneDetached, this, [this](QMdiSubWindow* window) {
        for (auto& record : records_)
            if (record.window == window) {
                record.detached = true;
                record.pane->setDetached(true);
                break;
            }
        syncConnectionTabs();
    });
    setCentralWidget(central);
    logs_ = new LogWorkspace(this);
    logs_->setTheme(theme_.dark());
    connect(logs_, &LogWorkspace::workspaceOperationRequested, this, &MainWindow::operation);
    const auto activateTab = [this](int index) {
        if (index >= 0 && index < records_.size()) {
            mdi_->setActiveSubWindow(records_[index].window);
            records_[index].window->show();
            for (const auto& record : records_)
                if (record.detached)
                    record.window->raise();
        }
    };
    connect(tabs_, &QTabBar::currentChanged, this, activateTab);
    connect(tabs_, &QTabBar::tabBarClicked, this, [this, activateTab](int index) {
        if (index >= 0 && index < records_.size())
            logs_->activateConnection(records_[index].session->config().id,
                                      records_[index].session->config().name);
        if (index >= 0 && index < records_.size() && index == tabs_->currentIndex() &&
            mdi_->activeSubWindow() != records_[index].window)
            activateTab(index);
    });

    connect(tabs_, &QTabBar::tabBarDoubleClicked, this, [this](int index) {
        if (index >= 0 && index < records_.size())
            closeRecord(records_[index].window);
    });
    connect(mdi_, &QMdiArea::subWindowActivated, this, [this](QMdiSubWindow* window) {
        updateActivePaneOutline();
        for (int i = 0; i < records_.size(); ++i)
            if (records_[i].window == window) {
                for (auto& previous : records_)
                    if (previous.window == lastActive_ && previous.window != window)
                        previous.controlsHeight = workspaceSplitter_->sizes().value(1, 200);
                static_cast<ConnectionTabs*>(tabs_)->setSelectionVisible(!records_[i].detached);
                const QSignalBlocker blocker(tabs_);
                if (!records_[i].detached)
                    tabs_->setCurrentIndex(i);
                controls_->setCurrentWidget(records_[i].pane->controlsWidget());
                controls_->show();
                fileStatuses_->setCurrentWidget(records_[i].pane->fileStatusWidget());
                fileStatuses_->show();
                const int desired = senderPinned_ ? pinnedSenderHeight_ : records_[i].controlsHeight;
                workspaceSplitter_->setSizes({qMax(0, workspaceSplitter_->height() - desired), desired});
                lastActive_ = window;
                updateConnectionStatus();
                logs_->activateConnection(records_[i].session->config().id,
                                          records_[i].session->config().name);
                statusBar()->clearMessage();
                updateToolbarState();
            }
    });
    connect(workspaceSplitter_, &QSplitter::splitterMoved, this, [this] {
        if (auto* record = current()) {
            record->controlsHeight = workspaceSplitter_->sizes().value(1, 200);
            record->pane->rememberSenderHeight(record->controlsHeight, senderPinned_);
            if (senderPinned_)
                pinnedSenderHeight_ = record->controlsHeight;
        }
    });
    connect(&theme_, &ThemeManager::changed, this, &MainWindow::restyle);
    restyle(theme_.dark());
    rebuildShortcuts();
    QSettings s;
    restoreGeometry(s.value("window/geometry").toByteArray());
}
MainWindow::~MainWindow() {
    disconnect(mdi_, nullptr, this, nullptr);
    disconnect(tabs_, nullptr, this, nullptr);
}
void MainWindow::updateToolbarState() {
    auto* record = current();
    for (const auto& pair : icons_) {
        if (pair.second == "power") {
            const bool connected = record && record->session->connected();
            const bool paused = connected && record->pane->receivePaused();
            const auto color = record
                                   ? appearanceColor(connected ? "status.connected" : "status.disconnected",
                                                     theme_.dark(), QColor(connected ? "#98c379" : "#e06c75"))
                                   : appearanceColor("text.disabled", theme_.dark(), QColor("#858d9b"));
            pair.first->setIcon(IconManager::icon("power", theme_.dark(), color));
            pair.first->setEnabled(record && !record->powerPending);
            pair.first->setText(connected ? "断开" : "连接");
            pair.first->setToolTip(record && record->powerPending ? "正在切换连接状态…"
                                   : connected                    ? "断开当前连接"
                                                                  : "连接当前设备");
            pair.first->setProperty("connectionState",
                                    record ? connected ? paused ? "paused" : "connected" : "disconnected"
                                           : "none");
        } else if (pair.second == "send")
            pair.first->setIcon(
                IconManager::icon("send", theme_.dark(), {}, record && record->pane->senderVisible()));
    }
}
MainWindow::Record* MainWindow::current() {
    for (auto& r : records_)
        if (r.window == mdi_->activeSubWindow())
            return &r;
    return nullptr;
}
void MainWindow::newConnection() {
    ConnectionDialog dialog(repository_, this);
    if (dialog.exec() == QDialog::Accepted)
        for (const auto& c : dialog.selected())
            addConnection(c);
}
void MainWindow::addTestConnection() {
    ConnectionConfig c;
    c.kind = "test";
    c.name = "test" + QString::number(++testCount_);
    c.id = QUuid::createUuid().toString();
    c.autoLog = true;
    c.logDirectory = qEnvironmentVariable("PORTWAVE_TEST_LOG_DIR");
    addConnection(c);
}
void MainWindow::addConnection(const ConnectionConfig& config) {
    for (auto& r : records_)
        if (r.session->config().id == config.id) {
            mdi_->setActiveSubWindow(r.window);
            statusBar()->showMessage("此连接已打开；关闭后可重新连接");
            return;
        }
    auto* session = new Session(config, this);
    logs_->activateConnection(config.id, config.name);
    auto* pane = new TerminalPane(session);
    pane->setSendShortcut(configuredShortcut("send_draft"));
    pane->setFindShortcuts({{QStringLiteral("find_next"), configuredShortcut("find_next")},
                            {QStringLiteral("find_previous"), configuredShortcut("find_previous")}});
    pane->setSenderPinned(senderPinned_);
    applyTerminalAppearance(pane, theme_.dark());
    auto* window = mdi_->addSubWindow(pane);
    window->setWindowFlags(Qt::SubWindow | Qt::FramelessWindowHint);
    window->setWindowTitle(config.name);
    window->setAttribute(Qt::WA_DeleteOnClose, false);
    window->installEventFilter(this);
    records_.push_back({session, pane, window});
    records_.back().controlsHeight = pane->storedSenderHeight(false);
    if (records_.size() == 1)
        pinnedSenderHeight_ = pane->storedSenderHeight(true);
    controls_->addWidget(pane->controlsWidget());
    fileStatuses_->addWidget(pane->fileStatusWidget());
    fileStatuses_->setCurrentWidget(pane->fileStatusWidget());
    fileStatuses_->show();
    controls_->setCurrentWidget(pane->controlsWidget());
    controls_->show();
    snap_->watchHeader(pane->dragHeader(), window);
    connect(pane, &TerminalPane::dockRequested, this, [this, window] { dockRecord(window); });
    connect(pane, &TerminalPane::senderPinChanged, this, [this](bool enabled) {
        senderPinned_ = enabled;
        if (enabled)
            pinnedSenderHeight_ = workspaceSplitter_->sizes().value(1, 200);
        if (enabled && current())
            current()->pane->rememberSenderHeight(pinnedSenderHeight_, true);
        for (auto& record : records_)
            record.pane->setSenderPinned(enabled);
    });
    connect(pane, &TerminalPane::senderVisibilityChanged, this, &MainWindow::updateToolbarState);
    connect(pane, &TerminalPane::receivePauseChanged, this, &MainWindow::updateToolbarState);
    tabs_->addTab(config.name);
    syncConnectionTabs();
    tabs_->setCurrentIndex(tabs_->count() - 1);
    window->showMaximized();
    for (const auto& record : records_)
        if (record.detached)
            record.window->raise();
    connect(pane, &TerminalPane::status, statusBar(),
            [this](const QString& msg) { statusBar()->showMessage(msg, 8000); });
    connect(pane, &TerminalPane::connectionStatusChanged, this, [this, pane](const QString&) {
        if (auto* active = current(); active && active->pane == pane) {
            updateConnectionStatus();
        }
    });
    connect(pane, &TerminalPane::openLog, logs_, [this, session](const QString& path) {
        logs_->openConnectionLog(session->config().id, session->config().name, path);
    });
    connect(pane, &TerminalPane::commandsChanged, this, [this, pane](const QString& group) {
        for (auto& record : records_)
            if (record.pane != pane)
                record.pane->reloadCommandGroup(group);
    });
    connect(session, &Session::configChanged, this, &MainWindow::saveConfiguration);
    connect(pane, &TerminalPane::targetRequested, this, [this](const QString& id) {
        for (int i = 0; i < records_.size(); ++i)
            if (records_[i].session->config().id == id) {
                mdi_->setActiveSubWindow(records_[i].window);
                return;
            }
    });
    QVector<QPair<QString, QString>> targets;
    for (const auto& r : records_)
        targets.push_back({r.session->config().name, r.session->config().id});
    for (auto& r : records_)
        r.pane->setTargets(targets);
    connect(session, &Session::stateChanged, this, [this, session](bool ok, const QString& message) {
        for (int i = 0; i < records_.size(); ++i)
            if (records_[i].session == session) {
                if (ok)
                    records_[i].powerPending = false;
                tabs_->setTabIcon(i, connectionStatusIcon(ok, theme_.dark()));
                tabs_->setTabToolTip(i, message);
            }
        updateToolbarState();
    });
    connect(session, &Session::failure, this, [this, session](const QString& msg) {
        for (auto& record : records_)
            if (record.session == session)
                record.powerPending = false;
        updateToolbarState();
        statusBar()->showMessage(msg, 15000);
        diagnostic("session.error");
    });
    connect(session, &Session::stopped, this, [this, session] {
        for (auto& record : records_)
            if (record.session == session)
                record.powerPending = false;
        updateToolbarState();
        if (closing_) {
            for (const auto& r : records_)
                if (!r.session->isStopped())
                    return;
            close();
        }
    });
    session->start();
}
void MainWindow::closeRecord(QMdiSubWindow* window) {
    for (int i = 0; i < records_.size(); ++i)
        if (records_[i].window == window) {
            auto record = records_[i];
            if (!record.session->isStopped()) {
                record.pane->shutdown();
                record.window->setEnabled(false);
                connect(
                    record.session, &Session::stopped, this, [this, window] { closeRecord(window); },
                    Qt::SingleShotConnection);
                record.session->stop();
                return;
            }
            {
                const QSignalBlocker blocker(tabs_);
                tabs_->removeTab(i);
                records_.removeAt(i);
            }
            controls_->removeWidget(record.pane->controlsWidget());
            fileStatuses_->removeWidget(record.pane->fileStatusWidget());
            syncConnectionTabs();
            QVector<QPair<QString, QString>> targets;
            for (const auto& r : records_)
                targets.push_back({r.session->config().name, r.session->config().id});
            for (auto& r : records_)
                r.pane->setTargets(targets);
            mdi_->removeSubWindow(record.pane);
            record.window->deleteLater();
            record.pane->deleteLater();
            record.session->deleteLater();
            return;
        }
}
void MainWindow::updateActivePaneOutline() {
    int visible = 0;
    for (const auto& record : records_)
        if (record.window->isVisible() && !record.window->isMinimized())
            ++visible;
    for (const auto& record : records_)
        record.pane->setActiveConnection(visible > 1 && !record.window->isMaximized() &&
                                         record.window == mdi_->activeSubWindow());
}
void MainWindow::syncConnectionTabs() {
    updateActivePaneOutline();
    const QSignalBlocker blocker(tabs_);
    bool anyDocked = false;
    for (int i = 0; i < records_.size(); ++i) {
        tabs_->setTabVisible(i, !records_[i].detached);
        anyDocked = anyDocked || !records_[i].detached;
    }
    tabs_->setVisible(anyDocked);
    auto* active = current();
    static_cast<ConnectionTabs*>(tabs_)->setSelectionVisible(active && !active->detached);
    static_cast<ConnectionTabs*>(tabs_)->synchronize();
    if (records_.isEmpty()) {
        controls_->hide();
        fileStatuses_->hide();
        connectionStatus_->clear();
    }
}
void MainWindow::dockRecord(QMdiSubWindow* window) {
    for (int i = 0; i < records_.size(); ++i)
        if (records_[i].window == window) {
            records_[i].detached = false;
            records_[i].pane->setDetached(false);
            snap_->forgetWindow(window);
            syncConnectionTabs();
            tabs_->setCurrentIndex(i);
            window->showMaximized();
            mdi_->setActiveSubWindow(window);
            for (const auto& record : records_)
                if (record.detached)
                    record.window->raise();
            return;
        }
}
bool MainWindow::eventFilter(QObject* object, QEvent* event) {
    if (event->type() == QEvent::WindowStateChange || event->type() == QEvent::Show ||
        event->type() == QEvent::Hide || event->type() == QEvent::Resize)
        for (const auto& record : records_)
            if (record.window == object) {
                QTimer::singleShot(0, this, &MainWindow::updateActivePaneOutline);
                break;
            }
    if (event->type() == QEvent::Close && !closing_) {
        for (const auto& r : records_)
            if (r.window == object) {
                static_cast<QCloseEvent*>(event)->ignore();
                closeRecord(r.window);
                return true;
            }
    }
    return QMainWindow::eventFilter(object, event);
}
void MainWindow::closeEvent(QCloseEvent* event) {
    bool pending = false;
    for (auto& r : records_)
        if (!r.session->isStopped()) {
            pending = true;
            r.pane->shutdown();
            r.session->stop();
        }
    if (pending) {
        closing_ = true;
        setEnabled(false);
        event->ignore();
        return;
    }
    QSettings().setValue("window/geometry", saveGeometry());
    logs_->close();
    event->accept();
}
void MainWindow::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && !isMaximized() && windowHandle()) {
        const auto p = e->position();
        Qt::Edges edges;
        if (p.x() < 7)
            edges |= Qt::LeftEdge;
        if (p.x() > width() - 7)
            edges |= Qt::RightEdge;
        if (p.y() < 7)
            edges |= Qt::TopEdge;
        if (p.y() > height() - 7)
            edges |= Qt::BottomEdge;
        if (edges) {
            windowHandle()->startSystemResize(edges);
            e->accept();
            return;
        }
    }
    QMainWindow::mousePressEvent(e);
}
void MainWindow::updateConnectionStatus() {
    auto* record = current();
    if (!record) {
        connectionStatus_->clear();
        return;
    }
    const auto detailed = record->pane->connectionStatusText();
    const auto color = resolveThemeColors(theme_.themeId())
                           .value(record->session->connected() ? "status.connected" : "status.disconnected");
    connectionStatus_->setText(QString("<span style='color:%1'>●</span> %2")
                                   .arg(color.name(), record->session->connected() ? "已连接" : "已断开"));
    connectionStatus_->setToolTip(detailed);
}
void MainWindow::restyle(bool dark) {
    const auto profile = theme_.profile();
    toolbar_->setFixedHeight(profile.controls.toolbarHeight);
    tabs_->setMinimumHeight(profile.controls.tabHeight);
    toolbar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    for (auto* button : toolbar_->findChildren<QToolButton*>())
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    caption_->setCommandBar(nullptr);
    qobject_cast<QVBoxLayout*>(menuWidget()->layout())->insertWidget(1, toolbar_);
    setContentsMargins(0, 0, 0, 0);
    for (const auto& pair : icons_)
        pair.first->setIcon(IconManager::icon(pair.second, dark));
    setWindowIcon(IconManager::appIcon());
    mdi_->setBackground(DesignTokens::forTheme(theme_.themeId()).surface);
    QSettings settings;
    const QString prefix = "appearance/" + theme_.themeId() + "/";
    QFont terminalFont(settings.value(prefix + "font", "Consolas").toString(),
                       settings.value(prefix + "size", 12).toInt());
    const QColor terminalForeground(
        settings.value(prefix + "foreground", DesignTokens::forTheme(theme_.themeId()).text.name())
            .toString());
    for (int i = 0; i < records_.size(); ++i) {
        auto& r = records_[i];
        applyTerminalAppearance(r.pane, dark);
        tabs_->setTabIcon(i, connectionStatusIcon(r.session->connected(), dark));
    }
    logs_->setTheme(dark);
    logs_->setReadingAppearance(QFont(settings
                                          .value("logs/appearance/" + theme_.themeId() + "/font",
                                                 settings.value("logs/appearance/font", "Consolas"))
                                          .toString(),
                                      settings
                                          .value("logs/appearance/" + theme_.themeId() + "/size",
                                                 settings.value("logs/appearance/size", 12))
                                          .toInt()),
                                QColor(settings
                                           .value("logs/appearance/" + theme_.themeId() + "/background",
                                                  DesignTokens::forTheme(theme_.themeId()).surface.name())
                                           .toString()),
                                terminalForeground);
    logs_->setReadingOptions(settings.value("logs/view/wrap", true).toBool(),
                             settings.value("logs/view/numbers", true).toBool(),
                             settings.value("logs/view/colors", true).toBool());
    updateToolbarState();
}
void MainWindow::applyTerminalAppearance(TerminalPane* pane, bool dark) {
    QSettings settings;
    const QString prefix = "appearance/" + theme_.themeId() + "/";
    pane->setTheme(dark);
    pane->setTerminalAppearance(
        QFont(settings.value(prefix + "font", "Consolas").toString(),
              settings.value(prefix + "size", 12).toInt()),
        QColor(settings.value(prefix + "background", DesignTokens::forTheme(theme_.themeId()).surface.name())
                   .toString()),
        QColor(settings.value(prefix + "foreground", DesignTokens::forTheme(theme_.themeId()).text.name())
                   .toString()),
        settings.value(prefix + "smooth", true).toBool());
}
void MainWindow::openFile(const QString& path) {
    logs_->openPath(path);
}

void MainWindow::settings() {
    QVector<TerminalPane*> panes;
    for (const auto& record : records_)
        panes.push_back(record.pane);
    SettingsDialog dialog(repository_, theme_, panes, logs_, this, {},
                          current() ? current()->session->config().id : QString{});
    connect(&dialog, &SettingsDialog::shortcutsChanged, this, [this, &panes] {
        rebuildShortcuts();
        for (auto* pane : panes)
            pane->reloadFindPreferences();
    });
    connect(&dialog, &SettingsDialog::findPreferencesChanged, this, [&panes] {
        for (auto* pane : panes)
            pane->reloadFindPreferences();
    });
    connect(&dialog, &SettingsDialog::actionRequested, this, &MainWindow::operation);
    dialog.exec();
}

void MainWindow::saveConfiguration(const ConnectionConfig& config) {
    if (config.id.isEmpty())
        return;
    logs_->registerConnection(config.id, config.name);
    for (int index = 0; index < records_.size(); ++index) {
        if (records_[index].session->config().id == config.id) {
            records_[index].window->setWindowTitle(config.name);
            tabs_->setTabText(index, config.name);
            break;
        }
    }
    QVector<QPair<QString, QString>> targets;
    for (const auto& record : records_) {
        const auto currentConfig = record.session->config();
        targets.push_back(
            {currentConfig.id == config.id ? config.name : currentConfig.name, currentConfig.id});
    }
    for (auto& record : records_)
        record.pane->setTargets(targets);
    if (config.kind == "test")
        return;
    QString error;
    if (!repository_.updateConnection(config, error))
        statusBar()->showMessage("设置保存失败：" + error, 10000);
}

void MainWindow::rebuildShortcuts() {
    for (auto* shortcut : shortcuts_)
        shortcut->deleteLater();
    shortcuts_.clear();
    for (const auto& definition : shortcutDefinitions()) {
        if (definition.scope != ShortcutScope::MainWindow)
            continue;
        const auto sequence = configuredShortcut(definition.id);
        if (sequence.isEmpty())
            continue;
        // Only the log search window may use the IME toggle combination; every other
        // operation stays clear of it until the user rebinds it.
        if (QLatin1String(definition.id) != "log_search" && conflictsWithImeToggle(sequence))
            continue;
        auto* shortcut = new QShortcut(sequence, this);
        shortcut->setContext(Qt::WindowShortcut);
        const QString id = QLatin1String(definition.id);
        connect(shortcut, &QShortcut::activated, this, [this, id] { operation(id); });
        shortcuts_.push_back(shortcut);
    }
    // Feature-level windows bind their own actions; the UI layer forwards the
    // configured sequences instead of letting features read the shortcut profile.
    QHash<QString, QKeySequence> feature;
    for (const auto id : {"search", "find_next", "find_previous", "log_refresh", "log_open", "log_close"})
        feature.insert(QLatin1String(id), configuredShortcut(QLatin1String(id)));
    logs_->setShortcutOverrides(feature);
    const auto send = configuredShortcut("send_draft");
    for (auto& record : records_) {
        record.pane->setSendShortcut(send);
        record.pane->setFindShortcuts(feature);
    }
}

void MainWindow::operation(const QString& name) {
    if (name.startsWith("appearance_layout:")) {
        if (!snap_->applyPreset(name.mid(QString("appearance_layout:").size())))
            statusBar()->showMessage("当前工作区尺寸不足，保留原布局", 5000);
        return;
    }
    if (name == "new_connection") {
        newConnection();
        return;
    }
    if (name == "clear") {
        if (auto* record = current())
            record->pane->terminal()->resetTerminal();
        return;
    }
    if (name == "new_test") {
        addTestConnection();
        return;
    }
    if (name == "manage_connections") {
        newConnection();
        return;
    }
    if (name == "cache_find") {
        if (auto* record = current())
            record->pane->find();
        return;
    }
    if (name == "search") {
        if (auto* record = current())
            record->pane->find();
        return;
    }
    if (name == "log_search" || name == "log_find" || name == "log_find_files") {
        logs_->show();
        logs_->raise();
        if (name == "log_find_files")
            logs_->showFindOptions();
        else if (name == "log_find")
            logs_->search();
        return;
    }
    if (name == "help" || name == "release_history") {
        QMessageBox::information(this, name == "help" ? "使用帮助" : "版本更新记录",
                                 name == "help" ? "PortWave 串口工具使用说明尚未迁移完成。"
                                                : "当前版本：PortWave C++ 0.2.0");
        return;
    }
    if (name == "save_layout") {
        bool ok = false;
        const auto label =
            QInputDialog::getText(this, "保存布局", "布局名称", QLineEdit::Normal, {}, &ok).trimmed();
        if (ok && !label.isEmpty()) {
            if (label.contains('/') || label.contains('\\')) {
                QMessageBox::warning(this, "保存布局", "布局名称不能包含斜杠。");
                return;
            }
            NamedLayoutSnapshot snapshot;
            snapshot.mainGeometry = saveGeometry();
            snapshot.activeConnectionId = current() ? current()->session->config().id : QString{};
            for (const auto& record : records_)
                snapshot.windows.push_back({record.session->config().id, record.window->saveGeometry(),
                                            record.window->isMaximized()});
            QSettings settings;
            QString error;
            if (!NamedLayoutStore::save(settings, label, snapshot, error)) {
                QMessageBox::warning(this, "保存布局失败", error);
                return;
            }
            statusBar()->showMessage("布局已保存：" + label, 5000);
        }
        return;
    }
    if (name.startsWith("restore_layout:")) {
        const auto label = name.mid(QString("restore_layout:").size());
        QSettings settings;
        const auto snapshot = NamedLayoutStore::load(settings, label);
        if (snapshot) {
            restoreGeometry(snapshot->mainGeometry);
            if (!snapshot->legacyGeometryOnly) {
                for (const auto& window : snapshot->windows)
                    for (const auto& record : records_)
                        if (record.session->config().id == window.connectionId) {
                            record.window->restoreGeometry(window.geometry);
                            if (window.maximized)
                                record.window->showMaximized();
                        }
                for (int i = 0; i < records_.size(); ++i)
                    if (records_[i].session->config().id == snapshot->activeConnectionId) {
                        tabs_->setCurrentIndex(i);
                        mdi_->setActiveSubWindow(records_[i].window);
                    }
            }
        }
        return;
    }
    if (name == "dock_all" || name == "restore_docked") {
        mdi_->tileSubWindows();
        return;
    }
    if (name == "restore_float") {
        mdi_->cascadeSubWindows();
        return;
    }
    if (name == "restore_log_window") {
        logs_->showNormal();
        logs_->show();
        logs_->raise();
        return;
    }
    if (name == "open_backup_directory") {
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)));
        return;
    }
    if (name == "save_workspace" || name == "new_workspace" || name == "copy_workspace" ||
        name == "rename_workspace" || name == "export_workspace" || name == "import_workspace" ||
        name == "backup_workspace") {
        auto workspaceFolder =
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/workspaces";
        QDir().mkpath(workspaceFolder);
        QSettings settings;
        if (name == "import_workspace") {
            if (!records_.isEmpty()) {
                QMessageBox::warning(this, "导入工作区",
                                     "请先关闭所有串口窗口，再导入工作区。导入不会自动连接设备。");
                return;
            }
            const auto path =
                QFileDialog::getOpenFileName(this, "导入整个工作区", {}, "PortWave 工作区 (*.swws)");
            if (path.isEmpty())
                return;
            QJsonObject importedSettings;
            QJsonArray importedConnections;
            QString error;
            if (!WorkspaceArchive::read(path, importedSettings, importedConnections, error)) {
                QMessageBox::warning(this, "工作区导入失败", error);
                return;
            }
            QJsonObject previousSettings;
            if (!WorkspaceArchive::captureSettings(settings, previousSettings, error)) {
                QMessageBox::warning(this, "工作区导入失败", error);
                return;
            }
            const auto previousConnections = repository_.loadConnections();
            QVector<ConnectionConfig> nextConnections;
            for (const auto& row : importedConnections)
                nextConnections.push_back(ConnectionConfig::fromJson(row.toObject()));
            if (!WorkspaceArchive::replaceSettings(settings, importedSettings, error)) {
                const auto failure = error;
                QString rollbackError;
                const bool restored =
                    WorkspaceArchive::replaceSettings(settings, previousSettings, rollbackError);
                QMessageBox::warning(this, "工作区导入失败",
                                     failure +
                                         (restored ? QString{} : "\n恢复原设置也失败：" + rollbackError));
                return;
            }
            if (!repository_.saveConnections(nextConnections, error)) {
                const auto failure = error;
                QString rollbackError;
                const bool settingsRestored =
                    WorkspaceArchive::replaceSettings(settings, previousSettings, rollbackError);
                const bool connectionsRestored =
                    repository_.saveConnections(previousConnections, rollbackError);
                const auto rollbackMessage = settingsRestored && connectionsRestored
                                                 ? QString{}
                                                 : "\n恢复原配置也失败：" + rollbackError;
                QMessageBox::warning(this, "工作区导入失败", failure + rollbackMessage);
                return;
            }
            theme_.applyTheme(AppearanceProfile::load().themeId);
            rebuildShortcuts();
            restyle(theme_.dark());
            restoreGeometry(settings.value("window/geometry").toByteArray());
            if (auto* settingsDialog = findChild<SettingsDialog*>())
                settingsDialog->reject();
            QMessageBox::information(
                this, "工作区已导入",
                "设置和连接配置已恢复。所有连接保持断开；已打开的日志文件不会导入。\n\n工作区：" + path);
            return;
        }
        QString path;
        if (name == "backup_workspace") {
            const auto folder = workspaceFolder + "/backups";
            QDir().mkpath(folder);
            path = folder + "/PortWave-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz") +
                   ".swws";
        } else if (name == "save_workspace") {
            path = settings.value("workspace/currentArchive").toString();
        }
        if (path.isEmpty()) {
            const QString suggested = workspaceFolder + "/PortWave-workspace.swws";
            path = QFileDialog::getSaveFileName(this,
                                                name == "new_workspace"      ? "新建工作区包"
                                                : name == "copy_workspace"   ? "另存工作区包"
                                                : name == "rename_workspace" ? "重命名工作区包"
                                                                             : "导出整个工作区",
                                                suggested, "PortWave 工作区 (*.swws)");
            if (path.isEmpty())
                return;
            if (!path.endsWith(".swws", Qt::CaseInsensitive))
                path += ".swws";
            if (name == "rename_workspace") {
                const auto old = settings.value("workspace/currentArchive").toString();
                if (old.isEmpty() || !QFile::exists(old) || !QFile::rename(old, path)) {
                    QMessageBox::warning(this, "工作区重命名失败",
                                         "当前工作区包不存在，或目标文件无法创建。");
                    return;
                }
                settings.setValue("workspace/currentArchive", path);
                statusBar()->showMessage("工作区包已重命名", 5000);
                return;
            }
        }
        QString error;
        if (!WorkspaceArchive::write(path, settings, repository_.loadConnections(), error)) {
            QMessageBox::warning(this, "工作区保存失败", error);
            return;
        }
        if (name != "backup_workspace")
            settings.setValue("workspace/currentArchive", path);
        QMessageBox::information(
            this, "工作区已保存",
            "设置与连接配置已保存。包不包含日志原文件或已打开的日志标签；导入后连接保持断开。\n\n" + path);
        return;
    }
    auto* record = current();
    if (!record)
        return;
    if (name == "new_command") {
        record->pane->addCommand();
        return;
    }
    if (name == "import_commands") {
        record->pane->importCommandGroup();
        return;
    }
    if (name == "export_commands") {
        record->pane->exportCommandGroup();
        return;
    }
    if (name == "command_group_new") {
        record->pane->manageCommandGroupFromSettings("new");
        return;
    }
    if (name == "command_group_copy") {
        record->pane->manageCommandGroupFromSettings("copy");
        return;
    }
    if (name == "command_group_rename") {
        record->pane->manageCommandGroupFromSettings("rename");
        return;
    }
    if (name == "command_group_delete") {
        record->pane->manageCommandGroupFromSettings("delete");
        return;
    }
    auto config = record->pane->currentConfiguration();
    if (name == "search") {
        record->pane->find();
        return;
    }
    if (name == "reconnect" || name == "clean_reconnect") {
        if (name == "clean_reconnect")
            record->pane->terminal()->clear();
        record->session->reconnect();
    } else if (name == "disconnect")
        record->session->stop();
    else if (name == "send_panel")
        record->pane->toggleSender();
    else if (name == "display_settings")
        record->pane->showDisplayOptions(mapToGlobal(rect().center()));
    else if (name == "display_text" || name == "display_hex" || name == "display_mixed") {
        config.displayMode = name == "display_text" ? 0 : name == "display_hex" ? 1 : 2;
        record->pane->applyConfiguration(config);
    } else if (name == "timestamp_display") {
        config.timestampDisplay = !config.timestampDisplay;
        record->pane->applyConfiguration(config);
    } else if (name == "timestamp_log") {
        config.timestampLog = !config.timestampLog;
        record->pane->applyConfiguration(config);
    } else if (name == "pause_receive") {
        config.pauseReceive = !config.pauseReceive;
        record->pane->applyConfiguration(config);
    } else if (name == "pause_display") {
        config.pauseDisplay = !config.pauseDisplay;
        record->pane->applyConfiguration(config);
    } else if (name == "error_display") {
        config.showErrors = !config.showErrors;
        record->pane->applyConfiguration(config);
    } else if (name == "auto_wrap") {
        config.wrap = !config.wrap;
        record->pane->applyConfiguration(config);
    } else if (name == "send_panel")
        record->pane->toggleSender();
    else
        statusBar()->showMessage("当前连接不支持操作：" + name, 3000);
}
} // namespace portwave
