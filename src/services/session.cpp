#include "services/session.h"
#include <QMetaObject>

namespace portwave {
Session::Session(ConnectionConfig config, QObject* parent) : QObject(parent), config_(std::move(config)) {
    connect(&thread_, &QThread::finished, this, [this] {
        stopped_ = true;
        connected_ = false;
        if (reconnectPending_) {
            reconnectPending_ = false;
            start();
        } else
            emit stopped();
    });
    createWorker();
}
void Session::createWorker() {
    worker_ = new SessionWorker(receive_, transmit_);
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::started, worker_,
            [worker = worker_, config = config_] { worker->open(config); });
    connect(worker_, &SessionWorker::closed, &thread_, &QThread::quit, Qt::DirectConnection);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &SessionWorker::stateChanged, this, [this](bool value, const QString& msg) {
        connected_ = value;
        emit stateChanged(value, msg);
    });
    connect(worker_, &SessionWorker::failure, this, &Session::failure);
    connect(worker_, &SessionWorker::logPathChanged, this, &Session::logPathChanged);
    connect(worker_, &SessionWorker::upgradeProgress, this, &Session::upgradeProgress);
    connect(worker_, &SessionWorker::upgradeFinished, this, [this](bool ok, const QString& msg) {
        upgrading_ = false;
        emit upgradeFinished(ok, msg);
    });
}
Session::~Session() {
    Q_ASSERT(!thread_.isRunning());
    if (!started_)
        delete worker_;
}
void Session::start() {
    if (!thread_.isRunning() && (!started_ || stopped_)) {
        if (stopped_)
            createWorker();
        stopped_ = false;
        started_ = true;
        thread_.start();
    }
}
void Session::stop() {
    reconnectPending_ = false;
    if (!started_) {
        stopped_ = true;
        emit stopped();
        return;
    }
    if (!stopped_)
        QMetaObject::invokeMethod(worker_, &SessionWorker::close, Qt::QueuedConnection);
}
void Session::applyConfig(const ConnectionConfig& config) {
    if (config.toJson() == config_.toJson())
        return;
    config_ = config;
    if (!stopped_)
        QMetaObject::invokeMethod(
            worker_, [worker = worker_, config] { worker->updateConfig(config); }, Qt::QueuedConnection);
    emit configChanged(config_);
}
void Session::reconnect() {
    if (thread_.isRunning()) {
        reconnectPending_ = true;
        transmit_->take(1048576U);
        QMetaObject::invokeMethod(worker_, &SessionWorker::close, Qt::QueuedConnection);
    } else
        start();
}
void Session::send(const QByteArray& bytes) {
    if (!connected_ || upgrading_)
        return;
    if (bytes.size() > 1048576LL || transmit_->size() + static_cast<std::size_t>(bytes.size()) > 1048576ULL) {
        emit failure("发送队列已满");
        return;
    }
    transmit_->push(bytes.toStdString());
}
void Session::pauseReceive(bool value) {
    if (!stopped_)
        QMetaObject::invokeMethod(
            worker_, [worker = worker_, value] { worker->pauseReceive(value); }, Qt::QueuedConnection);
}
void Session::upgrade(const QString& path) {
    if (stopped_ || !connected_ || upgrading_)
        return;
    upgrading_ = true;
    QMetaObject::invokeMethod(
        worker_, [worker = worker_, path] { worker->startUpgrade(path); }, Qt::QueuedConnection);
}
void Session::cancelUpgrade() {
    if (!stopped_)
        QMetaObject::invokeMethod(worker_, &SessionWorker::cancelUpgrade, Qt::QueuedConnection);
}
} // namespace portwave
