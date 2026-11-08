#pragma once
#include "core/bounded_queue.h"
#include "core/protocol.h"
#include "infrastructure/log_writer.h"
#include "infrastructure/settings.h"
#include <QFile>
#include <QObject>
#include <QSerialPort>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <memory>

namespace portwave {
// All methods, devices, timers and disk writes are confined to one worker thread.
// Errors are signals; received bytes enter a bounded queue, never queued RX signals.
class SessionWorker final : public QObject {
    Q_OBJECT
  public:
    SessionWorker(std::shared_ptr<BoundedQueue> receive, std::shared_ptr<BoundedQueue> transmit);
  public slots:
    void open(portwave::ConnectionConfig config);
    void close();
    void pauseReceive(bool value);
    void startUpgrade(const QString& path);
    void cancelUpgrade();
    void updateConfig(portwave::ConnectionConfig config);
  signals:
    void stateChanged(bool connected, const QString& message);
    void logPathChanged(const QString& path);
    void failure(const QString& message);
    void upgradeProgress(int value);
    void upgradeFinished(bool success, const QString& message);
    void closed();

  private:
    void accept(const QByteArray& bytes);
    void pumpTransmit();
    bool writeDevice(const QByteArray& bytes);
    void sendUpgradePacket();
    void consumeReplies(const QByteArray& bytes);
    void finishUpgrade(bool success, const QString& message);
    void suspendLogging();
    std::shared_ptr<BoundedQueue> receive_, transmit_;
    ConnectionConfig config_;
    QSerialPort* serial_{};
    QTcpSocket* socket_{};
    QTimer *test_{}, *pump_{}, *ackTimer_{};
    QThread logThread_;
    LogWriter* logger_{};
    bool connected_{}, paused_{}, upgrading_{}, closing_{};
    quint64 testLine_{};
    QByteArray firmware_, packet_;
    FrameParser replyParser_;
    LogFrameParser logParser_;
    int upgradeStage_{}, sequence_{}, packetCount_{}, attempts_{};
    std::uint32_t imageCrc_{};
};
} // namespace portwave
