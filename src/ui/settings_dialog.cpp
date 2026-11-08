#include "ui/settings_dialog.h"
#include "features/window_caption.h"
#include "ui/keyboard_shortcuts.h"
#include "ui/shortcuts.h"
#include <QApplication>
#include <QColorDialog>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSerialPortInfo>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

namespace portwave {
class ColorButton final : public QPushButton {
  public:
    ColorButton(QWidget* parent = nullptr) : QPushButton(parent) {
        connect(this, &QPushButton::clicked, this, [this] {
            auto next = QColorDialog::getColor(color_, this, "选择颜色");
            if (next.isValid()) {
                setColor(next);
                if (changed)
                    changed();
            }
        });
    }
    QColor color() const { return color_; }
    void setColor(const QColor& color) {
        color_ = color;
        setText(color.name());
        QPixmap swatch(18, 18);
        swatch.fill(color);
        setIcon(QIcon(swatch));
    }
    std::function<void()> changed;

  private:
    QColor color_;
};
QGridLayout* SettingsDialog::section(QVBoxLayout* page, const QString& title) {
    auto* host = new QWidget;
    host->setObjectName("settingsSection");
    auto* root = new QVBoxLayout(host);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(8);
    auto* label = new QLabel(title);
    label->setObjectName("settingsSectionTitle");
    root->addWidget(label);
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(24);
    grid->setVerticalSpacing(8);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    root->addLayout(grid);
    page->addWidget(host);
    return grid;
}
void SettingsDialog::field(QGridLayout* grid, const QString& label, QWidget* widget, const QString& key) {
    auto* cell = new QWidget;
    cell->setObjectName("settingRow_" + (key.isEmpty() ? label : key));
    auto* layout = new QHBoxLayout(cell);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    auto* check = qobject_cast<QCheckBox*>(widget);
    const bool behavior = key == "echo" || key == "newline" || key == "upgrade" || key == "timestampInterval";
    auto* caption = new QLabel(check ? check->text() : label);
    caption->setMinimumWidth(82);
    caption->setProperty("muted", true);
    if (!caption->text().isEmpty())
        layout->addWidget(caption);
    if (check) {
        check->setText({});
        check->setProperty("fluentToggle", true);
        layout->addStretch();
    }
    if (behavior) {
        auto* icon = new QLabel;
        icon->setProperty("settingsIcon", key == "echo"      ? "send"
                                          : key == "newline" ? "log"
                                          : key == "upgrade" ? "power"
                                                             : "settings");
        icon->setPixmap(IconManager::icon(key == "echo"      ? "send"
                                          : key == "newline" ? "log"
                                          : key == "upgrade" ? "power"
                                                             : "settings",
                                          theme_.dark())
                            .pixmap(18, 18));
        layout->insertWidget(0, icon);
        const QString explanation = key == "echo"      ? "输入的内容会在接收区中显示。"
                                    : key == "newline" ? "接收换行时，将光标移到下一行行首。"
                                    : key == "upgrade" ? "按设备升级协议处理通信与日志。"
                                                       : "每隔指定秒数显示时间标签，0 表示每行显示。";
        auto* note = new QLabel(explanation);
        note->setProperty("muted", true);
        note->setWordWrap(true);
        layout->insertWidget(2, note, 1);
        if (!check) {
            widget->setMaximumWidth(170);
            qobject_cast<QSpinBox*>(widget)->setSuffix(" 秒");
        }
    }
    if (key == "kind") {
        widget->hide();
        layout->addWidget(widget);
        auto* choices = new QWidget;
        choices->setObjectName("connectionTypeChoices");
        auto* row = new QHBoxLayout(choices);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(0);
        auto* combo = qobject_cast<QComboBox*>(widget);
        for (int i = 0; i < combo->count(); ++i) {
            auto* button = new QPushButton(combo->itemText(i));
            button->setObjectName("connectionTypeChoice");
            button->setProperty("choiceIndex", i);
            button->setCheckable(true);
            button->setAutoDefault(false);
            row->addWidget(button, 1);
            connect(button, &QPushButton::clicked, combo, [this, combo, i] {
                combo->setCurrentIndex(i);
                updateConnectionType();
            });
        }
        layout->addWidget(choices, 1);
    } else {
        layout->addWidget(widget, check ? 0 : 1);
    }
    int index = grid->property("nextField").toInt();
    if (behavior) {
        index += index % 2;
        grid->addWidget(cell, index / 2, 0, 1, 2);
        grid->setProperty("nextField", index + 2);
    } else {
        grid->addWidget(cell, index / 2, index % 2);
        grid->setProperty("nextField", index + 1);
    }
    if (!key.isEmpty()) {
        widget->setObjectName("setting_" + key);
        widget->setProperty("settingsPage", pages_->count() - 1);
        fields_.insert(key, widget);
    }
}
void SettingsDialog::action(QVBoxLayout* page, const QString& text, const QString& id) {
    auto* button = new QPushButton(text);
    button->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    page->addWidget(button);
    connect(button, &QPushButton::clicked, this, [this, id] {
        emit actionRequested(id);
        if (id.contains("command") || id == "import_commands" || id == "export_commands")
            refreshCommands();
    });
}
SettingsDialog::SettingsDialog(SettingsRepository& repository, ThemeManager& theme,
                               const QVector<TerminalPane*>& panes, LogWorkspace* logs, QWidget* parent,
                               QString category, const QString& initialConnection)
    : QDialog(parent), repository_(repository), theme_(theme), logs_(logs) {
    for (auto* pane : panes)
        panes_.push_back(pane);
    for (const auto& config : repository.loadConnections())
        drafts_.insert(config.id, config);
    for (auto* pane : panes)
        drafts_.insert(pane->session()->config().id, pane->currentConfiguration());
    setWindowTitle("设置");
    setObjectName("approvedSettings");
    setWindowFlag(Qt::FramelessWindowHint);
    resize(1100, 720);
    setMinimumSize(840, 620);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(1, 1, 1, 1);
    root->setSpacing(0);
    root->addWidget(new WindowCaption(this, "PortWave  设置"));
    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    root->addLayout(body, 1);
    auto* sidebar = new QWidget;
    sidebar->setObjectName("settingsSidebar");
    sidebar->setFixedWidth(205);
    auto* side = new QVBoxLayout(sidebar);
    side->setContentsMargins(12, 12, 12, 8);
    side->setSpacing(3);
    settingsSearch_ = new QLineEdit;
    settingsSearch_->setObjectName("settingsSearch");
    settingsSearch_->setPlaceholderText("搜索设置");
    settingsSearch_->setClearButtonEnabled(true);
    side->addWidget(settingsSearch_);
    auto* navScroll = new QScrollArea;
    navScroll->setWidgetResizable(true);
    navScroll->setFrameShape(QFrame::NoFrame);
    side->addWidget(navScroll, 1);
    auto* navHost = new QWidget;
    auto* nav = new QVBoxLayout(navHost);
    nav->setContentsMargins(4, 2, 4, 8);
    nav->setSpacing(2);
    nav->setAlignment(Qt::AlignTop);
    navScroll->setWidget(navHost);
    body->addWidget(sidebar);
    auto* content = new QWidget;
    content->setObjectName("settingsContent");
    auto* contentRoot = new QVBoxLayout(content);
    contentRoot->setContentsMargins(0, 0, 0, 0);
    body->addWidget(content, 1);
    auto* titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(22, 12, 22, 4);
    heading_ = new QLabel;
    heading_->setObjectName("settingsHeading");
    titleRow->addWidget(heading_);
    titleRow->addStretch();
    scopeHint_ = new QLabel("作用范围：全局");
    scopeHint_->setProperty("muted", true);
    scopeHint_->hide();
    titleRow->addWidget(scopeHint_);
    contentRoot->addLayout(titleRow);
    description_ = new QLabel;
    description_->setWordWrap(true);
    description_->setProperty("muted", true);
    description_->setContentsMargins(22, 0, 22, 4);
    contentRoot->addWidget(description_);
    auto* scope = new QWidget;
    scope->setObjectName("settingsScope");
    auto* scopeRow = new QHBoxLayout(scope);
    scopeRow->setContentsMargins(18, 4, 18, 4);
    scopeRow->addWidget(new QLabel("当前连接"));
    connections_ = new QComboBox;
    connections_->setObjectName("settingsConnections");
    for (auto it = drafts_.cbegin(); it != drafts_.cend(); ++it)
        connections_->addItem(
            it->name + (it->kind == "serial" && !it->port.isEmpty() ? " · " + it->port : QString{}),
            it.key());
    if (drafts_.isEmpty())
        connections_->addItem("未选择连接", QString{});
    // Open on the connection of the active pane, as the window the user just used.
    const int activeConnection = connections_->findData(initialConnection);
    if (activeConnection >= 0)
        connections_->setCurrentIndex(activeConnection);
    scopeRow->addWidget(connections_, 1);
    scopeRow->addWidget(new QLabel("工作区"));
    workspaces_ = new QComboBox;
    workspaces_->addItem("默认工作区", "default");
    scopeRow->addWidget(workspaces_, 1);
    contentRoot->addWidget(scope);
    pages_ = new QStackedWidget;
    contentRoot->addWidget(pages_, 1);
    const QStringList names{"连接配置",   "接收显示",   "发送与命令", "日志文件",     "查找与标记",
                            "颜色与字体", "窗口与布局", "快捷键",     "工作区与备份", "帮助与诊断"};
    const QStringList icons{"sliders",  "power",  "send",    "log",    "search",
                            "settings", "panels", "sliders", "panels", "settings"};
    QVector<QVBoxLayout*> pageLayouts;
    for (int i = 0; i < names.size(); ++i) {
        if (i == 0 || i == 5 || i == 8) {
            auto* group = new QLabel(i == 0 ? "连接与数据" : i == 5 ? "界面与行为" : "数据管理");
            group->setObjectName("settingsGroup");
            nav->addWidget(group);
        }
        auto* categoryButton = new QPushButton(names[i]);
        categoryButton->setProperty("category", true);
        categoryButton->setCheckable(true);
        categoryButton->setObjectName("settingsCategory");
        categoryButton->setIcon(IconManager::icon(icons[i], theme.dark()));
        categoryButton->setIconSize(QSize(16, 16));
        nav->addWidget(categoryButton);
        categories_.push_back(categoryButton);
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* page = new QWidget;
        page->setObjectName("settingsPage");
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(22, 0, 22, 16);
        layout->setSpacing(8);
        scroll->setWidget(page);
        pages_->addWidget(scroll);
        pageLayouts.push_back(layout);
        connect(categoryButton, &QPushButton::clicked, this, [this, i] { pages_->setCurrentIndex(i); });
    }
    auto line = [&](QGridLayout* group, const QString& label, const QString& key) {
        auto* value = new QLineEdit;
        field(group, label, value, key);
        return value;
    };
    auto spin = [&](QGridLayout* group, const QString& label, const QString& key, int minimum, int maximum) {
        auto* value = new QSpinBox;
        value->setRange(minimum, maximum);
        field(group, label, value, key);
        return value;
    };
    auto check = [&](QGridLayout* group, const QString& text, const QString& key) {
        auto* value = new QCheckBox(text);
        field(group, {}, value, key);
        return value;
    };
    auto choice = [&](QGridLayout* group, const QString& label, const QString& key,
                      const QVector<QPair<QString, QVariant>>& items) {
        auto* value = new QComboBox;
        for (const auto& item : items)
            value->addItem(item.first, item.second);
        field(group, label, value, key);
        return value;
    };
    auto* group = section(pageLayouts[0], "基本连接");
    line(group, "连接名称", "name");
    auto* kind = choice(group, "连接类型", "kind", {{"串口", "serial"}, {"TCP", "tcp"}, {"测试", "test"}});
    auto* portHost = new QWidget;
    auto* portRow = new QHBoxLayout(portHost);
    portRow->setContentsMargins(0, 0, 0, 0);
    auto* port = new QComboBox;
    port->setEditable(true);
    fields_.insert("port", port);
    port->setObjectName("setting_port");
    portRow->addWidget(port, 1);
    auto* refresh = new QPushButton;
    refresh->setIcon(IconManager::icon("refresh", theme.dark()));
    refresh->setProperty("settingsActionIcon", "refresh");
    refresh->setToolTip("刷新串口列表");
    refresh->setFixedWidth(32);
    portRow->addWidget(refresh);
    field(group, "串口号", portHost);
    auto refreshPorts = [port] {
        auto previous = port->currentText();
        port->clear();
        for (const auto& info : QSerialPortInfo::availablePorts())
            port->addItem(info.portName());
        port->setCurrentText(previous);
    };
    refreshPorts();
    connect(refresh, &QPushButton::clicked, this, refreshPorts);
    line(group, "IP 地址", "host");
    spin(group, "TCP 端口", "tcpPort", 1, 65535);
    spin(group, "仿真每行间隔（ms）", "simulationIntervalMs", 1, 60000);
    group = section(pageLayouts[0], "串口参数");
    serialSection_ = group->parentWidget();
    auto* baud = choice(group, "波特率", "baud",
                        {{"9600", 9600},
                         {"19200", 19200},
                         {"38400", 38400},
                         {"57600", 57600},
                         {"115200", 115200},
                         {"230400", 230400},
                         {"460800", 460800},
                         {"921600", 921600}});
    baud->setEditable(true);
    baud->setProperty("numeric", true);
    choice(group, "停止位", "stopBits", {{"1", 1}, {"1.5", 3}, {"2", 2}});
    choice(group, "数据位", "dataBits", {{"5", 5}, {"6", 6}, {"7", 7}, {"8", 8}});
    choice(group, "流控", "flow", {{"无", 0}, {"RTS/CTS", 1}, {"XON/XOFF", 2}});
    choice(group, "校验", "parity", {{"无", 0}, {"偶", 2}, {"奇", 3}, {"Space", 4}, {"Mark", 5}});
    group = section(pageLayouts[0], "终端行为");
    check(group, "输入回显", "echo");
    check(group, "新行模式", "newline");
    check(group, "升级模式", "upgrade");
    spin(group, "时间标签间隔", "timestampInterval", 0, 86400);
    connections_->setContextMenuPolicy(Qt::ActionsContextMenu);
    connections_->setToolTip("右键可管理全部连接或新建测试连接");
    auto* manage = new QAction("管理全部连接…", connections_);
    auto* test = new QAction("新建测试连接", connections_);
    connections_->addActions({manage, test});
    connect(manage, &QAction::triggered, this, [this] { emit actionRequested("manage_connections"); });
    connect(test, &QAction::triggered, this, [this] { emit actionRequested("new_test"); });
    connect(kind, &QComboBox::currentIndexChanged, this, &SettingsDialog::updateConnectionType);
    group = section(pageLayouts[1], "显示格式");
    choice(group, "显示模式", "displayMode", {{"文本", 0}, {"HEX", 1}, {"混合", 2}});
    spin(group, "接收显示最大行数", "bufferLines", 100, 100000);
    group = section(pageLayouts[1], "显示与接收");
    check(group, "时间标签(界面)", "timestampDisplay");
    check(group, "时间标签(log)", "timestampLog");
    check(group, "暂停接收", "pauseReceive");
    check(group, "暂停显示", "pauseDisplay");
    check(group, "错误显示", "showErrors");
    check(group, "自动换行", "wrap");
    group = section(pageLayouts[1], "全局 · 历史滚动");
    scrollMode_ = new QComboBox;
    scrollMode_->addItems({"上划后保持当前视图", "始终跟随最新", "上划后持续刷新"});
    scrollMode_->setCurrentIndex(QSettings().value("view/scrollMode", 0).toInt());
    field(group, "滚动方式", scrollMode_);
    group = section(pageLayouts[2], "发送格式");
    check(group, "HEX", "sendHex");
    check(group, "回车 CR", "sendCR");
    check(group, "换行 LF", "sendLF");
    check(group, "发送当前行", "sendCurrentLine");
    check(group, "逐行前进", "advanceLine");
    group = section(pageLayouts[2], "自动发送");
    autoSend_ = new QCheckBox("自动发送");
    field(group, {}, autoSend_);
    sendInterval_ = new QDoubleSpinBox;
    sendInterval_->setDecimals(0);
    sendInterval_->setRange(10, 3600000);
    field(group, "发送间隔", sendInterval_, "sendIntervalMs");
    sendUnit_ = new QComboBox;
    sendUnit_->addItem("ms", 0);
    sendUnit_->addItem("s", 1);
    sendUnit_->setProperty("previousUnit", 0);
    field(group, "时间单位", sendUnit_, "sendIntervalUnit");
    connect(sendUnit_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const int oldIndex = sendUnit_->property("previousUnit").toInt();
        const double milliseconds = sendInterval_->value() * (oldIndex == 1 ? 1000.0 : 1.0);
        const QSignalBlocker blocker(sendInterval_);
        sendInterval_->setDecimals(index == 1 ? 3 : 0);
        sendInterval_->setRange(index == 1 ? 0.01 : 10.0, index == 1 ? 3600.0 : 3600000.0);
        sendInterval_->setValue(index == 1 ? milliseconds / 1000.0 : milliseconds);
        sendUnit_->setProperty("previousUnit", index);
    });
    action(pageLayouts[2], "显示／隐藏发送区", "send_panel");
    group = section(pageLayouts[2], "快捷命令组");
    commandGroups_ = new QComboBox;
    auto commandGroups = QJsonDocument::fromJson(QSettings().value("commands/groups").toByteArray()).object();
    QStringList groupNames = commandGroups.keys();
    if (!groupNames.contains("默认"))
        groupNames.prepend("默认");
    groupNames.removeAll("默认");
    groupNames.sort(Qt::CaseInsensitive);
    groupNames.prepend("默认");
    for (const auto& name : groupNames)
        commandGroups_->addItem(name, name);
    connect(commandGroups_, &QComboBox::currentIndexChanged, this, [this] {
        if (!loading_)
            refreshCommands();
    });
    field(group, "命令组", commandGroups_, "commandGroup");
    commands_ = new QTableWidget;
    commands_->setObjectName("settingsCommandTable");
    commands_->setColumnCount(3);
    commands_->setHorizontalHeaderLabels({"按钮名称", "发送内容", "操作"});
    commands_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    commands_->verticalHeader()->hide();
    commands_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    pageLayouts[2]->addWidget(commands_);
    action(pageLayouts[2], "新建快捷命令…", "new_command");
    action(pageLayouts[2], "导入命令组 TXT…", "import_commands");
    action(pageLayouts[2], "导出命令组 TXT…", "export_commands");
    action(pageLayouts[2], "新建命令组…", "command_group_new");
    action(pageLayouts[2], "复制命令组…", "command_group_copy");
    action(pageLayouts[2], "重命名命令组…", "command_group_rename");
    action(pageLayouts[2], "删除命令组…", "command_group_delete");
    group = section(pageLayouts[3], "自动存盘");
    check(group, "自动存盘", "autoLog");
    check(group, "纯文本存盘", "plainTextLog")
        ->setToolTip(
            "去除 ANSI 颜色及终端控制码，便于外部编辑器阅读；不影响界面彩色显示。切换后新建日志文件。");
    auto* logDirectory = line(group, "存盘目录", "logDirectory");
    auto* browse = new QPushButton("浏览…");
    field(group, {}, browse);
    connect(browse, &QPushButton::clicked, this, [this, logDirectory] {
        auto path = QFileDialog::getExistingDirectory(this, "存盘目录", logDirectory->text());
        if (!path.isEmpty())
            logDirectory->setText(path);
    });
    group = section(pageLayouts[3], "文件分卷");
    check(group, "按大小分卷", "rotate");
    spin(group, "分卷大小（KiB）", "rotateKiB", 1, 1048576);
    group = section(pageLayouts[3], "默认打开方式");
    auto* openOptions = new QWidget;
    auto* openLayout = new QVBoxLayout(openOptions);
    openLayout->setContentsMargins(0, 0, 0, 0);
    openLayout->setSpacing(8);
    auto* openNote = new QLabel("双击底部日志文件名时执行以下操作（适用于所有连接）");
    openNote->setProperty("muted", true);
    openNote->setWordWrap(true);
    openLayout->addWidget(openNote);
    logOpenAction_ = new QButtonGroup(this);
    const QStringList openLabels{"打开文件", "在 PortWave 中查看", "打开所在文件夹"};
    const QStringList openIds{"logOpenExternal", "logOpenPortWave", "logOpenFolder"};
    auto* programOptions = new QWidget;
    auto* programLayout = new QVBoxLayout(programOptions);
    programLayout->setContentsMargins(24, 0, 0, 0);
    auto* programModes = new QWidget;
    auto* programModeLayout = new QHBoxLayout(programModes);
    programModeLayout->setContentsMargins(0, 0, 0, 0);
    programModeLayout->setSpacing(0);
    programModeLayout->addWidget(new QLabel("打开程序"));
    programModeLayout->addSpacing(12);
    logOpenProgramMode_ = new QButtonGroup(this);
    for (int i = 0; i < 2; ++i) {
        auto* choice = new QPushButton(i == 0 ? "系统默认程序" : "指定程序");
        choice->setObjectName("logOpenProgramChoice");
        choice->setProperty("choiceIndex", i);
        choice->setCheckable(true);
        choice->setAutoDefault(false);
        logOpenProgramMode_->addButton(choice, i);
        programModeLayout->addWidget(choice);
    }
    programModeLayout->addStretch();
    programLayout->addWidget(programModes);
    auto* programPathRow = new QWidget;
    auto* programPathLayout = new QHBoxLayout(programPathRow);
    programPathLayout->setContentsMargins(0, 0, 0, 0);
    programPathLayout->addWidget(new QLabel("程序路径"));
    logOpenProgram_ = new QLineEdit;
    logOpenProgram_->setObjectName("logOpenProgramPath");
    logOpenProgram_->setPlaceholderText("选择用于打开日志的程序（.exe）");
    programPathLayout->addWidget(logOpenProgram_, 1);
    auto* browseProgram = new QPushButton("浏览…");
    browseProgram->setObjectName("browseLogOpenProgram");
    programPathLayout->addWidget(browseProgram);
    programLayout->addWidget(programPathRow);
    auto* programNote = new QLabel("未指定程序时，使用系统默认程序打开。");
    programNote->setProperty("muted", true);
    programNote->setWordWrap(true);
    programLayout->addWidget(programNote);
    for (int i = 0; i < openLabels.size(); ++i) {
        auto* choice = new QRadioButton(openLabels[i]);
        choice->setObjectName(openIds[i]);
        logOpenAction_->addButton(choice, i);
        openLayout->addWidget(choice);
        if (i == 0)
            openLayout->addWidget(programOptions);
    }
    group->addWidget(openOptions, 0, 0, 1, 2);
    const auto logOpen = repository_.loadLogOpenPreferences();
    logOpenAction_->button(static_cast<int>(logOpen.action))->setChecked(true);
    logOpenProgramMode_->button(logOpen.customProgram ? 1 : 0)->setChecked(true);
    logOpenProgram_->setText(logOpen.program);
    const auto updateLogOpenControls = [this, programOptions, programPathRow] {
        programOptions->setEnabled(logOpenAction_->checkedId() == 0);
        programPathRow->setEnabled(logOpenProgramMode_->checkedId() == 1);
    };
    connect(logOpenAction_, &QButtonGroup::idToggled, this, [this, updateLogOpenControls](int, bool) {
        updateLogOpenControls();
        dirty();
    });
    connect(logOpenProgramMode_, &QButtonGroup::idToggled, this, [this, updateLogOpenControls](int, bool) {
        updateLogOpenControls();
        dirty();
    });
    connect(browseProgram, &QPushButton::clicked, this, [this] {
        const auto program = QFileDialog::getOpenFileName(this, "选择日志打开程序", logOpenProgram_->text(),
                                                          "可执行程序 (*.exe)");
        if (!program.isEmpty())
            logOpenProgram_->setText(QDir::toNativeSeparators(program));
    });
    updateLogOpenControls();
    group = section(pageLayouts[3], "日志工作区 · 浏览");
    logFont_ = new QFontComboBox;
    logFont_->setObjectName("logReadingFont");
    logFontSize_ = new QSpinBox;
    logFontSize_->setObjectName("logReadingSize");
    logFontSize_->setRange(6, 48);
    logFontSize_->setSuffix(" pt");
    field(group, "日志字体", logFont_);
    field(group, "日志字号", logFontSize_);

