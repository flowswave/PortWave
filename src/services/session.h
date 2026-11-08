#pragma once
#include "infrastructure/session_worker.h"
#include <QThread>

namespace portwave {
// GUI-thread facade. stop() is asynchronous; stopped() authorizes destruction.
class Session final : public QObject {
    Q_OBJECT
  public:
    explicit Session(ConnectionConfig config, QObject* parent = nullptr);
    ~Session() override;
    void start();
    void stop();
    void send(const QByteArray& bytes);
    std::string take(std::size_t budget = 16384) { return receive_->take(budget); }
    // GUI-thread display catch-up only; bounded read, intentional skipping is
    // counted by dropped(). Does not pause the worker, receive or disk logger.
    std::string takeLatest(std::size_t budget, bool& skipped) {
        return receive_->takeLatest(budget, skipped);
    }
    std::size_t dropped() const { return receive_->dropped(); }
    const ConnectionConfig& config() const { return config_; }
    bool connected() const { return connected_; }
    bool isStopped() const { return stopped_; }
    bool upgrading() const { return upgrading_; }
    void pauseReceive(bool value);
    void upgrade(const QString& path);
    void cancelUpgrade();
    void applyConfig(const ConnectionConfig& config);
    void reconnect();
  signals:
    void stateChanged(bool connected, const QString& message);
    void logPathChanged(const QString& path);
    void failure(const QString& message);
    void stopped();
    void upgradeProgress(int value);
    void upgradeFinished(bool success, const QString& message);
    void configChanged(const portwave::ConnectionConfig& config);

  private:
    ConnectionConfig config_;
    std::shared_ptr<BoundedQueue> receive_ = std::make_shared<BoundedQueue>();
    std::shared_ptr<BoundedQueue> transmit_ = std::make_shared<BoundedQueue>();
    QThread thread_;
    void createWorker();
    SessionWorker* worker_{};
    bool started_{}, stopped_{}, connected_{}, upgrading_{}, reconnectPending_{};
};
} // namespace portwave
