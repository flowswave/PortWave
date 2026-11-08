#include "infrastructure/session_worker.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QStandardPaths>
#include <QUuid>

namespace portwave {
namespace {
std::span<const std::uint8_t> view(const QByteArray& data) {
    return {reinterpret_cast<const std::uint8_t*>(data.constData()), static_cast<std::size_t>(data.size())};
}
QByteArray byteArray(const Bytes& b) {
    return {reinterpret_cast<const char*>(b.data()), static_cast<qsizetype>(b.size())};
}
} // namespace
SessionWorker::SessionWorker(std::shared_ptr<BoundedQueue> receive, std::shared_ptr<BoundedQueue> transmit)
    : receive_(std::move(receive)), transmit_(std::move(transmit)) {
}
void SessionWorker::open(ConnectionConfig config) {
    config_ = std::move(config);
    paused_ = config_.pauseReceive;
    auto logConfig = config_;
    logConfig.autoLog = false;
    logConfig.rotate = false;
    logger_ = new LogWriter(logConfig);
    logger_->moveToThread(&logThread_);
    connect(&logThread_, &QThread::started, logger_, &LogWriter::start);
    connect(logger_, &LogWriter::pathChanged, this, &SessionWorker::logPathChanged);
    connect(logger_, &LogWriter::failure, this, &SessionWorker::failure);
    connect(logger_, &LogWriter::finished, &logThread_, &QThread::quit, Qt::DirectConnection);
    connect(&logThread_, &QThread::finished, logger_, &QObject::deleteLater);
    connect(&logThread_, &QThread::finished, this, [this] {
        logger_ = nullptr;
        emit closed();
    });
    logThread_.start();
    pump_ = new QTimer(this);
    connect(pump_, &QTimer::timeout, this, &SessionWorker::pumpTransmit);
    pump_->start(10);
    ackTimer_ = new QTimer(this);
    ackTimer_->setSingleShot(true);
    connect(ackTimer_, &QTimer::timeout, this, [this] {
        if (++attempts_ > 3) {
            finishUpgrade(false, "等待 ACK 超时，升级已停止");
            return;
        }
        writeDevice(packet_);
        ackTimer_->start(upgradeStage_ == 0 ? 20000 : 2000);
    });
    if (config_.kind == "test") {
        connected_ = true;
        logger_->configure(config_);
        test_ = new QTimer(this);
        connect(test_, &QTimer::timeout, this, [this] {
            if (paused_)
                return;
            QByteArray batch;
            static const char* colors[] = {"32", "33", "31", "36", "35", "38;5;39", "38;2;30;160;110"};
            static const char* levels[] = {"INFO", "WARN", "ERROR", "DEBUG", "NOTICE", "TRACE", "INFO"};
            static const char* modules[] = {"service", "sensor", "network", "serial",
                                            "system",  "worker", "storage"};
            static const char* messages[] = {"test connection ready; request completed",
                                             "simulated temperature warning: 72 C",
                                             "simulated timeout; retry scheduled",
                                             "rx=AA 55 01 02; checksum OK",
                                             "configuration reloaded",
                                             "256-color output; processing next sample",
                                             "RGB color output; log saved"};
            const auto stamp = QLocale::c().toString(QDateTime::currentDateTime(), "MMM dd HH:mm:ss");
            for (int i = 0; i < 1; ++i) {
                const auto line = ++testLine_;
                const int n = static_cast<int>((line - 1) % 7);
                batch += QString("%1 %2 %3[%4]: \x1b[%5m[%6] %7 serial receive stress sample; UTF-8 中文; "
                                 "ANSI colors; scroll/search responsiveness. \x1b[0m\r\n")
                             .arg(stamp, config_.name, QString::fromLatin1(modules[n]))
                             .arg(line, 8, 10, QChar('0'))
                             .arg(QString::fromLatin1(colors[n]), QString::fromLatin1(levels[n]),
                                  QString::fromLatin1(messages[n]))
                             .toUtf8();
            }
            accept(batch);
        });
        test_->setTimerType(Qt::PreciseTimer);
        test_->start(config_.simulationIntervalMs);
        emit stateChanged(true, "测试连接已建立");
        accept(QString("\x1b[32muser@%1\x1b[0m:\x1b[34m~\x1b[0m$ journalctl -f\r\n[LOCAL SIMULATION] ANSI "
                       "color demo; no external device required\r\n")
                   .arg(config_.name)
                   .toUtf8());
    } else if (config_.kind == "tcp") {
        socket_ = new QTcpSocket(this);
        socket_->setReadBufferSize(1024LL * 1024);
        connect(socket_, &QTcpSocket::readyRead, this, [this] {
            if (!paused_)
                accept(socket_->readAll());
        });
        connect(socket_, &QTcpSocket::connected, this, [this] {
            connected_ = true;
            logger_->configure(config_);
            emit stateChanged(true, "TCP 已连接");
        });
        connect(socket_, &QTcpSocket::disconnected, this, [this] {
            connected_ = false;
            cancelUpgrade();
            suspendLogging();
            emit stateChanged(false, "TCP 已断开");
        });
        connect(socket_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
            emit failure(socket_->errorString());
            emit stateChanged(false, "TCP 连接失败");
        });
        socket_->connectToHost(config_.host, static_cast<quint16>(config_.tcpPort));
        QTimer::singleShot(10000, this, [this] {
            if (socket_ && socket_->state() == QAbstractSocket::ConnectingState) {
                socket_->abort();
                emit failure("TCP 连接超时，请检查地址、端口和防火墙");
            }
        });
    } else {
        serial_ = new QSerialPort(this);
        serial_->setPortName(config_.port);
        serial_->setBaudRate(config_.baud);
        serial_->setReadBufferSize(1024LL * 1024);
        serial_->setDataBits(static_cast<QSerialPort::DataBits>(config_.dataBits));
        serial_->setParity(static_cast<QSerialPort::Parity>(config_.parity));
        serial_->setStopBits(static_cast<QSerialPort::StopBits>(config_.stopBits));
        serial_->setFlowControl(static_cast<QSerialPort::FlowControl>(config_.flow));
        connect(serial_, &QSerialPort::readyRead, this, [this] {
            if (!paused_)
                accept(serial_->readAll());
        });
        connect(serial_, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError error) {
            if (error == QSerialPort::ResourceError) {
                connected_ = false;
                serial_->close();
                cancelUpgrade();
                suspendLogging();
                emit stateChanged(false, "串口已断开");
                emit failure(serial_->errorString());
            }
        });
        connected_ = serial_->open(QIODevice::ReadWrite);
        if (connected_)
            logger_->configure(config_);
        emit stateChanged(connected_, connected_ ? "串口已连接" : "串口打开失败");
        if (!connected_)
            emit failure(config_.port + "：" + serial_->errorString() +
                         "\n请检查端口是否存在、是否被其它程序占用及驱动状态。");
    }
}
void SessionWorker::suspendLogging() {
    if (!logger_)
        return;
    auto inactive = config_;
    inactive.autoLog = false;
    inactive.rotate = false;
    logger_->configure(inactive);
}
void SessionWorker::accept(const QByteArray& bytes) {
    if (bytes.isEmpty())
        return;
    if (logger_)
        logger_->submit(bytes, QDateTime::currentMSecsSinceEpoch());
    if (upgrading_)
        consumeReplies(bytes);
    if (config_.upgrade) {
        for (auto& text : logParser_.feed(view(bytes))) {
            text.push_back('\n');
            receive_->push(std::string(text.begin(), text.end()));
        }
    } else
        receive_->push(bytes.toStdString());
}
bool SessionWorker::writeDevice(const QByteArray& bytes) {
    if (!connected_) {
        emit failure("连接未打开");
        return false;
    }
    if (bytes.size() > 1024LL * 1024) {
        emit failure("单次发送超过 1 MiB 限制");
        return false;
    }
    if (serial_) {
        if (serial_->bytesToWrite() > 1024LL * 1024) {
            emit failure("发送缓冲已满");
            return false;
        }
        return serial_->write(bytes) == bytes.size();
    }
    if (socket_) {
        if (socket_->bytesToWrite() > 1024LL * 1024) {
            emit failure("TCP 发送缓冲已满");
            return false;
        }
        return socket_->write(bytes) == bytes.size();
    }
    if (config_.kind == "test")
        accept("\r\n[TX] " + bytes + "\r\n");
    return true;
}
void SessionWorker::pumpTransmit() {
    auto bytes = transmit_->take(16384);
    if (bytes.empty() || upgrading_)
        return;
    const QByteArray data(bytes.data(), static_cast<qsizetype>(bytes.size()));
    if (!writeDevice(data))
        emit failure("发送失败");
    else if (config_.echo && config_.kind != "test")
        receive_->push(bytes);
}
void SessionWorker::pauseReceive(bool value) {
    paused_ = value;
    if (!value) {
        if (serial_)
            accept(serial_->readAll());
        if (socket_)
            accept(socket_->readAll());
    }
}
void SessionWorker::close() {
    if (closing_)
        return;
    closing_ = true;
    cancelUpgrade();
    if (test_)
        test_->stop();
    if (pump_)
        pump_->stop();
    if (serial_)
        serial_->close();
    if (socket_)
        socket_->abort();
    connected_ = false;
    emit stateChanged(false, "已断开");
    if (logger_)
        logger_->finish();
    else
        emit closed();
}
void SessionWorker::updateConfig(ConnectionConfig config) {
    const bool loggingChanged = config.name != config_.name || config.logDirectory != config_.logDirectory ||
                                config.autoLog != config_.autoLog || config.rotate != config_.rotate ||
                                config.rotateKiB != config_.rotateKiB || config.upgrade != config_.upgrade ||
                                config.timestampLog != config_.timestampLog ||
                                config.plainTextLog != config_.plainTextLog ||
                                config.timestampInterval != config_.timestampInterval;
    config_ = std::move(config);
    paused_ = config_.pauseReceive;
    if (test_ && test_->interval() != config_.simulationIntervalMs)
        test_->setInterval(config_.simulationIntervalMs);
    if (logger_ && connected_ && loggingChanged)
        logger_->configure(config_);
}
void SessionWorker::startUpgrade(const QString& path) {
    auto reject = [this](const QString& message) {
        emit failure(message);
        emit upgradeFinished(false, message);
    };
    if (upgrading_ || !connected_ || config_.kind == "test") {
        reject("请在实际串口或 TCP 连接上启动升级");
        return;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        reject(f.errorString());
        return;
    }
    if (f.size() < 1 || f.size() > 0x70000) {
        reject("固件大小必须在 1 至 458752 字节之间");
        return;
    }
    firmware_ = f.readAll();
    if (firmware_.size() != f.size()) {
        reject("固件读取不完整");
        return;
    }
    imageCrc_ = crc32(view(firmware_));
    packetCount_ = (static_cast<int>(firmware_.size()) + 1023) / 1024;
    upgrading_ = true;
    upgradeStage_ = 0;
    sequence_ = 0;
    replyParser_.reset();
    transmit_->take(1048576U);
    emit upgradeProgress(0);
    sendUpgradePacket();
}
void SessionWorker::sendUpgradePacket() {
    Frame frame;
    frame.total = static_cast<std::uint16_t>(upgradeStage_ == 0 ? packetCount_ : 0);
    frame.sequence = static_cast<std::uint16_t>(upgradeStage_ == 1 ? sequence_ : 0);
    if (upgradeStage_ == 0) {
        frame.opcode = 1;
        append32(frame.payload, static_cast<std::uint32_t>(firmware_.size()));
        append32(frame.payload, static_cast<std::uint32_t>(packetCount_));
        append16(frame.payload, 1024);
        append32(frame.payload, imageCrc_);
    } else if (upgradeStage_ == 1) {
        frame.opcode = 2;
        auto chunk = firmware_.mid(qsizetype(sequence_) * 1024, 1024);
        frame.payload.assign(view(chunk).begin(), view(chunk).end());
    } else {
        frame.opcode = 3;
        append32(frame.payload, imageCrc_);
    }
    packet_ = byteArray(packFrame(frame));
    attempts_ = 0;
    if (!writeDevice(packet_)) {
        finishUpgrade(false, "升级发送失败");
        return;
    }
    ackTimer_->start(upgradeStage_ == 0 ? 20000 : 2000);
}
void SessionWorker::consumeReplies(const QByteArray& bytes) {
    for (const auto& frame : replyParser_.feed(view(bytes))) {
        if (!upgrading_)
            break;
        if (frame.sequence != (upgradeStage_ == 1 ? sequence_ : 0))
            continue;
        if (frame.opcode == 0x82) {
            finishUpgrade(
                false, QString("设备返回 NACK，错误码 %1").arg(frame.payload.empty() ? 6 : frame.payload[0]));
            break;
        }
        if (frame.opcode != 0x81 || frame.payload.size() < 3)
            continue;
        const int progress = frame.payload[0] + int(frame.payload[1]) * 256;
        const int expected = upgradeStage_ == 0 ? 0 : upgradeStage_ == 1 ? sequence_ + 1 : packetCount_;
        const int success = upgradeStage_ == 2 ? 1 : 0;
        if (progress != expected || frame.payload[2] != success)
            continue;
        ackTimer_->stop();
        emit upgradeProgress(progress * 100 / packetCount_);
        if (upgradeStage_ == 2) {
            finishUpgrade(true, "固件升级完成");
            break;
        }
        if (upgradeStage_ == 0)
            upgradeStage_ = 1;
        else if (++sequence_ >= packetCount_)
            upgradeStage_ = 2;
        sendUpgradePacket();
    }
}
void SessionWorker::finishUpgrade(bool success, const QString& message) {
    if (!upgrading_)
        return;
    upgrading_ = false;
    if (ackTimer_)
        ackTimer_->stop();
    firmware_.clear();
    packet_.clear();
    emit upgradeFinished(success, message);
}
void SessionWorker::cancelUpgrade() {
    finishUpgrade(false, "升级已取消");
}
} // namespace portwave