    logColors_ = new QCheckBox("显示日志原色（关闭为黑白）");
    logNumbers_ = new QCheckBox("显示行号");
    logWrap_ = new QCheckBox("自动换行");
    logColors_->setChecked(QSettings().value("logs/view/colors", true).toBool());
    logNumbers_->setChecked(QSettings().value("logs/view/numbers", true).toBool());
    logWrap_->setChecked(QSettings().value("logs/view/wrap", true).toBool());
    for (auto* value : {logColors_, logNumbers_, logWrap_})
        field(group, {}, value);
    group = section(pageLayouts[4], "串口缓存查找 · 设置记忆");
    for (const auto& item : QVector<QPair<QString, QString>>{{"backward", "反向查找"},
                                                             {"whole", "全词匹配"},
                                                             {"case", "匹配大小写"},
                                                             {"wrap", "循环查找"},
                                                             {"keep", "保留历史检索结果"},
                                                             {"live", "实时更新"}}) {
        auto* value = new QCheckBox(item.second);
        findOptions_.insert(item.first, value);
        field(group, {}, value);
    }
    action(pageLayouts[4], "打开串口缓存查找选项…", "cache_find");
    group = section(pageLayouts[4], "当前日志工作区 · 查找与标记");
    findMode_ = new QComboBox;
    findMode_->addItems({"普通", "扩展", "正则表达式"});
    field(group, "查找模式", findMode_);
    action(pageLayouts[4], "打开日志查找／标记选项…", "log_find");
    action(pageLayouts[4], "选择近期日志、文件或文件夹检索…", "log_find_files");
    group = section(pageLayouts[5], "外观组合 · 分别配置，应用后生效");
    controlStyle_ = new QComboBox;
    controlStyle_->setObjectName("appearanceControls");
    controlStyle_->addItem("标准", "standard");
    controlStyle_->addItem("紧凑", "compact");
    layoutPreset_ = new QComboBox;
    layoutPreset_->setObjectName("appearanceLayout");
    layoutPreset_->addItem("保持当前布局", "keep");
    layoutPreset_->addItem("单窗口标签", "single");
    layoutPreset_->addItem("左右并排", "columns");
    layoutPreset_->addItem("上下并排", "rows");
    const auto profile = theme.profile();
    controlStyle_->setCurrentIndex(controlStyle_->findData(profile.controls.id));
    field(group, "当前主题 · 控件风格", controlStyle_);
    field(group, "本次应用布局", layoutPreset_);
    group = section(pageLayouts[5], "全局 · 窗口和终端");
    themeChoice_ = new QComboBox;
    themeChoice_->setObjectName("appearanceTheme");
    themeChoice_->addItem("明亮", "light");
    themeChoice_->addItem("炫彩", "chromatic");
    themeChoice_->setCurrentIndex(themeChoice_->findData(theme.themeId()));
    field(group, "界面主题", themeChoice_);
    auto* resetTheme = new QPushButton("重置当前主题");
    resetTheme->setObjectName("resetCurrentTheme");
    field(group, "同时恢复日志检索背景与字体，应用或确定后保存。", resetTheme);
    background_ = new ColorButton;
    foreground_ = new ColorButton;
    active_ = new ColorButton;
    reading_ = new ColorButton;
    font_ = new QFontComboBox;
    fontSize_ = new QSpinBox;
    fontSize_->setObjectName("terminalReadingSize");
    fontSize_->setRange(6, 48);
    smoothing_ = new QCheckBox("字体平滑");
    field(group, "终端背景", background_);
    field(group, "终端文字", foreground_);
    field(group, "激活窗口颜色", active_);
    field(group, "终端字体", font_);
    field(group, "字号", fontSize_);
    field(group, {}, smoothing_);
    group = section(pageLayouts[5], "日志工作区 · 正文阅读");
    field(group, "阅读背景（自定义）", reading_);
    preview_ = new QLabel("[22:49:40.103] INFO Serial connection opened\n[22:49:41.601] INFO Checksum "
                          "verified\n[22:49:44.599] WARN Response timeout");
    preview_->setObjectName("settingsFontPreview");
    preview_->setMargin(16);
    pageLayouts[5]->addWidget(preview_);
    for (int mode = 0; mode < themeChoice_->count(); ++mode) {
        const auto token = DesignTokens::forTheme(themeChoice_->itemData(mode).toString());
        const QString prefix = "appearance/" + themeChoice_->itemData(mode).toString() + "/";
        QSettings settings;
        appearanceDrafts_[mode] = {
            {"controls", AppearanceProfile::load(themeChoice_->itemData(mode).toString()).controls.id},
            {"background", settings.value(prefix + "background", token.surface.name()).toString()},
            {"foreground", settings.value(prefix + "foreground", token.text.name()).toString()},
            {"active", settings.value(prefix + "active", token.accent.name()).toString()},
            {"font", settings.value(prefix + "font", "Consolas").toString()},
            {"size", settings.value(prefix + "size", 12).toInt()},
            {"smooth", settings.value(prefix + "smooth", true).toBool()},
            {"logFont", settings
                            .value("logs/appearance/" + themeChoice_->itemData(mode).toString() + "/font",
                                   settings.value("logs/appearance/font", "Consolas"))
                            .toString()},
            {"logSize", settings
                            .value("logs/appearance/" + themeChoice_->itemData(mode).toString() + "/size",
                                   settings.value("logs/appearance/size", 12))
                            .toInt()},
            {"reading",
             settings
                 .value("logs/appearance/" + themeChoice_->itemData(mode).toString() + "/background",
                        token.surface.name())
                 .toString()}};
    }
    appearanceMode_ = themeChoice_->currentIndex();
    loadAppearance(appearanceMode_);
    auto preview = [this] {
        QFont font(font_->currentFont().family(), fontSize_->value());
        preview_->setFont(font);
        preview_->setAutoFillBackground(true);
        auto palette = preview_->palette();
        palette.setColor(QPalette::Window, background_->color());
        palette.setColor(QPalette::WindowText, foreground_->color());
        preview_->setPalette(palette);
    };
    for (auto* value : {background_, foreground_, active_, reading_})
        value->changed = [this, preview] {
            preview();
            dirty();
        };
    connect(font_, &QFontComboBox::currentFontChanged, this, [this, preview] {
        preview();
        dirty();
    });
    connect(fontSize_, &QSpinBox::valueChanged, this, [this, preview] {
        preview();
        dirty();
    });
    connect(themeChoice_, &QComboBox::currentIndexChanged, this, [this, preview](int index) {
        captureAppearance(appearanceMode_);
        appearanceMode_ = index;
        loadAppearance(index);
        preview();
        dirty();
    });
    connect(resetTheme, &QPushButton::clicked, this, [this, preview] {
        const auto token = DesignTokens::forTheme(themeChoice_->currentData().toString());
        appearanceDrafts_[themeChoice_->currentIndex()] = {{"background", token.surface.name()},
                                                           {"foreground", token.text.name()},
                                                           {"active", token.accent.name()},
                                                           {"font", "Consolas"},
                                                           {"size", 12},
                                                           {"smooth", true},
                                                           {"reading", token.surface.name()},
                                                           {"logFont", "Consolas"},
                                                           {"logSize", 12}};
        loadAppearance(themeChoice_->currentIndex());
        reading_->setColor(token.surface);
        preview();
        dirty();
    });
    preview();
    section(pageLayouts[6], "串口窗口");
    action(pageLayouts[6], "全部串口吸合到主窗口", "dock_all");
    action(pageLayouts[6], "恢复上次浮动布局", "restore_float");
    action(pageLayouts[6], "恢复上次主窗口布局", "restore_docked");
    section(pageLayouts[6], "自定义布局");
    action(pageLayouts[6], "保存当前布局…", "save_layout");
    auto* layoutList = new QComboBox;
    layoutList->setObjectName("savedLayoutList");
    QSettings layoutSettings;
    layoutSettings.beginGroup("layouts/saved");
    for (const auto& key : layoutSettings.childKeys())
        layoutList->addItem(key);
    pageLayouts[6]->addWidget(layoutList);
    auto* restore = new QPushButton("恢复所选布局");
    pageLayouts[6]->addWidget(restore);
    connect(restore, &QPushButton::clicked, this,
            [this, layoutList] { emit actionRequested("restore_layout:" + layoutList->currentText()); });
    section(pageLayouts[6], "日志窗口");
    action(pageLayouts[6], "恢复单窗口", "restore_log_window");
    group = section(pageLayouts[7], "快捷键");
    const auto hint = new QLabel("所有快捷键都可修改；留空表示不绑定。默认值与之前约定一致。");
    hint->setProperty("muted", true);
    hint->setWordWrap(true);
    for (const auto& definition : shortcutDefinitions()) {
        auto* key = new QKeySequenceEdit;
        key->setKeySequence(configuredShortcut(QString::fromLatin1(definition.id)));
        keys_.insert(QString::fromLatin1(definition.id), key);
        field(group, QString::fromUtf8(definition.label), key);
    }
    group->addWidget(hint, group->rowCount(), 0, 1, 2);
    section(pageLayouts[8], "日志工作区");
    action(pageLayouts[8], "保存 / 更新工作区与布局", "save_workspace");
    action(pageLayouts[8], "新建工作区…", "new_workspace");
    action(pageLayouts[8], "另存为新工作区…", "copy_workspace");
    action(pageLayouts[8], "重命名当前工作区…", "rename_workspace");
    section(pageLayouts[8], "整个工作区 · 导入与导出");
    action(pageLayouts[8], "导出整个工作区…", "export_workspace");
    action(pageLayouts[8], "导入整个工作区…", "import_workspace");
    auto* note =
        new QLabel("备份保存连接配置、布局、发送框及工作区搜索状态；日志源文件和已打开标签不会打包。");
    note->setWordWrap(true);
    note->setProperty("muted", true);
    pageLayouts[8]->addWidget(note);
    section(pageLayouts[8], "备份");
    action(pageLayouts[8], "备份当前工作区", "backup_workspace");
    action(pageLayouts[8], "打开备份文件夹", "open_backup_directory");
    section(pageLayouts[9], "使用帮助");
    action(pageLayouts[9], "打开使用说明", "help");
    group = section(pageLayouts[9], "版本信息");
    field(group, "版本", new QLabel("PortWave C++ 0.2.0 / Qt " + QString(qVersion())));
    field(group, "编译", new QLabel(QString::fromLatin1(__DATE__ " " __TIME__)));
    action(pageLayouts[9], "版本更新记录", "release_history");
    section(pageLayouts[9], "诊断");
    auto* diagnostics = new QPushButton("打开诊断日志目录（暂未开放）");
    diagnostics->setEnabled(false);
    pageLayouts[9]->addWidget(diagnostics);
    for (auto* layout : pageLayouts)
        layout->addStretch();
    auto* footer = new QWidget;
    footer->setObjectName("settingsFooter");
    auto* bottom = new QHBoxLayout(footer);
    bottom->setContentsMargins(20, 12, 20, 12);
    auto* reset = new QPushButton("恢复本页默认");
    bottom->addWidget(reset);
    message_ = new QLabel("未修改");
    message_->setProperty("muted", true);
    bottom->addWidget(message_, 1);
    auto* help = new QPushButton("帮助");
    bottom->addWidget(help);
    cancelButton_ = new QPushButton("取消");
    applyButton_ = new QPushButton("应用");
    okButton_ = new QPushButton("确定");
    okButton_->setProperty("primary", true);
    okButton_->setDefault(true);
    bottom->addWidget(cancelButton_);
    bottom->addWidget(applyButton_);
    bottom->addWidget(okButton_);
    root->addWidget(footer);
    connect(reset, &QPushButton::clicked, this, &SettingsDialog::resetPage);
    connect(help, &QPushButton::clicked, this, [this] { emit actionRequested("help"); });
    connect(okButton_, &QPushButton::clicked, this, &SettingsDialog::accept);
    connect(cancelButton_, &QPushButton::clicked, this, &QDialog::reject);
    connect(applyButton_, &QPushButton::clicked, this, [this] { apply(); });
    connect(pages_, &QStackedWidget::currentChanged, this, [this, names](int index) {
        heading_->setText(names[index]);
        description_->setText(index == 0  ? "配置当前连接的通信参数与终端行为。"
                              : index < 5 ? "选择连接或日志工作区后调整相关设置；应用或确定后保存。"
                              : index < 8 ? "修改界面和操作偏好；切换分类保留尚未应用的修改。"
                                          : "管理工作区、备份与使用帮助。");
        scopeHint_->setText(index < 5 ? "作用范围：当前连接 / 工作区" : "作用范围：全局");
        for (int i = 0; i < categories_.size(); ++i)
            categories_[i]->setChecked(i == index);
    });
    connect(settingsSearch_, &QLineEdit::textChanged, this, [this](const QString& query) {
        for (auto* button : categories_)
            button->setVisible(button->text().contains(query, Qt::CaseInsensitive));
    });
    for (auto* control : findChildren<QLineEdit*>())
        if (control != settingsSearch_)
            connect(control, &QLineEdit::textChanged, this, [this] { dirty(); });
    for (auto* control : findChildren<QCheckBox*>())
        connect(control, &QCheckBox::toggled, this, [this] { dirty(); });
    for (auto* control : findChildren<QComboBox*>())
        if (control != connections_)
            connect(control, &QComboBox::currentIndexChanged, this, [this] { dirty(); });
    for (auto* control : findChildren<QSpinBox*>())
        connect(control, &QSpinBox::valueChanged, this, [this] { dirty(); });
    for (auto* control : keys_)
        connect(control, &QKeySequenceEdit::keySequenceChanged, this, [this] { dirty(); });
    connect(&theme_, &ThemeManager::changed, this, [this, icons](bool dark) {
        for (int i = 0; i < categories_.size(); ++i)
            categories_[i]->setIcon(IconManager::icon(icons[i], dark));
        for (auto* label : findChildren<QLabel*>())
            if (label->property("settingsIcon").isValid())
                label->setPixmap(
                    IconManager::icon(label->property("settingsIcon").toString(), dark).pixmap(18, 18));
        for (auto* button : findChildren<QPushButton*>())
            if (button->property("settingsActionIcon").isValid())
                button->setIcon(IconManager::icon(button->property("settingsActionIcon").toString(), dark));
    });
    connectionIndex_ = connections_->currentIndex();
    loadConnection();
    connect(connections_, &QComboBox::currentIndexChanged, this, &SettingsDialog::changeConnection);
    pages_->setCurrentIndex(qMax(0, names.indexOf(category)));
    emit pages_->currentChanged(pages_->currentIndex());
    captureAppearance(appearanceMode_);
    appearanceBaseline_ = appearanceDrafts_;
    for (auto it = keys_.cbegin(); it != keys_.cend(); ++it)
        keysBaseline_[it.key()] = it.value()->keySequence().toString(QKeySequence::PortableText);
    loading_ = false;
    dirty_ = false;
    applyButton_->setEnabled(false);
    restoreGeometry(QSettings().value("dialogs/settings/geometry").toByteArray());
}
void SettingsDialog::dirty() {
    if (loading_)
        return;
    dirty_ = true;
    message_->setText("● 有未保存的更改");
    applyButton_->setEnabled(true);
}
void SettingsDialog::storeConnection() {
    const auto id = connections_->itemData(connectionIndex_).toString();
    if (!drafts_.contains(id))
        return;
    auto json = drafts_[id].toJson();
    for (auto it = fields_.cbegin(); it != fields_.cend(); ++it) {
        auto* control = it.value();
        QVariant value;
        if (auto* edit = qobject_cast<QLineEdit*>(control))
            value = edit->text();
        else if (auto* check = qobject_cast<QCheckBox*>(control))
            value = check->isChecked();
        else if (auto* doubleSpin = qobject_cast<QDoubleSpinBox*>(control))
            value = doubleSpin->value();
        else if (auto* intSpin = qobject_cast<QSpinBox*>(control))
            value = intSpin->value();
        else if (auto* combo = qobject_cast<QComboBox*>(control)) {
            value = combo->currentData().isValid() && !combo->isEditable() ? combo->currentData()
                                                                           : QVariant(combo->currentText());
            if (combo->property("numeric").toBool())
                value = value.toInt();
        }
        if (it.key() == "sendIntervalMs" && sendUnit_->currentData().toInt() == 1)
            value = qRound(value.toDouble() * 1000.0);
        json[it.key()] = QJsonValue::fromVariant(value);
    }
    drafts_[id] = ConnectionConfig::fromJson(json);
}
void SettingsDialog::loadConnection() {
    loading_ = true;
    const auto id = connections_->currentData().toString();
    auto c = drafts_.value(id);
    auto json = c.toJson();
    for (auto it = fields_.cbegin(); it != fields_.cend(); ++it) {
        auto* control = it.value();
        control->setEnabled(!id.isEmpty());
        const auto value = json[it.key()].toVariant();
        if (auto* edit = qobject_cast<QLineEdit*>(control))
            edit->setText(value.toString());
        else if (auto* check = qobject_cast<QCheckBox*>(control))
            check->setChecked(value.toBool());
        else if (auto* doubleSpin = qobject_cast<QDoubleSpinBox*>(control))
            doubleSpin->setValue(value.toDouble());
        else if (auto* intSpin = qobject_cast<QSpinBox*>(control))
            intSpin->setValue(value.toInt());
        else if (auto* combo = qobject_cast<QComboBox*>(control)) {
            if (combo->isEditable())
                combo->setCurrentText(value.toString());
            else
                combo->setCurrentIndex(combo->findData(value));
        }
    }
    const auto config = drafts_.value(id);
    autoSend_->setChecked(config.automaticSend);
    for (const auto& pane : panes_)
        if (pane && pane->session()->config().id == id)
            autoSend_->setChecked(pane->automaticSending());
    sendUnit_->setProperty("previousUnit", config.sendIntervalUnit);
    sendUnit_->setCurrentIndex(config.sendIntervalUnit);
    sendUnit_->setProperty("previousUnit", config.sendIntervalUnit);
    {
        const QSignalBlocker blocker(sendInterval_);
        const bool seconds = config.sendIntervalUnit == 1;
        sendInterval_->setDecimals(seconds ? 3 : 0);
        sendInterval_->setRange(seconds ? 0.01 : 10.0, seconds ? 3600.0 : 3600000.0);
        sendInterval_->setValue(seconds ? static_cast<double>(config.sendIntervalMs) / 1000.0
                                        : config.sendIntervalMs);
    }
    QSettings settings;
    for (auto it = findOptions_.cbegin(); it != findOptions_.cend(); ++it)
        it.value()->setChecked(
            settings.value("search/cache/" + id + "/" + it.key(), it.key() == "wrap").toBool());
    findMode_->setCurrentIndex(settings.value("search/log/default/mode", 0).toInt());
    refreshCommands();
    updateConnectionType();
    loading_ = false;
}
void SettingsDialog::changeConnection(int index) {
    if (index == connectionIndex_)
        return;
    if (dirty_) {
        auto answer = QMessageBox::question(this, "切换设置对象", "应用当前修改后再切换？",
                                            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
        if (answer == QMessageBox::Cancel || (answer == QMessageBox::Yes && !apply())) {
            QSignalBlocker blocker(connections_);
            connections_->setCurrentIndex(connectionIndex_);
            return;
        }
    }
    connectionIndex_ = index;
    loadConnection();
    dirty_ = false;
    message_->setText("未修改");
    applyButton_->setEnabled(false);
}
void SettingsDialog::captureAppearance(int mode) {
    appearanceDrafts_[mode] = {{"controls", controlStyle_->currentData().toString()},
                               {"background", background_->color().name()},
                               {"foreground", foreground_->color().name()},
                               {"active", active_->color().name()},
                               {"font", font_->currentFont().family()},
                               {"size", fontSize_->value()},
                               {"smooth", smoothing_->isChecked()},
                               {"reading", reading_->color().name()},
                               {"logFont", logFont_->currentFont().family()},
                               {"logSize", logFontSize_->value()}};
}
void SettingsDialog::loadAppearance(int mode) {
    bool previous = loading_;
    loading_ = true;
    auto draft = appearanceDrafts_[mode];
    logFont_->setCurrentFont(QFont(draft["logFont"].toString("Consolas")));
    logFontSize_->setValue(draft["logSize"].toInt(12));
    controlStyle_->setCurrentIndex(controlStyle_->findData(draft["controls"].toString("standard")));
    background_->setColor(QColor(draft["background"].toString()));
    foreground_->setColor(QColor(draft["foreground"].toString()));
    active_->setColor(QColor(draft["active"].toString()));
    font_->setCurrentFont(QFont(draft["font"].toString()));
    fontSize_->setValue(draft["size"].toInt(12));
    smoothing_->setChecked(draft["smooth"].toBool(true));
    reading_->setColor(QColor(draft["reading"].toString()));
    loading_ = previous;
}
void SettingsDialog::refreshCommands() {
    commands_->setRowCount(0);
    const auto id = connections_->currentData().toString();
    QJsonObject groups = QJsonDocument::fromJson(QSettings().value("commands/groups").toByteArray()).object();
    const auto definitions = groups.value(commandGroups_->currentText()).toArray();
    if (!id.isEmpty()) {
        commands_->setRowCount(static_cast<int>(definitions.size()));
        for (int row = 0; row < definitions.size(); ++row) {
            auto item = definitions[row].toObject();
            commands_->setItem(row, 0, new QTableWidgetItem(item["name"].toString()));
            commands_->setItem(row, 1, new QTableWidgetItem(item["text"].toString()));
            auto* edit = new QPushButton("编辑");
            commands_->setCellWidget(row, 2, edit);
            connect(edit, &QPushButton::clicked, this, [this, row] {
                const auto selected = commandGroups_->currentText();
                for (const auto& pane : panes_)
                    if (pane && pane->session()->config().id == connections_->currentData().toString() &&
                        pane->commandGroupName() == selected) {
                        pane->manageCommand(row);
                        refreshCommands();
                        return;
                    }
                message_->setText("请先在当前连接窗格中切换到所选命令组，再编辑命令");
            });
            commands_->setRowHeight(row, 36);
        }
        commands_->setFixedHeight(qBound(100, 35 + 36 * static_cast<int>(definitions.size()), 240));
    }
}
bool SettingsDialog::apply() {
    QElapsedTimer applyTime;
    applyTime.start();
    bool shortcutChanges = false, findChanges = false;
    LogOpenPreferences logOpen;
    logOpen.action = static_cast<LogOpenAction>(logOpenAction_->checkedId());
    logOpen.customProgram = logOpenProgramMode_->checkedId() == 1;
    logOpen.program = logOpenProgram_->text().trimmed();
    if (logOpen.action == LogOpenAction::ExternalFile && logOpen.customProgram &&
        !logOpen.program.isEmpty()) {
        const QFileInfo program(logOpen.program);
        if (!program.isAbsolute() || !program.isFile() || !program.isExecutable()) {
            message_->setText("请选择有效的程序完整路径；留空则使用系统默认程序。");
            pages_->setCurrentIndex(3);
            return false;
        }
    }
    storeConnection();
    const auto editedConnectionId = connections_->itemData(connectionIndex_).toString();
    if (drafts_.contains(editedConnectionId))
        drafts_[editedConnectionId].automaticSend = autoSend_->isChecked();
    QSet<QString> sequences;
    // Editing keys and window-manager combinations stay reserved. Every application
    // action, including the previously fixed Ctrl+F / Ctrl+Shift+F / F3 / F5 /
    // Ctrl+O / Ctrl+W and the send-draft shortcut, is configurable.
    const QSet<QString> reserved{
        "Ctrl+C",    "Ctrl+V",   "Ctrl+Shift+C", "Ctrl+Shift+V",   "Ctrl+Insert",  "Shift+Insert",
        "Ctrl+A",    "Ctrl+X",   "Ctrl+Z",       "Ctrl+Y",         "Shift+Return", "Shift+Enter",
        "Ctrl+Home", "Ctrl+End", "Ctrl+Tab",     "Ctrl+Shift+Tab", "Alt+F4"};
    for (auto it = keys_.cbegin(); it != keys_.cend(); ++it) {
        auto sequence = it.value()->keySequence();
        auto text = sequence.toString(QKeySequence::PortableText);
        if (text.isEmpty())
            continue;
        if (it.key() != "log_search" && conflictsWithImeToggle(sequence)) {
            message_->setText("Ctrl+Shift+F 与输入法简繁切换冲突，请使用其它快捷键。");
            pages_->setCurrentIndex(7);
            return false;
        }
        if (sequence.count() != 1 || sequences.contains(text) || reserved.contains(text)) {
            message_->setText("快捷键重复或与保留快捷键冲突：" + text);
            pages_->setCurrentIndex(7);
            return false;
        }
        sequences.insert(text);
    }
    for (const auto& config : drafts_)
        if (config.name.trimmed().isEmpty() || config.baud < 50 || config.baud > 4000000 ||
            (config.kind == "serial" && config.port.isEmpty()) ||
            (config.kind == "tcp" && config.host.isEmpty())) {
            message_->setText("请填写有效的连接名称、端口 / 地址和波特率");
            pages_->setCurrentIndex(0);
            return false;
        }
    auto saved = repository_.loadConnections();
    QJsonArray previousConnections;
    for (const auto& config : saved)
        previousConnections.append(config.toJson());
    for (const auto& config : drafts_) {
        bool found = false;
        for (auto& item : saved)
            if (item.id == config.id) {
                item = config;
                found = true;
                break;
            }
        if (!found && config.kind != "test")
            saved.push_back(config);
    }
    QString error;
    QJsonArray nextConnections;
    for (const auto& config : saved)
        nextConnections.append(config.toJson());
    if (nextConnections != previousConnections && !repository_.saveConnections(saved, error)) {
        message_->setText("保存失败：" + error);
        return false;
    }
    const auto previousLogOpen = repository_.loadLogOpenPreferences();
    if ((previousLogOpen.action != logOpen.action || previousLogOpen.customProgram != logOpen.customProgram ||
         previousLogOpen.program != logOpen.program) &&
        !repository_.saveLogOpenPreferences(logOpen, error)) {
        message_->setText("保存失败：" + error);
        return false;
    }
    for (const auto& pane : panes_)
        if (pane && drafts_.contains(pane->session()->config().id)) {
            const auto id = pane->session()->config().id;
            if (pane->currentConfiguration().toJson() != drafts_[id].toJson())
                pane->applyConfiguration(drafts_[id]);
        }
    QSettings settings;
    const bool scrollChanged = settings.value("view/scrollMode", 0).toInt() != scrollMode_->currentIndex();
    settings.setValue("view/scrollMode", scrollMode_->currentIndex());
    for (auto it = keys_.cbegin(); it != keys_.cend(); ++it) {
        const auto next = it.value()->keySequence().toString(QKeySequence::PortableText);
        shortcutChanges |= next != keysBaseline_.value(it.key());
        if (settings.value("shortcuts/" + it.key()).toString() != next)
            settings.setValue("shortcuts/" + it.key(), next);
    }
    for (auto it = findOptions_.cbegin(); it != findOptions_.cend(); ++it) {
        const auto key = "search/cache/" + connections_->currentData().toString() + "/" + it.key();
        const bool next = it.value()->isChecked();
        findChanges |= settings.value(key, next).toBool() != next;
        if (!settings.contains(key) || settings.value(key).toBool() != next)
            settings.setValue(key, next);
    }
    settings.setValue("search/log/default/mode", findMode_->currentIndex());
    captureAppearance(appearanceMode_);
    const bool appearanceChanged =
        themeChoice_->currentData().toString() != theme_.themeId() ||
        appearanceDrafts_.value(appearanceMode_) != appearanceBaseline_.value(appearanceMode_);
    const bool logChanged = settings.value("logs/view/wrap", true).toBool() != logWrap_->isChecked() ||
                            settings.value("logs/view/numbers", true).toBool() != logNumbers_->isChecked() ||
                            settings.value("logs/view/colors", true).toBool() != logColors_->isChecked();
    if (logChanged) {
        settings.setValue("logs/view/wrap", logWrap_->isChecked());
        settings.setValue("logs/view/numbers", logNumbers_->isChecked());
        settings.setValue("logs/view/colors", logColors_->isChecked());
    }
    for (auto it = appearanceDrafts_.cbegin(); it != appearanceDrafts_.cend(); ++it) {
        const QString prefix = "appearance/" + themeChoice_->itemData(it.key()).toString() + "/";
        for (auto field = it->constBegin(); field != it->constEnd(); ++field)
            if (field.key() == "logFont" || field.key() == "logSize")
                settings.setValue("logs/appearance/" + themeChoice_->itemData(it.key()).toString() +
                                      (field.key() == "logFont" ? "/font" : "/size"),
                                  field.value().toVariant());
            else if (field.key() == "reading")
                settings.setValue("logs/appearance/" + themeChoice_->itemData(it.key()).toString() +
                                      "/background",
                                  field.value().toVariant());
            else
                settings.setValue(prefix + field.key(), field.value().toVariant());
    }
    settings.setValue("dialogs/settings/geometry", saveGeometry());
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        message_->setText("设置写入失败");
        return false;
    }
    if (logs_ && logChanged)
        logs_->setReadingOptions(logWrap_->isChecked(), logNumbers_->isChecked(), logColors_->isChecked());
    if (scrollChanged)
        for (const auto& pane : panes_)
            if (pane)
                pane->terminal()->setScrollMode(scrollMode_->currentIndex());
    if (appearanceChanged) {
        auto before = appearanceBaseline_.value(appearanceMode_);
        auto after = appearanceDrafts_.value(appearanceMode_);
        for (const auto& key : {"logFont", "logSize", "reading"}) {
            before.remove(key);
            after.remove(key);
        }
        const bool onlyLog = themeChoice_->currentData().toString() == theme_.themeId() && before == after;
        if (onlyLog && logs_)
            logs_->setReadingAppearance(QFont(logFont_->currentFont().family(), logFontSize_->value()),
                                        reading_->color(), foreground_->color());
        else
            theme_.applyTheme(themeChoice_->currentData().toString());
    }
    const auto layout = layoutPreset_->currentData().toString();
    if (layout != "keep") {
        emit actionRequested("appearance_layout:" + layout);
        const QSignalBlocker blocker(layoutPreset_);
        layoutPreset_->setCurrentIndex(0);
    }
    if (shortcutChanges)
        emit shortcutsChanged();
    if (findChanges)
        emit findPreferencesChanged();
    appearanceBaseline_ = appearanceDrafts_;
    for (auto it = keys_.cbegin(); it != keys_.cend(); ++it)
        keysBaseline_[it.key()] = it.value()->keySequence().toString(QKeySequence::PortableText);
    setProperty("lastApplyMs", applyTime.elapsed());
    dirty_ = false;
    applyButton_->setEnabled(false);
    message_->setText("已应用；串口连接参数在重连后生效");
    return true;
}
void SettingsDialog::accept() {
    if (apply())
        QDialog::accept();
}
void SettingsDialog::resetPage() {
    const auto page = pages_->currentIndex();
    if (page == 5) {
        auto* button = findChild<QPushButton*>("resetCurrentTheme");
        if (button)
            button->click();
        return;
    }
    const auto defaults = ConnectionConfig{}.toJson();
    for (auto it = fields_.cbegin(); it != fields_.cend(); ++it) {
        auto* widget = it.value();
        int owner = it.key() == "name" || it.key() == "kind" || it.key() == "port" || it.key() == "baud" ||
                            it.key() == "host" || it.key() == "tcpPort" || it.key() == "dataBits" ||
                            it.key() == "stopBits" || it.key() == "parity" || it.key() == "flow" ||
                            it.key() == "echo" || it.key() == "upgrade" || it.key() == "newline" ||
                            it.key() == "timestampInterval"
                        ? 0
                    : it.key().startsWith("send") || it.key() == "advanceLine" ? 2
                    : it.key() == "autoLog" || it.key() == "plainTextLog" || it.key() == "logDirectory" ||
                            it.key() == "rotate" || it.key() == "rotateKiB"
                        ? 3
                        : 1;
        if (owner != page || it.key() == "name" || it.key() == "port" || it.key() == "host")
            continue;
        auto value = defaults[it.key()].toVariant();
        if (auto* check = qobject_cast<QCheckBox*>(widget))
            check->setChecked(value.toBool());
        else if (auto* spin = qobject_cast<QSpinBox*>(widget))
            spin->setValue(value.toInt());
        else if (auto* combo = qobject_cast<QComboBox*>(widget)) {
            if (combo->isEditable())
                combo->setCurrentText(value.toString());
            else
                combo->setCurrentIndex(combo->findData(value));
        }
    }
    if (page == 1)
        scrollMode_->setCurrentIndex(0);
    if (page == 2) {
        autoSend_->setChecked(false);
        sendUnit_->setCurrentIndex(0);
        {
            const QSignalBlocker blocker(sendInterval_);
            sendInterval_->setDecimals(0);
            sendInterval_->setRange(10, 3600000);
            sendInterval_->setValue(ConnectionConfig{}.sendIntervalMs);
        }
        const auto id = connections_->currentData().toString();
        if (drafts_.contains(id))
            drafts_[id].sendIntervalUnit = 0;
    }
    if (page == 3) {
        logOpenAction_->button(static_cast<int>(LogOpenAction::PortWave))->setChecked(true);
        logOpenProgramMode_->button(0)->setChecked(true);
        logOpenProgram_->clear();
        logWrap_->setChecked(true);
        logNumbers_->setChecked(true);
        logColors_->setChecked(true);
    }
    if (page == 4) {
        for (auto it = findOptions_.cbegin(); it != findOptions_.cend(); ++it)
            it.value()->setChecked(it.key() == "wrap");
        findMode_->setCurrentIndex(0);
    }
    if (page == 7)
        for (const auto& definition : shortcutDefinitions())
            if (auto* key = keys_.value(QString::fromLatin1(definition.id)))
                key->setKeySequence(definition.fallback);
    dirty();
}
} // namespace portwave

void portwave::SettingsDialog::updateConnectionType() {
    if (auto* row = findChild<QWidget*>("settingRow_simulationIntervalMs"))
        row->setVisible(qobject_cast<QComboBox*>(fields_.value("kind"))->currentData().toString() == "test");
    auto* kind = qobject_cast<QComboBox*>(fields_.value("kind"));
    const auto type = kind->currentData().toString();
    const bool serial = type == "serial";
    serialSection_->setVisible(serial);
    if (auto* row = findChild<QWidget*>("settingRow_串口号"))
        row->setVisible(serial);
    fields_["host"]->parentWidget()->setVisible(type == "tcp");
    fields_["tcpPort"]->parentWidget()->setVisible(type == "tcp");
    if (auto* choices = findChild<QWidget*>("connectionTypeChoices"))
        for (auto* button : choices->findChildren<QPushButton*>())
            button->setChecked(button->property("choiceIndex").toInt() == kind->currentIndex());
}
