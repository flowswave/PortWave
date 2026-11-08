#include "ui/connection_dialog.h"
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSerialPortInfo>
#include <QStackedWidget>
#include <QUuid>
#include <QVBoxLayout>

namespace portwave {
ConnectionDialog::ConnectionDialog(SettingsRepository& repository, QWidget* parent)
    : QDialog(parent), repository_(repository), configs_(repository.loadConnections()) {
    setWindowTitle("新建连接 · PortWave");
    resize(850, 680);
    setMinimumSize(660, 500);
    auto* root = new QVBoxLayout(this);
    auto* body = new QHBoxLayout;
    root->addLayout(body, 1);
    auto* left = new QVBoxLayout;
    list_ = new QListWidget;
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setMinimumWidth(175);
    left->addWidget(new QLabel("连接配置"));
    left->addWidget(list_, 1);
    auto* actions = new QHBoxLayout;
    auto* add = new QPushButton("新建");
    auto* remove = new QPushButton("删除");
    actions->addWidget(add);
    actions->addWidget(remove);
    left->addLayout(actions);
    body->addLayout(left, 1);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* page = new QWidget;
    auto* form = new QFormLayout(page);
    form->setContentsMargins(20, 12, 20, 12);
    body->addWidget(scroll, 3);
    scroll->setWidget(page);
    simulationInterval_ = new QSpinBox;
    simulationInterval_->setObjectName("simulationIntervalMs");
    simulationInterval_->setRange(1, 60000);
    simulationInterval_->setSuffix(" ms / 行");
    name_ = new QLineEdit;
    kind_ = new QComboBox;
    kind_->setObjectName("connectionKind");
    kind_->addItem("COM 串口", "serial");
    kind_->addItem("TCP 客户端", "tcp");
    kind_->addItem("本地测试连接", "test");
    port_ = new QComboBox;
    port_->setEditable(true);
    auto* portRow = new QHBoxLayout;
    portRow->addWidget(port_, 1);
    auto* scan = new QPushButton("刷新");
    portRow->addWidget(scan);
    auto refresh = [this] {
        auto current = port_->currentText();
        port_->clear();
        for (const auto& p : QSerialPortInfo::availablePorts())
            port_->addItem(p.portName());
        if (!current.isEmpty())
            port_->setCurrentText(current);
    };
    refresh();
    connect(scan, &QPushButton::clicked, this, refresh);
    baud_ = new QComboBox;
    baud_->setEditable(true);
    baud_->addItems({"9600", "19200", "38400", "57600", "115200", "230400", "460800", "921600"});
    dataBits_ = new QComboBox;
    dataBits_->addItems({"5", "6", "7", "8"});
    parity_ = new QComboBox;
    for (auto pair : {qMakePair("无", 0), qMakePair("偶", 2), qMakePair("奇", 3), qMakePair("Space", 4),
                      qMakePair("Mark", 5)})
        parity_->addItem(QString::fromUtf8(pair.first), pair.second);
    stopBits_ = new QComboBox;
    stopBits_->addItem("1", 1);
    stopBits_->addItem("1.5", 3);
    stopBits_->addItem("2", 2);
    flow_ = new QComboBox;
    flow_->addItem("无", 0);
    flow_->addItem("RTS/CTS", 1);
    flow_->addItem("XON/XOFF", 2);
    host_ = new QLineEdit;
    tcpPort_ = new QSpinBox;
    tcpPort_->setRange(1, 65535);
    lines_ = new QSpinBox;
    lines_->setRange(100, 100000);
    lines_->setSingleStep(1000);
    echo_ = new QCheckBox("输入回显");
    upgrade_ = new QCheckBox("升级模式（boot / compass IAP）");
    newline_ = new QCheckBox("新行模式（LF 后回到行首）");
    autoLog_ = new QCheckBox("自动存盘");
    rotate_ = new QCheckBox("按大小分卷");
    rotateSize_ = new QSpinBox;
    rotateSize_->setRange(1, 1024);
    rotateSize_->setSuffix(" MiB");
    logDir_ = new QLineEdit;
    auto* logRow = new QHBoxLayout;
    logRow->addWidget(logDir_);
    auto* browse = new QPushButton("浏览…");
    logRow->addWidget(browse);
    connect(browse, &QPushButton::clicked, this, [this] {
        auto path = QFileDialog::getExistingDirectory(this, "日志目录", logDir_->text());
        if (!path.isEmpty())
            logDir_->setText(path);
    });
    form->addRow("连接名称", name_);
    form->addRow("连接类型", kind_);
    form->addRow("串口", portRow);
    form->addRow("波特率", baud_);
    form->addRow("数据位", dataBits_);
    form->addRow("校验位", parity_);
    form->addRow("停止位", stopBits_);
    form->addRow("流控制", flow_);
    form->addRow("TCP 地址", host_);
    form->addRow("TCP 端口", tcpPort_);
    lines_->setObjectName("receiveDisplayMaxLines");
    lines_->setToolTip("只限制接收区显示行数，日志文件继续完整保存");
    form->addRow("接收显示最大行数", lines_);
    form->addRow("仿真每行间隔", simulationInterval_);
    connect(kind_, &QComboBox::currentIndexChanged, this, [this, form] {
        form->setRowVisible(simulationInterval_, kind_->currentData().toString() == "test");
    });
    form->addRow(echo_);
    form->addRow(upgrade_);
    form->addRow(newline_);
    form->addRow(autoLog_);
    form->addRow("日志目录", logRow);
    form->addRow(rotate_, rotateSize_);
    auto* buttons = new QDialogButtonBox;
    auto* open = buttons->addButton("打开所选", QDialogButtonBox::AcceptRole);
    auto* save = buttons->addButton("保存", QDialogButtonBox::ApplyRole);
    buttons->addButton("取消", QDialogButtonBox::RejectRole);
    root->addWidget(buttons);
    for (const auto& c : configs_)
        list_->addItem(c.name);
    connect(list_, &QListWidget::currentRowChanged, this, &ConnectionDialog::load);
    connect(add, &QPushButton::clicked, this, [this] {
        store();
        ConnectionConfig c;
        c.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        c.name = "连接" + QString::number(configs_.size() + 1);
        configs_.push_back(c);
        list_->addItem(c.name);
        list_->setCurrentRow(list_->count() - 1);
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        auto i = list_->currentRow();
        if (i < 0)
            return;
        index_ = -1;
        configs_.removeAt(i);
        delete list_->takeItem(i);
        if (!configs_.empty())
            load(list_->currentRow());
    });
    connect(save, &QPushButton::clicked, this, [this] { persist(); });
    connect(open, &QPushButton::clicked, this, [this] {
        if (persist() && !selected().isEmpty())
            accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    if (configs_.isEmpty())
        add->click();
    else
        list_->setCurrentRow(0);
}
void ConnectionDialog::store() {
    if (index_ < 0 || index_ >= configs_.size())
        return;
    auto& c = configs_[index_];
    c.name = name_->text().trimmed();
    c.kind = kind_->currentData().toString();
    c.port = port_->currentText().trimmed();
    c.baud = baud_->currentText().toInt();
    c.host = host_->text().trimmed();
    c.tcpPort = tcpPort_->value();
    c.dataBits = dataBits_->currentText().toInt();
    c.parity = parity_->currentData().toInt();
    c.stopBits = stopBits_->currentData().toInt();
    c.flow = flow_->currentData().toInt();
    c.bufferLines = lines_->value();
    c.simulationIntervalMs = simulationInterval_->value();
    c.echo = echo_->isChecked();
    c.upgrade = upgrade_->isChecked();
    c.newline = newline_->isChecked();
    c.autoLog = autoLog_->isChecked();
    c.logDirectory = logDir_->text();
    c.rotate = rotate_->isChecked();
    c.rotateMiB = rotateSize_->value();
    if (auto* item = list_->item(index_))
        item->setText(c.name);
}
void ConnectionDialog::load(int index) {
    store();
    index_ = index;
    if (index < 0 || index >= configs_.size())
        return;
    const auto& c = configs_[index];
    name_->setText(c.name);
    kind_->setCurrentIndex(kind_->findData(c.kind));
    port_->setCurrentText(c.port);
    baud_->setCurrentText(QString::number(c.baud));
    dataBits_->setCurrentText(QString::number(c.dataBits));
    parity_->setCurrentIndex(parity_->findData(c.parity));
    stopBits_->setCurrentIndex(stopBits_->findData(c.stopBits));
    flow_->setCurrentIndex(flow_->findData(c.flow));
    host_->setText(c.host);
    tcpPort_->setValue(c.tcpPort);
    lines_->setValue(c.bufferLines);
    simulationInterval_->setValue(c.simulationIntervalMs);
    qobject_cast<QFormLayout*>(simulationInterval_->parentWidget()->layout())
        ->setRowVisible(simulationInterval_, c.kind == "test");
    echo_->setChecked(c.echo);
    upgrade_->setChecked(c.upgrade);
    newline_->setChecked(c.newline);
    autoLog_->setChecked(c.autoLog);
    logDir_->setText(c.logDirectory);
    rotate_->setChecked(c.rotate);
    rotateSize_->setValue(c.rotateMiB);
}
bool ConnectionDialog::persist() {
    store();
    for (const auto& c : configs_)
        if (c.name.isEmpty() || c.baud < 50 || c.baud > 4000000 || (c.kind == "serial" && c.port.isEmpty()) ||
            (c.kind == "tcp" && c.host.isEmpty())) {
            QMessageBox::warning(this, "配置不完整", "请填写名称、有效的串口/地址及波特率。");
            return false;
        }
    QString error;
    if (!repository_.saveConnections(configs_, error)) {
        QMessageBox::warning(this, "保存失败", error);
        return false;
    }
    return true;
}
QVector<ConnectionConfig> ConnectionDialog::selected() const {
    QVector<ConnectionConfig> result;
    for (auto* item : list_->selectedItems()) {
        auto i = list_->row(item);
        if (i >= 0 && i < configs_.size())
            result.push_back(configs_[i]);
    }
    return result;
}
} // namespace portwave
