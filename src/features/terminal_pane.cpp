#include "features/terminal_pane.h"
#include "features/window_caption.h"
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QTextBlock>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidgetAction>

namespace portwave {
namespace {
class ActiveOutline final : public QWidget {
  public:
    explicit ActiveOutline(QWidget* parent) : QWidget(parent) {
        setObjectName("activeConnectionOutline");
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(palette().color(QPalette::Accent), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 4, 4);
    }
};
} // namespace
TerminalPane::TerminalPane(Session* session, QWidget* parent)
    : QWidget(parent), session_(session), firmwarePath_(session->config().firmware) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(2, 2, 2, 2);
    activeOutline_ = new ActiveOutline(this);
    activeOutline_->hide();
    root->setSpacing(0);
    header_ = new QWidget(this);
    header_->setObjectName("terminalPaneHeader");
    auto* headerLayout = new QHBoxLayout(header_);
    headerLayout->setContentsMargins(8, 2, 4, 2);
    stateDot_ = new QLabel("●");
    stateDot_->setAttribute(Qt::WA_TransparentForMouseEvents);
    headerLayout->addWidget(stateDot_);
    title_ = new QLabel(session_->config().name);
    title_->setAttribute(Qt::WA_TransparentForMouseEvents);
    headerLayout->addWidget(title_, 1);
    auto* dock = new QToolButton;
    dock->setObjectName("dockTerminal");
    dock->setText("↥");
    dock->setFixedSize(34, 32);
    header_->setMinimumHeight(36);
    dock->setToolTip("停靠到主窗口");
    connect(dock, &QToolButton::clicked, this, &TerminalPane::dockRequested);
    headerLayout->addWidget(dock);
    root->addWidget(header_);
    header_->hide();
    terminal_ = new TerminalView(this);
    terminal_->document()->setMaximumBlockCount(session->config().bufferLines);
    terminal_->setNewline(session->config().newline);
    root->addWidget(terminal_, 1);
    controls_ = new QWidget(this);
    controls_->setObjectName("connectionControls");
    auto* controlsLayout = new QVBoxLayout(controls_);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(0);
    auto* options = new QHBoxLayout;
    options->setContentsMargins(8, 2, 8, 2);
    mode_ = new QComboBox;
    mode_->addItems({"文本", "HEX", "混合"});
    options->addWidget(mode_);
    auto* pause = new QCheckBox("暂停显示");
    pauseDisplayCheck_ = pause;
    options->addWidget(pause);
    auto* receive = new QCheckBox("暂停接收");
    pauseReceiveCheck_ = receive;
    options->addWidget(receive);
    auto* wrap = new QCheckBox("自动换行");
    wrapCheck_ = wrap;
    wrap->setChecked(true);
    options->addWidget(wrap);
    options->addStretch();
    auto* clear = new QPushButton("清空");
    options->addWidget(clear);
    displayOptions_ = new QWidget(this);
    displayOptions_->setLayout(options);
    displayOptions_->hide();
    terminal_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(clear, &QPushButton::clicked, terminal_, &TerminalView::resetTerminal);
    connect(mode_, &QComboBox::currentIndexChanged, terminal_, &TerminalView::setDisplayMode);
    connect(pause, &QCheckBox::toggled, this, &TerminalPane::pauseDisplay);
    connect(receive, &QCheckBox::toggled, session_, &Session::pauseReceive);
    connect(receive, &QCheckBox::toggled, this, [this](bool value) {
        terminal_->setReceivePaused(value);
        emit receivePauseChanged(value);
    });
    connect(terminal_, &TerminalView::resumeReceiveRequested, this, [this] { setReceivePaused(false); });
    connect(wrap, &QCheckBox::toggled, this, &TerminalPane::setWrap);
    auto* shortcutsRow = new QWidget;
    shortcutsRow->setObjectName("shortcutBar");
    shortcutsRow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* shortcutLayout = new QHBoxLayout(shortcutsRow);
    shortcutLayout->setContentsMargins(14, 4, 14, 4);
    group_ = new QComboBox;
    group_->setFixedWidth(90);
    auto settings = QSettings();
    auto groupDocument = QJsonDocument::fromJson(settings.value("commands/groups").toByteArray());
    auto groups = groupDocument.isObject() ? groupDocument.object() : QJsonObject{};
    if (!groups.contains("默认")) {
        const auto legacy = QJsonDocument::fromJson(
            settings
                .value("commands/default",
                       QByteArray("[{\"name\":\"AT\",\"text\":\"AT\\\\r\",\"hex\":false},"
                                  "{\"name\":\"状态\",\"text\":\"状态\\\\r\",\"hex\":false},"
                                  "{\"name\":\"复位\",\"text\":\"复位\\\\r\",\"hex\":false}]"))
                .toByteArray());
        groups.insert("默认", legacy.isArray() ? legacy.array() : QJsonArray{});
        settings.setValue("commands/groups", QJsonDocument(groups).toJson(QJsonDocument::Compact));
    }
    QStringList groupNames = groups.keys();
    groupNames.removeAll("默认");
    groupNames.sort(Qt::CaseInsensitive);
    group_->addItem("默认");
    group_->addItems(groupNames);
    shortcutLayout->addWidget(group_);
    auto* scroll = new QScrollArea;
    scroll->setObjectName("commandsScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFixedHeight(26);
    shortcuts_ = new QWidget;
    auto* commandsLayout = new QHBoxLayout(shortcuts_);
    commandsLayout->setContentsMargins(0, 0, 0, 0);
    commandsLayout->setSpacing(8);
    scroll->setWidget(shortcuts_);
    shortcutLayout->addWidget(scroll, 1);
    controlsLayout->addWidget(shortcutsRow, 0);
    commands_ = groups[activeCommandGroup_].toArray();
    rebuildShortcuts();
    group_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(group_, &QComboBox::currentTextChanged, this, [this](const QString& name) {
        if (name.isEmpty() || name == activeCommandGroup_)
            return;
        saveCommandGroup();
        activeCommandGroup_ = name;
        const auto groups =
            QJsonDocument::fromJson(QSettings().value("commands/groups").toByteArray()).object();
        commands_ = groups.value(activeCommandGroup_).toArray();
        rebuildShortcuts();
    });
    connect(group_, &QWidget::customContextMenuRequested, this, [this](const QPoint& p) {
        QMenu menu;
        menu.addAction("新建命令组…", this, [this] { manageCommandGroup("new"); });
        menu.addAction("复制当前组…", this, [this] { manageCommandGroup("copy"); });
        menu.addAction("重命名当前组…", this, [this] { manageCommandGroup("rename"); });
        auto* remove = menu.addAction("删除当前组", this, [this] { manageCommandGroup("delete"); });
        remove->setEnabled(activeCommandGroup_ != "默认");
        menu.addSeparator();
        menu.addAction("导出当前组到 TXT…", this, &TerminalPane::exportCommands);
        menu.addAction("从 TXT 导入组…", this, &TerminalPane::importCommands);
        menu.exec(group_->mapToGlobal(p));
    });
    sender_ = new QWidget;
    sender_->setObjectName("terminalSend");
    auto* sendRoot = new QVBoxLayout(sender_);
    sendRoot->setContentsMargins(14, 6, 14, 6);
    sendRoot->setSpacing(8);
    sender_->setMinimumHeight(88);
    auto* optionsPanel = new QWidget;
    optionsPanel->setObjectName("sendOptionsPanel");
    auto* sendOptions = new QVBoxLayout(optionsPanel);
    sendOptions->setContentsMargins(12, 10, 12, 10);
    target_ = new QComboBox;
    target_->setObjectName("sendTargetCombo");
    target_->addItem(session->config().name, session->config().id);
    target_->setMinimumWidth(180);
    sendOptions->addWidget(new QLabel("发送目标"));
    sendOptions->addWidget(target_);
    connect(target_, &QComboBox::activated, this, [this](int) {
        if (auto* menu = controls_->findChild<QMenu*>("sendOptionsPopup"))
            menu->close();
        const auto requested = target_->currentData().toString();
        {
            const QSignalBlocker blocker(target_);
            target_->setCurrentIndex(target_->findData(session_->config().id));
        }
        emit targetRequested(requested);
    });
    hex_ = new QCheckBox("HEX");
    cr_ = new QCheckBox("回车 CR");
    lf_ = new QCheckBox("换行 LF");
    currentLine_ = new QCheckBox("发送当前行");
    advance_ = new QCheckBox("逐行前进");
    automatic_ = new QCheckBox("自动发送");
    cr_->setChecked(true);
    currentLine_->setChecked(true);
    currentLine_->setParent(sender_);
    currentLine_->hide();
    advance_->setParent(sender_);
    advance_->hide();
    for (auto* w : {hex_, cr_, lf_, automatic_, currentLine_, advance_})
        sendOptions->addWidget(w);
    auto* intervalRow = new QHBoxLayout;
    intervalRow->addWidget(new QLabel("间隔"));
    interval_ = new QDoubleSpinBox;
    interval_->setDecimals(0);
    interval_->setRange(10, 3600000);
    interval_->setValue(1000);
    interval_->setObjectName("sendInterval");
    intervalRow->addWidget(interval_);
    unit_ = new QComboBox;
    unit_->setFixedWidth(72);
    interval_->setFixedWidth(100);
    unit_->addItems({"ms", "s"});
    unit_->setObjectName("sendIntervalUnit");
    intervalRow->addWidget(unit_);
    sendOptions->addLayout(intervalRow);
    connect(unit_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const double milliseconds = index == 1 ? interval_->value() : interval_->value() * 1000;
        interval_->blockSignals(true);
        interval_->setDecimals(index == 1 ? 3 : 0);
        interval_->setRange(index == 1 ? 0.01 : 10, index == 1 ? 3600 : 3600000);
        interval_->setValue(index == 1 ? milliseconds / 1000 : milliseconds);
        interval_->blockSignals(false);
    });
    currentLine_->show();
    advance_->show();
    auto* sendRow = new QHBoxLayout;
    draft_ = new QPlainTextEdit;
    draft_->setObjectName("sendEdit");
    draft_->setPlaceholderText("输入待发送内容；HEX 示例：AA 55 01");
    draft_->setMinimumHeight(42);
    draft_->setFont(QFont("Consolas", 12));
    auto* sendButton = new QPushButton("发送");
    sendButton->setObjectName("sendNow");
    sendButton->setFixedWidth(68);
    sendButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    sendButton->setProperty("primary", true);
    auto* sendMenu = new QToolButton;
    sendMenu->setObjectName("sendOptionsMenu");
    sendMenu->setToolTip("发送选项：HEX、回车、换行、自动发送");
    sendMenu->setFixedWidth(30);
    sendMenu->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    auto* optionsPopup = new QMenu(controls_);
    optionsPopup->setObjectName("sendOptionsPopup");
    auto* optionsAction = new QWidgetAction(optionsPopup);
    optionsAction->setDefaultWidget(optionsPanel);
    optionsPopup->addAction(optionsAction);
    sendMenu->setMenu(optionsPopup);
    sendMenu->setPopupMode(QToolButton::InstantPopup);
    auto* history = new QToolButton;
    history->setObjectName("sendHistory");
    history->setToolTip("发送历史：仅填入草稿，不自动发送");
    history->setFixedWidth(30);
    history->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    auto* historyPopup = new QMenu(controls_);
    historyPopup->setObjectName("sendHistoryPopup");
    history->setMenu(historyPopup);
    history->setPopupMode(QToolButton::InstantPopup);
    auto* historyPanel = new QWidget;
    historyPanel->setObjectName("sendHistoryPanel");
    historyPanel->setMinimumWidth(420);
    auto* historyLayout = new QVBoxLayout(historyPanel);
    historyLayout->setContentsMargins(12, 12, 12, 12);
    historyLayout->addWidget(new QLabel("发送历史"));
    auto* historyList = new QListWidget;
    historyList->setObjectName("sendHistoryList");
    historyList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    historyList->setTextElideMode(Qt::ElideRight);
    historyLayout->addWidget(historyList);
    historyLayout->addWidget(new QLabel("选择记录填入发送框"));
    auto* historyAction = new QWidgetAction(historyPopup);
    historyAction->setDefaultWidget(historyPanel);
    historyPopup->addAction(historyAction);
    connect(historyPopup, &QMenu::aboutToShow, this, [this, historyList] {
        historyList->clear();
        for (const auto& text : sendHistory_) {
            auto label = text;
            label.replace('\n', " ↵ ");
            auto* item = new QListWidgetItem(label.left(180), historyList);
            item->setData(Qt::UserRole, text);
            item->setToolTip(text);
            item->setSizeHint(QSize(0, 40));
        }
        if (sendHistory_.isEmpty()) {
            auto* empty = new QListWidgetItem("暂无发送历史", historyList);
            empty->setFlags(Qt::NoItemFlags);
        }
        historyList->setFixedHeight(qBound(86, historyList->count() * 42 + 4, 280));
    });
    const auto chooseHistory = [this, historyPopup](QListWidgetItem* item) {
        if (!item || !item->data(Qt::UserRole).isValid())
            return;
        draft_->setPlainText(item->data(Qt::UserRole).toString());
        historyPopup->close();
        draft_->setFocus();
    };
    connect(historyList, &QListWidget::itemClicked, this, chooseHistory);
    connect(historyList, &QListWidget::itemActivated, this, chooseHistory);
    sendRow->addWidget(sendMenu);
    sendRow->addWidget(sendButton);
    sendRow->addWidget(history);
    sendRow->addWidget(draft_, 1);
    sendRoot->addLayout(sendRow, 1);
    senderPin_ = new QToolButton(draft_);
    senderPin_->setObjectName("senderPin");
    senderPin_->setToolTip("固定各连接的发送区高度");
    senderPin_->setCheckable(true);
    senderPin_->setChecked(QSettings().value("sender/pinned", true).toBool());
    senderPin_->setIcon(pinIcon(senderPin_->isChecked()));
    senderPin_->setFixedSize(22, 22);
    draft_->installEventFilter(this);
    sendHint_ = new QLabel(sender_);
    sendHint_->setObjectName("sendHint");
    sendHint_->setAttribute(Qt::WA_TransparentForMouseEvents);
    sendHint_->hide();
    sendHintTimer_.setSingleShot(true);
    connect(&sendHintTimer_, &QTimer::timeout, sendHint_, &QLabel::hide);
    sender_->installEventFilter(this);
    connect(senderPin_, &QToolButton::toggled, this, [this](bool enabled) {
        senderPin_->setIcon(pinIcon(enabled));
        QSettings().setValue("sender/pinned", enabled);
        emit senderPinChanged(enabled);
    });
    draft_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(draft_, &QWidget::customContextMenuRequested, this, [this](const QPoint& p) {
        auto* menu = draft_->createStandardContextMenu();
        menu->addSeparator();
        auto* current = menu->addAction("发送当前行");
        current->setCheckable(true);
        current->setChecked(currentLine_->isChecked());
        connect(current, &QAction::toggled, currentLine_, &QCheckBox::setChecked);
        auto* advance = menu->addAction("逐行前进");
        advance->setCheckable(true);
        advance->setChecked(advance_->isChecked());
        connect(advance, &QAction::toggled, advance_, &QCheckBox::setChecked);
        menu->exec(draft_->mapToGlobal(p));
        delete menu;
    });
    auto* firmwareRow = new QHBoxLayout;
    auto* firmwareLabel = new QLabel(firmwarePath_.isEmpty() ? "未选择固件" : firmwarePath_);
    firmwareLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    firmwareRow->addWidget(firmwareLabel, 1);
    auto* select = new QPushButton("选择 .bin…");
    auto* upgrade = new QPushButton("开始升级");
    auto* cancel = new QPushButton("取消升级");
    progress_ = new QProgressBar;
    progress_->setRange(0, 100);
    progress_->setMaximumWidth(160);
    firmwareRow->addWidget(select);
    firmwareRow->addWidget(upgrade);
    firmwareRow->addWidget(cancel);
    firmwareRow->addWidget(progress_);
    if (session->config().upgrade)
        sendRoot->addLayout(firmwareRow);
    else {
        auto* unused = new QWidget;
        unused->setLayout(firmwareRow);
        unused->setParent(sender_);
        unused->hide();
    }
    connect(select, &QPushButton::clicked, this, [this, firmwareLabel] {
        auto path = QFileDialog::getOpenFileName(this, "选择固件", {}, "Firmware (*.bin)");
        if (!path.isEmpty()) {
            firmwarePath_ = path;
            firmwareLabel->setText(path);
            firmwareLabel->setToolTip(path);
        }
    });
    connect(upgrade, &QPushButton::clicked, this, [this] {
        automatic_->setChecked(false);
        steps_.clear();
        stepTimer_.stop();
        if (firmwarePath_.isEmpty()) {
            emit status("请先选择 .bin 固件");
            return;
        }
        session_->upgrade(firmwarePath_);
    });
    connect(cancel, &QPushButton::clicked, session_, &Session::cancelUpgrade);
    connect(session_, &Session::upgradeProgress, progress_, &QProgressBar::setValue);
    connect(session_, &Session::upgradeFinished, this,
            [this](bool, const QString& msg) { emit status(msg); });
    controlsLayout->addWidget(sender_, 1);
    sender_->setVisible(true);
    path_ = new QLabel;
    path_->setObjectName("logFileStatus");
    path_->setTextFormat(Qt::PlainText);
    path_->setMinimumWidth(0);
    path_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    path_->installEventFilter(this);
    controlsLayout->addWidget(path_);
    root->addWidget(controls_);
    path_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(path_, &QWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        QMenu menu;
        auto* open = menu.addAction("打开文件", this, [this] { openLogFile(LogOpenAction::ExternalFile); });
        auto* browse =
            menu.addAction("在 PortWave 中查看", this, [this] { openLogFile(LogOpenAction::PortWave); });
        auto* folder = menu.addAction("打开所在文件夹", this, [this] { openLogFile(LogOpenAction::Folder); });
        open->setEnabled(QFileInfo::exists(logPath_));
        browse->setEnabled(open->isEnabled());
        folder->setEnabled(!logPath_.isEmpty());
        menu.exec(path_->mapToGlobal(point));
    });
    connect(session_, &Session::logPathChanged, this, [this](const QString& path) {
        logPath_ = path;
        refreshStatus();
    });
    connect(terminal_, &TerminalView::input, session_, &Session::send);
    connect(sendButton, &QPushButton::clicked, this, &TerminalPane::send);
    sendShortcut_ = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), draft_);
    sendShortcut_->setContext(Qt::WidgetShortcut);
    connect(sendShortcut_, &QShortcut::activated, this, &TerminalPane::send);
    connect(terminal_, &TerminalView::reconnectRequested, this, [this] {
        if (session_->connected())
            return;
        stateMessage_ = "正在重新连接…";
        refreshStatus();
        session_->reconnect();
    });
    connect(automatic_, &QCheckBox::toggled, this, [this](bool value) {
        if (value)
            autoSend_.start(qRound(interval_->value() * (unit_->currentIndex() == 1 ? 1000 : 1)));
        else
            autoSend_.stop();
    });
    connect(interval_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (autoSend_.isActive())
            autoSend_.setInterval(qRound(value * (unit_->currentIndex() == 1 ? 1000 : 1)));
    });
    connect(&autoSend_, &QTimer::timeout, this, &TerminalPane::send);
    stepTimer_.setSingleShot(true);
    connect(&stepTimer_, &QTimer::timeout, this, &TerminalPane::nextStep);
    connect(session_, &Session::stateChanged, this, [this](bool connected, const QString& message) {
        stateMessage_ = message;
        everConnected_ = everConnected_ || connected;
        // Keyboard reconnect only applies to a session that was connected and then
        // disconnected; a never-connected pane keeps Space as terminal input.
        terminal_->setDisconnected(everConnected_ && !connected);
        if (!connected) {
            steps_.clear();
            stepTimer_.stop();
            automatic_->setChecked(false);
        }
        refreshStatus();
    });
    connect(session_, &Session::configChanged, this, [this] { refreshStatus(); });
    refreshStatus();
    connect(&refresh_, &QTimer::timeout, this, [this] {
        if (refresh_.interval() != 50)
            refresh_.setInterval(50);
        if (pausedDisplay_ || interactionPaused_ || pauseReceiveCheck_->isChecked()) {
            return;
        }
        if (catchUpLatest_) {
            catchUpLatest_ = false;
            bool skipped = false;
            const auto bytes = session_->takeLatest(16384, skipped);
            terminal_->resumeLatest(bytes, skipped);
            if (finder_)
                finder_->tickLive();
            return;
        }
        auto bytes = session_->take(isVisible() ? 16384 : 4096);
        if (!bytes.empty()) {
            terminal_->consume(bytes);
            if (finder_)
                finder_->tickLive();
        }
    });
    refresh_.start(50);
    applyConfiguration(session_->config());
    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(200);
    connect(&saveTimer_, &QTimer::timeout, this, [this] {
        auto config = currentConfiguration();
        if (config.toJson() != session_->config().toJson()) {
            session_->applyConfig(config);
        }
    });
    auto changed = [this] {
        if (!applying_)
            saveTimer_.start();
    };
    for (auto* check : {hex_, cr_, lf_, currentLine_, advance_, automatic_, pauseReceiveCheck_,
                        pauseDisplayCheck_, wrapCheck_})
        connect(check, &QCheckBox::toggled, this, changed);
    connect(mode_, &QComboBox::currentIndexChanged, this, changed);
    connect(group_, &QComboBox::currentTextChanged, this, changed);
    connect(draft_, &QPlainTextEdit::textChanged, this, changed);
    connect(interval_, &QDoubleSpinBox::valueChanged, this, changed);
    connect(unit_, &QComboBox::currentIndexChanged, this, changed);
}
TerminalPane::~TerminalPane() {
    delete controls_.data();
    delete path_.data();
}
void TerminalPane::openLogFile(LogOpenAction action) {
    if (logPath_.isEmpty())
        return;
    if (action != LogOpenAction::Folder && !QFileInfo(logPath_).isFile()) {
        emit status("日志文件不存在或已被移动");
        return;
    }
    if (action == LogOpenAction::PortWave) {
        emit openLog(logPath_);
        return;
    }
    bool opened = false;
    if (action == LogOpenAction::Folder) {
        opened = QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(logPath_).absolutePath()));
    } else {
        const auto preferences = SettingsRepository{}.loadLogOpenPreferences();
        const auto program = preferences.program.trimmed();
        if (preferences.customProgram && !program.isEmpty()) {
            const QFileInfo executable(program);
            if (!executable.isAbsolute() || !executable.isFile() || !executable.isExecutable()) {
                emit status("指定的日志打开程序不可用，请在设置 → 日志文件中重新选择");
                return;
            }
            // Pass the log path as one argument, including spaces; never invoke a shell.
            opened = QProcess::startDetached(program, QStringList{logPath_});
        } else {
            opened = QDesktopServices::openUrl(QUrl::fromLocalFile(logPath_));
        }
    }
    if (!opened)
        emit status("无法打开日志文件或文件夹，请检查打开程序及系统文件关联");
}
bool TerminalPane::eventFilter(QObject* object, QEvent* event) {
    if (object == draft_ && event->type() == QEvent::Resize && senderPin_)
        senderPin_->move(draft_->width() - senderPin_->width() - 5, 5);
    if ((object == draft_ || object == sender_) && event->type() == QEvent::Resize)
        positionSendHint();
    if (object == path_ && event->type() == QEvent::MouseButtonDblClick && !logPath_.isEmpty() &&
        static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        openLogFile(SettingsRepository{}.loadLogOpenPreferences().action);
        return true;
    }
    return QWidget::eventFilter(object, event);
}
void TerminalPane::setSenderPinned(bool enabled) {
    const QSignalBlocker blocker(senderPin_);
    senderPin_->setChecked(enabled);
    senderPin_->setIcon(pinIcon(enabled));
}
int TerminalPane::storedSenderHeight(bool shared) const {
    const auto key = shared ? QString("sender/sharedHeight") : "sender/heights/" + session_->config().id;
    return qBound(80, QSettings().value(key, 128).toInt(), 10000);
}
void TerminalPane::rememberSenderHeight(int height, bool shared) {
    if (height < 80 || height > 10000)
        return;
    const auto key = shared ? QString("sender/sharedHeight") : "sender/heights/" + session_->config().id;
    QSettings settings;
    if (settings.value(key).toInt() != height)
        settings.setValue(key, height);
}
void TerminalPane::setTheme(bool dark) {
    senderPin_->setIcon(pinIcon(senderPin_->isChecked()));
    terminal_->setTheme(dark);
    auto* send = controls_->findChild<QPushButton*>("sendNow");
    send->setFixedWidth(108);
    send->setIcon(sendControlIcon(2, appearanceColor("button.primaryForeground", dark, Qt::white)));
    auto* options = controls_->findChild<QToolButton*>("sendOptionsMenu");
    auto* history = controls_->findChild<QToolButton*>("sendHistory");
    options->setIcon(sendControlIcon(0, palette().color(QPalette::WindowText)));
    history->setIcon(sendControlIcon(1, palette().color(QPalette::WindowText)));
    options->show();
    history->show();
    refreshStatus();
}
void TerminalPane::setSendShortcut(const QKeySequence& sequence) {
    if (!sendShortcut_)
        return;
    sendShortcut_->setKey(sequence);
    sendShortcut_->setEnabled(!sequence.isEmpty());
}
void TerminalPane::setFindShortcuts(const QHash<QString, QKeySequence>& overrides) {
    findShortcutOverrides_ = overrides;
    if (finder_)
        finder_->setShortcutOverrides(overrides);
}
void TerminalPane::setReceivePaused(bool paused) {
    if (pauseReceiveCheck_->isChecked() == paused)
        return;
    pauseReceiveCheck_->setChecked(paused);
    session_->applyConfig(currentConfiguration());
    refreshStatus();
}
void TerminalPane::setInteractionPaused(bool paused) {
    if (interactionPaused_ == paused)
        return;
    interactionPaused_ = paused;
    if (!paused) {
        catchUpLatest_ = true;
        // Defer catch-up until the final drop geometry/layout has settled.
        if (refresh_.isActive())
            refresh_.start(0);
    }
}
void TerminalPane::setActiveConnection(bool active) {
    setProperty("activeConnection", active);
    activeOutline_->setGeometry(rect());
    activeOutline_->setVisible(active);
    if (active)
        activeOutline_->raise();
}
void TerminalPane::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (activeOutline_)
        activeOutline_->setGeometry(rect());
}
void TerminalPane::setDetached(bool value) {
    header_->setVisible(value);
}
QString TerminalPane::connectionStatusText() const {
    return connectionStatusText_;
}
void TerminalPane::refreshStatus() {
    const auto& config = session_->config();
    const QString endpoint = config.kind == "tcp" ? QString("TCP %1:%2").arg(config.host).arg(config.tcpPort)
                             : config.port.isEmpty() ? config.name
                                                     : config.port;
    const QString state = session_->connected()     ? endpoint + " 已连接"
                          : stateMessage_.isEmpty() ? endpoint + " 已断开"
                                                    : stateMessage_;
    connectionStatusText_ = QString("%1  接收模式：%2")
                                .arg(endpoint == config.name ? state : config.name + " · " + state,
                                     config.upgrade ? "升级协议解析" : "常规串口");
    emit connectionStatusChanged(connectionStatusText_);
    title_->setText(config.name);
    const bool dark = qApp->property("portwave.theme.dark").isValid()
                          ? qApp->property("portwave.theme.dark").toBool()
                          : true;
    const QString role = !session_->connected() || config.pauseDisplay ? "status.disconnected"
                         : config.pauseReceive                         ? "status.paused"
                                                                       : "status.connected";
    const QColor fallback = role == "status.disconnected" ? QColor("#e06c75")
                            : role == "status.paused"     ? QColor("#e5c07b")
                                                          : QColor("#98c379");
    stateDot_->setStyleSheet(QString("color:%1;").arg(appearanceColor(role, dark, fallback).name()));
    title_->setToolTip(connectionStatusText_);
    if (!logPath_.isEmpty()) {
        path_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        path_->setText(QFileInfo(logPath_).fileName() +
                       (session_->connected() ? QString{} : QString("（已停止）")));
        path_->setToolTip(logPath_ + "\n双击按设置中的默认打开方式操作；右键选择打开方式");
        path_->setCursor(Qt::PointingHandCursor);
    } else {
        path_->setText(config.autoLog || config.rotate ? "存盘文件：等待连接" : "存盘文件：未启用");
        path_->setToolTip({});
        path_->setCursor(Qt::ArrowCursor);
    }
}
ConnectionConfig TerminalPane::currentConfiguration() const {
    auto c = session_->config();
    c.sendDraft = draft_->toPlainText();
    c.sendHex = hex_->isChecked();
    c.sendCR = cr_->isChecked();
    c.sendLF = lf_->isChecked();
    c.sendCurrentLine = currentLine_->isChecked();
    c.advanceLine = advance_->isChecked();
    c.automaticSend = automatic_->isChecked();
    c.sendIntervalUnit = unit_->currentIndex();
    c.sendIntervalMs = qRound(interval_->value() * (unit_->currentIndex() == 1 ? 1000 : 1));
    c.displayMode = mode_->currentIndex();
    c.pauseReceive = pauseReceiveCheck_->isChecked();
    c.pauseDisplay = pauseDisplayCheck_->isChecked();
    c.wrap = wrapCheck_->isChecked();
    c.commandGroup = activeCommandGroup_;
    return c;
}
void TerminalPane::applyConfiguration(const ConnectionConfig& c) {
    applying_ = true;
    if (draft_->toPlainText() != c.sendDraft)
        draft_->setPlainText(c.sendDraft);
    hex_->setChecked(c.sendHex);
    cr_->setChecked(c.sendCR);
    lf_->setChecked(c.sendLF);
    currentLine_->setChecked(c.sendCurrentLine);
    advance_->setChecked(c.advanceLine);
    unit_->setCurrentIndex(c.sendIntervalUnit);
    interval_->setValue(c.sendIntervalUnit == 1 ? static_cast<double>(c.sendIntervalMs) / 1000.0
                                                : c.sendIntervalMs);
    {
        const QSignalBlocker blocker(automatic_);
        automatic_->setChecked(c.automaticSend);
    }
    if (!c.automaticSend)
        autoSend_.stop();
    mode_->setCurrentIndex(c.displayMode);
    pauseReceiveCheck_->setChecked(c.pauseReceive);
    pauseDisplayCheck_->setChecked(c.pauseDisplay);
    wrapCheck_->setChecked(c.wrap);
    const auto nextGroup = c.commandGroup.isEmpty() ? QStringLiteral("默认") : c.commandGroup;
    if (nextGroup != activeCommandGroup_)
        setCommandGroup(nextGroup);
    terminal_->setNewline(c.newline);
    terminal_->setTimestampOptions(c.timestampDisplay, c.timestampInterval);
    terminal_->setScrollMode(QSettings().value("view/scrollMode", 0).toInt());
    if (terminal_->document()->maximumBlockCount() != c.bufferLines)
        terminal_->document()->setMaximumBlockCount(c.bufferLines);
    session_->applyConfig(c);
    applying_ = false;
}
void TerminalPane::setTerminalAppearance(const QFont& font, const QColor& background,
                                         const QColor& foreground, bool smooth) {
    auto f = font;
    f.setStyleStrategy(smooth ? QFont::PreferAntialias : QFont::NoAntialias);
    terminal_->setFont(f);
    terminal_->document()->setDefaultFont(f);
    terminal_->setStyleSheet(QString("QTextEdit {background:%1;color:%2;font-family:%3;font-size:%4pt;}")
                                 .arg(background.name(), foreground.name(), f.family())
                                 .arg(f.pointSize()));
    terminal_->setDisplayColors(background, foreground);
}
void TerminalPane::setCommands(const QJsonArray& commands) {
    commands_ = commands;
    saveCommandGroup();
    rebuildShortcuts();
    emit commandsChanged(activeCommandGroup_);
}
void TerminalPane::reloadCommandGroup(const QString& name) {
    if (activeCommandGroup_ != name)
        return;
    const auto groups = QJsonDocument::fromJson(QSettings().value("commands/groups").toByteArray()).object();
    commands_ = groups.value(name).toArray();
    rebuildShortcuts();
}
void TerminalPane::setCommandGroup(const QString& name) {
    const auto index = group_->findText(name);
    if (index >= 0)
        group_->setCurrentIndex(index);
}
void TerminalPane::saveCommandGroup() {
    QSettings settings;
    auto groups = QJsonDocument::fromJson(settings.value("commands/groups").toByteArray()).object();
    groups.insert(activeCommandGroup_, commands_);
    settings.setValue("commands/groups", QJsonDocument(groups).toJson(QJsonDocument::Compact));
    if (activeCommandGroup_ == "默认")
        settings.setValue("commands/default", QJsonDocument(commands_).toJson(QJsonDocument::Compact));
}
void TerminalPane::manageCommandGroup(const QString& operation) {
    saveCommandGroup();
    if (operation == "delete") {
        if (activeCommandGroup_ == "默认")
            return;
        auto groups = QJsonDocument::fromJson(QSettings().value("commands/groups").toByteArray()).object();
        groups.remove(activeCommandGroup_);
        QSettings().setValue("commands/groups", QJsonDocument(groups).toJson(QJsonDocument::Compact));
        const auto old = activeCommandGroup_;
        activeCommandGroup_ = "默认";
        {
            const QSignalBlocker blocker(group_);
            group_->removeItem(group_->findText(old));
            group_->setCurrentText(activeCommandGroup_);
        }
        commands_ = groups.value(activeCommandGroup_).toArray();
        rebuildShortcuts();
        emit commandsChanged(old);
        emit commandsChanged(activeCommandGroup_);
        return;
    }
    bool ok = false;
    const QString prompt = operation == "rename" ? "新命令组名称" : "命令组名称";
    const auto name = QInputDialog::getText(this, "命令组", prompt, QLineEdit::Normal, {}, &ok).trimmed();
    if (!ok || name.isEmpty() || name == "默认")
        return;
    auto groups = QJsonDocument::fromJson(QSettings().value("commands/groups").toByteArray()).object();
    if (operation == "rename") {
        if (activeCommandGroup_ == "默认" || groups.contains(name)) {
            emit status("默认组不可重命名，且命令组名称不能重复");
            return;
        }
        groups.remove(activeCommandGroup_);
        groups.insert(name, commands_);
        const auto old = activeCommandGroup_;
        activeCommandGroup_ = name;
        const QSignalBlocker blocker(group_);
        group_->removeItem(group_->findText(old));
        group_->addItem(name);
        group_->setCurrentText(name);
    } else {
        if (groups.contains(name)) {
            emit status("命令组名称已存在");
            return;
        }
        groups.insert(name, operation == "copy" ? QJsonValue(commands_) : QJsonValue(QJsonArray{}));
        QSettings().setValue("commands/groups", QJsonDocument(groups).toJson(QJsonDocument::Compact));
        group_->addItem(name);
        group_->setCurrentText(name);
        return;
    }
    QSettings().setValue("commands/groups", QJsonDocument(groups).toJson(QJsonDocument::Compact));
    rebuildShortcuts();
    emit commandsChanged(activeCommandGroup_);
}
void TerminalPane::setTargets(const QVector<QPair<QString, QString>>& targets) {
    bool same = target_->count() == targets.size();
    for (int i = 0; same && i < targets.size(); ++i)
        same =
            target_->itemText(i) == targets[i].first && target_->itemData(i).toString() == targets[i].second;
    if (same)
        return;
    target_->blockSignals(true);
    target_->clear();
    for (const auto& target : targets)
        target_->addItem(target.first, target.second);
    target_->setCurrentIndex(target_->findData(session_->config().id));
    target_->blockSignals(false);
}
void TerminalPane::showDisplayOptions(const QPoint& position) {
    QDialog popup(this, Qt::Popup);
    auto* layout = new QVBoxLayout(&popup);
    layout->addWidget(displayOptions_);
    displayOptions_->show();
    popup.move(position);
    popup.exec();
    layout->removeWidget(displayOptions_);
    displayOptions_->setParent(this);
    displayOptions_->hide();
}
void TerminalPane::toggleSender() {
    sender_->setVisible(sender_->isHidden());
    emit senderVisibilityChanged(!sender_->isHidden());
}
void TerminalPane::send() {
    if (!session_->connected() || session_->upgrading() || stepTimer_.isActive())
        return;
    auto cursor = draft_->textCursor();
    QString text = currentLine_->isChecked() ? cursor.block().text() : draft_->toPlainText();
    // A blank line must not reach the port: no CR/LF-only packet, no history entry.
    if (text.trimmed().isEmpty()) {
        showSendHint(currentLine_->isChecked() ? QStringLiteral("当前行为空，未发送")
                                               : QStringLiteral("发送内容为空，未发送"));
        return;
    }
    QByteArray bytes;
    if (hex_->isChecked()) {
        auto command = parseCommand(text.toStdString(), true);
        if (!command) {
            emit status(QString::fromStdString(command.error));
            return;
        }
        for (const auto& s : command.steps)
            bytes.append(reinterpret_cast<const char*>(s.data.data()), static_cast<qsizetype>(s.data.size()));
    } else
        bytes = text.toUtf8();
    if (cr_->isChecked())
        bytes += '\r';
    if (lf_->isChecked())
        bytes += '\n';
    session_->send(bytes);
    if (!text.isEmpty() && text.size() <= 4096) {
        sendHistory_.removeAll(text);
        sendHistory_.prepend(text);
        while (sendHistory_.size() > 20)
            sendHistory_.removeLast();
    }
    if (currentLine_->isChecked() && advance_->isChecked()) {
        cursor.movePosition(QTextCursor::NextBlock);
        draft_->setTextCursor(cursor);
    }
}
void TerminalPane::showSendHint(const QString& text) {
    if (!sendHint_)
        return;
    sendHint_->setText(text);
    sendHint_->adjustSize();
    sendHint_->show();
    sendHint_->raise();
    positionSendHint();
    sendHintTimer_.start(2000);
}
void TerminalPane::positionSendHint() {
    if (!sendHint_ || !draft_ || sendHint_->isHidden())
        return;
    sendHint_->adjustSize();
    // Bottom-left inside the draft so the placeholder/top line stays readable.
    const QPoint origin = draft_->mapTo(sender_, QPoint(0, 0));
    const int y = origin.y() + qMax(6, draft_->height() - sendHint_->height() - 6);
    sendHint_->move(origin.x() + 12, y);
}
void TerminalPane::executeShortcut(const QString& command, bool hex, bool appendCR, bool appendLF) {
    if (!session_->connected() || session_->upgrading() || !steps_.empty())
        return;
    auto result = parseCommand(command.toUtf8().toStdString(), hex,
                               QApplication::clipboard()->text().toUtf8().toStdString());
    if (!result) {
        emit status(QString::fromStdString(result.error));
        return;
    }
    if (appendCR || appendLF) {
        CommandStep suffix;
        if (appendCR)
            suffix.data.push_back(13);
        if (appendLF)
            suffix.data.push_back(10);
        result.steps.push_back(std::move(suffix));
    }
    steps_ = std::move(result.steps);
    step_ = 0;
    nextStep();
}
void TerminalPane::nextStep() {
    while (step_ < steps_.size()) {
        const auto& s = steps_[step_++];
        if (s.pause) {
            stepTimer_.start(1000);
            return;
        }
        session_->send(
            QByteArray(reinterpret_cast<const char*>(s.data.data()), static_cast<qsizetype>(s.data.size())));
    }
    steps_.clear();
    step_ = 0;
}
void TerminalPane::rebuildShortcuts() {
    auto* layout = qobject_cast<QHBoxLayout*>(shortcuts_->layout());
    while (auto* item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (int i = 0; i < commands_.size(); ++i) {
        auto object = commands_[i].toObject();
        auto* button = new QPushButton(object["name"].toString());
        button->setObjectName("quickCommand");
        layout->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, object] {
            executeShortcut(object["text"].toString(), object["hex"].toBool(), object["appendCR"].toBool(),
                            object["appendLF"].toBool());
        });
        button->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(button, &QWidget::customContextMenuRequested, this, [this, button, i](const QPoint& p) {
            QMenu menu;
            menu.addAction("编辑…", this, [this, i] { editShortcut(i); });
            menu.addAction("删除", this, [this, i] {
                commands_.removeAt(i);
                saveCommandGroup();
                rebuildShortcuts();
                emit commandsChanged(activeCommandGroup_);
            });
            menu.exec(button->mapToGlobal(p));
        });
    }
    auto* add = new QPushButton("+");
    add->setObjectName("quickCommand");
    add->setFixedWidth(36);
    layout->addWidget(add);
    connect(add, &QPushButton::clicked, this, [this] { editShortcut(-1); });
    layout->addStretch();
}
void TerminalPane::editShortcut(int index) {
    QDialog dialog(this);
    dialog.setWindowTitle("快捷命令");
    dialog.setObjectName("quickCommandDialog");
    auto* form = new QFormLayout(&dialog);
    auto object = index < 0 ? QJsonObject{} : commands_[index].toObject();
    auto* name = new QLineEdit(object["name"].toString());
    auto* text = new QLineEdit(object["text"].toString());
    auto* hex = new QCheckBox("HEX");
    hex->setChecked(object["hex"].toBool());
    form->addRow("名称", name);
    form->addRow("发送字符串", text);
    text->setObjectName("quickCommandText");
    auto* cr = new QCheckBox("追加回车 CR (\\r)");
    auto* lf = new QCheckBox("追加换行 LF (\\n)");
    cr->setObjectName("quickAppendCR");
    lf->setObjectName("quickAppendLF");
    for (auto* option : {cr, lf})
        option->setProperty("fluentToggle", true);
    cr->setChecked(object["appendCR"].toBool());
    lf->setChecked(object["appendLF"].toBool());
    form->addRow(cr);
    form->addRow(lf);
    form->addRow("支持 \\r \\n \\p \\v 与三位八进制", hex);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || name->text().trimmed().isEmpty())
        return;
    auto parsed = parseCommand(text->text().toUtf8().toStdString(), hex->isChecked());
    if (!parsed) {
        emit status(QString::fromStdString(parsed.error));
        return;
    }
    QJsonObject command{{"name", name->text()},
                        {"text", text->text()},
                        {"hex", hex->isChecked()},
                        {"appendCR", cr->isChecked()},
                        {"appendLF", lf->isChecked()}};
    if (index < 0)
        commands_.push_back(command);
    else
        commands_[index] = command;
    saveCommandGroup();
    rebuildShortcuts();
    emit commandsChanged(activeCommandGroup_);
}
void TerminalPane::importCommands() {
    auto path = QFileDialog::getOpenFileName(this, "导入命令组", {}, "Text (*.txt)");
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4LL * 1024 * 1024) {
        emit status("无法读取命令组或文件超过 4 MiB");
        return;
    }
    auto doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject() || !doc.object()["buttons"].isArray()) {
        emit status("命令组格式错误");
        return;
    }
    QJsonArray array;
    const auto sourceName = doc.object()["name"].toString().trimmed();
    for (const auto& value : doc.object()["buttons"].toArray()) {
        const auto source = value.toObject();
        const bool legacy = source.contains("label") || source.contains("command") || source.contains("mode");
        const auto name = legacy ? source["label"].toString() : source["name"].toString();
        const auto text = legacy ? source["command"].toString() : source["text"].toString();
        const bool hex = legacy ? source["mode"].toString() == "hex" : source["hex"].toBool();
        if (name.trimmed().isEmpty() || !source.contains(legacy ? "command" : "text") ||
            !parseCommand(text.toUtf8().toStdString(), hex)) {
            emit status("命令校验失败，原配置保留");
            return;
        }
        array.push_back(QJsonObject{{"name", name},
                                    {"text", text},
                                    {"hex", hex},
                                    {"appendCR", source["appendCR"].toBool()},
                                    {"appendLF", source["appendLF"].toBool()}});
    }
    auto groups = QJsonDocument::fromJson(QSettings().value("commands/groups").toByteArray()).object();
    QString importedName = sourceName.isEmpty() ? QFileInfo(path).completeBaseName() : sourceName;
    if (importedName.isEmpty())
        importedName = "导入命令组";
    if (importedName == "默认")
        importedName = "导入命令组";
    const auto baseName = importedName;
    for (int suffix = 2; groups.contains(importedName); ++suffix)
        importedName = baseName + QString::number(suffix);
    groups.insert(importedName, array);
    QSettings().setValue("commands/groups", QJsonDocument(groups).toJson(QJsonDocument::Compact));
    activeCommandGroup_ = importedName;
    commands_ = array;
    {
        const QSignalBlocker blocker(group_);
        group_->addItem(importedName);
        group_->setCurrentText(importedName);
    }
    rebuildShortcuts();
    emit commandsChanged(activeCommandGroup_);
}
void TerminalPane::exportCommands() {
    auto path = QFileDialog::getSaveFileName(this, "导出命令组", {}, "Text (*.txt)");
    if (path.isEmpty())
        return;
    QSaveFile file(path);
    auto bytes = QJsonDocument(QJsonObject{{"name", group_->currentText()}, {"buttons", commands_}}).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        emit status("命令组保存失败：" + file.errorString());
}
void TerminalPane::find() {
    if (!finder_)
        finder_ = new FindDialog(
            [this](bool) {
                SearchDocument source;
                source.id = session_->config().id;
                source.path = session_->config().name;
                source.text = terminal_->toPlainText();
                source.anchor = terminal_->textCursor().position();
                source.selectionStart = terminal_->textCursor().selectionStart();
                source.selectionEnd = terminal_->textCursor().selectionEnd();
                source.revision = terminal_->document()->revision();
                return QVector<SearchDocument>{source};
            },
            [this](const SearchHit& hit) {
                if (terminal_->toPlainText().mid(hit.position, hit.length) != hit.matched)
                    return false;
                QTextCursor cursor(terminal_->document());
                cursor.setPosition(static_cast<int>(hit.position));
                cursor.setPosition(static_cast<int>(hit.position + hit.length), QTextCursor::KeepAnchor);
                terminal_->setTextCursor(cursor);
                terminal_->ensureCursorVisible();
                return true;
            },
            true, "search/cache/" + session_->config().id, this);
    finder_->setShortcutOverrides(findShortcutOverrides_);
    finder_->setMarkBackground(terminal_->palette().color(QPalette::Base));
    finder_->openFind(terminal_->textCursor().selectedText());
}
void TerminalPane::shutdown() {
    refresh_.stop();
    saveTimer_.stop();
    autoSend_.stop();
    stepTimer_.stop();
    steps_.clear();
}
} // namespace portwave
