#pragma once
#include "core/ansi.h"
#include "core/protocol.h"
#include "infrastructure/settings.h"
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QObject>
#include <QTimer>
#include <deque>

namespace portwave {
// Dedicated disk thread. submit/configure/finish are thread-safe and nonblocking.
// A bounded 4 MiB queue drops oldest records with an explicit warning in the log.
class LogWriter final : public QObject {
    Q_OBJECT
  public:
    explicit LogWriter(ConnectionConfig config);
    void submit(QByteArray bytes, qint64 timestampMs);
    void configure(ConnectionConfig config);
    void finish();
    quint64 droppedBytes() const;
  public slots:
    void start();
  signals:
    void pathChanged(const QString& path);
    void failure(const QString& message);
    void finished();

  private:
    struct Record {
        QByteArray bytes;
        qint64 timestampMs{};
        ConnectionConfig config;
        bool reconfigure{};
    };
    void process();
    bool openNext();
    bool writeText(const QByteArray& text);
    void normal(const QByteArray& bytes, const QByteArray& prefix, bool final = false);
    void consume(const Record& record);
    ConnectionConfig config_;
    mutable QMutex mutex_;
    std::deque<Record> records_;
    qsizetype pendingBytes_{};
    quint64 dropped_{}, notedDrops_{};
    bool closing_{}, rotatePending_{}, lineStart_ = true, failed_{};
    qint64 lastTimestamp_{}, written_{};
    QFile file_;
    QTimer* timer_{};
    QElapsedTimer flushClock_;
    QByteArray utf8Pending_;
    AnsiTextFilter ansiTextFilter_;
    LogFrameParser logs_;
    FrameParser frames_;
};
} // namespace portwave
