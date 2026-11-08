#include "infrastructure/log_writer.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QStandardPaths>
#include <algorithm>

namespace portwave {
LogWriter::LogWriter(ConnectionConfig config) : config_(std::move(config)) {
}
void LogWriter::submit(QByteArray bytes, qint64 timestampMs) {
    QMutexLocker lock(&mutex_);
    if (closing_ || bytes.isEmpty())
        return;
    constexpr qsizetype limit = 4 * 1024 * 1024;
    if (bytes.size() > limit) {
        dropped_ += static_cast<quint64>(bytes.size());
        return;
    }
    while (pendingBytes_ + bytes.size() > limit && !records_.empty()) {
        auto it =
            std::find_if(records_.begin(), records_.end(), [](const Record& r) { return !r.reconfigure; });
        if (it == records_.end())
            break;
        pendingBytes_ -= it->bytes.size();
        dropped_ += static_cast<quint64>(it->bytes.size());
        records_.erase(it);
    }
    pendingBytes_ += bytes.size();
    records_.push_back({std::move(bytes), timestampMs, {}, false});
}
void LogWriter::configure(ConnectionConfig config) {
    QMutexLocker lock(&mutex_);
    if (!closing_)
        records_.push_back({{}, 0, std::move(config), true});
}
void LogWriter::finish() {
    QMutexLocker lock(&mutex_);
    closing_ = true;
}
quint64 LogWriter::droppedBytes() const {
    QMutexLocker lock(&mutex_);
    return dropped_;
}
void LogWriter::start() {
    if (config_.autoLog || config_.rotate)
        openNext();
    flushClock_.start();
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &LogWriter::process);
    timer_->start(20);
}
bool LogWriter::openNext() {
    if (file_.isOpen()) {
        file_.flush();
        file_.close();
    }
    QString directory = config_.logDirectory;
    if (directory.isEmpty())
        directory = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/PortWaveCpp/Logs";
    if (!QDir().mkpath(directory)) {
        failed_ = true;
        emit failure("无法创建日志目录：" + directory);
        return false;
    }
    QString safe;
    for (const auto c : config_.name)
        safe += c.isLetterOrNumber() || c == '-' || c == '_' ? c : QChar('_');
    if (safe.isEmpty())
        safe = "serial";
    const auto now = QDateTime::currentDateTime();
    const auto stem = QString("%1(%2-%3-%4 %5)")
                          .arg(safe)
                          .arg(now.date().year())
                          .arg(now.date().month())
                          .arg(now.date().day())
                          .arg(now.toString("HH_mm_ss"));
    for (int suffix = 0; suffix < 100000; ++suffix) {
        auto path =
            QDir(directory).filePath(stem + (suffix ? "_" + QString::number(suffix) : QString{}) + ".txt");
        if (QFileInfo::exists(path))
            continue;
        file_.setFileName(path);
        if (file_.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            written_ = 0;
            rotatePending_ = false;
            failed_ = false;
            emit pathChanged(path);
            return true;
        }
        if (QFileInfo::exists(path))
            continue;
        break;
    }
    failed_ = true;
    emit failure("日志文件创建失败：" + file_.errorString());
    return false;
}
bool LogWriter::writeText(const QByteArray& text) {
    if (text.isEmpty() || failed_ || !(config_.autoLog || config_.rotate))
        return !failed_;
    const auto saved = config_.plainTextLog ? QByteArray::fromStdString(ansiTextFilter_.feed(std::string_view(
                                                  text.constData(), static_cast<std::size_t>(text.size()))))
                                            : text;
    if (saved.isEmpty())
        return !failed_;
    if ((rotatePending_ || !file_.isOpen()) && !openNext())
        return false;
    if (file_.write(saved) != saved.size()) {
        failed_ = true;
        emit failure("日志写入失败：" + file_.errorString());
        file_.close();
        return false;
    }
    written_ += saved.size();
    if (config_.rotate && written_ >= qint64(config_.rotateKiB) * 1024) {
        if (!file_.flush()) {
            failed_ = true;
            emit failure("日志刷盘失败：" + file_.errorString());
            return false;
        }
        rotatePending_ = true;
    }
    return true;
}
void LogWriter::normal(const QByteArray& bytes, const QByteArray& prefix, bool final) {
    utf8Pending_ += bytes;
    QByteArray valid;
    qsizetype pos = 0;
    while (pos < utf8Pending_.size()) {
        const auto c = static_cast<unsigned char>(utf8Pending_[pos]);
        int length = c < 0x80                 ? 1
                     : c >= 0xc2 && c <= 0xdf ? 2
                     : c >= 0xe0 && c <= 0xef ? 3
                     : c >= 0xf0 && c <= 0xf4 ? 4
                                              : 0;
        if (length && pos + length > utf8Pending_.size() && !final)
            break;
        bool ok = length > 0 && pos + length <= utf8Pending_.size();
        for (int i = 1; ok && i < length; ++i) {
            auto next = static_cast<unsigned char>(utf8Pending_[pos + i]);
            ok = (next & 0xc0U) == 0x80U;
        }
        if (ok && length > 1) {
            auto second = static_cast<unsigned char>(utf8Pending_[pos + 1]);
            ok = !(c == 0xe0 && second < 0xa0) && !(c == 0xed && second >= 0xa0) &&
                 !(c == 0xf0 && second < 0x90) && !(c == 0xf4 && second >= 0x90);
        }
        if (ok) {
            valid += utf8Pending_.mid(pos, length);
            pos += length;
        } else {
            valid += "\\x" + QByteArray(1, utf8Pending_[pos]).toHex();
            ++pos;
        }
    }
    utf8Pending_.remove(0, pos);
    qsizetype start = 0;
    while (start < valid.size()) {
        auto end = valid.indexOf('\n', start);
        bool newline = end >= 0;
        if (!newline)
            end = valid.size();
        auto part = valid.mid(start, end - start);
        QByteArray output;
        if (!part.isEmpty() && lineStart_)
            output += prefix;
        output += part;
        if (newline)
            output += '\n';
        writeText(output);
        lineStart_ = newline;
        start = end + 1;
    }
}
void LogWriter::consume(const Record& record) {
    if (record.reconfigure) {
        const bool rotate =
            config_.name != record.config.name || config_.logDirectory != record.config.logDirectory ||
            config_.autoLog != record.config.autoLog || config_.rotate != record.config.rotate ||
            config_.rotateKiB != record.config.rotateKiB || config_.upgrade != record.config.upgrade ||
            config_.plainTextLog != record.config.plainTextLog;
        if (rotate) {
            normal({}, {}, true);
            if (file_.isOpen()) {
                file_.flush();
                file_.close();
            }
            utf8Pending_.clear();
            ansiTextFilter_.reset();
            logs_ = LogFrameParser{};
            frames_.reset();
            lineStart_ = true;
        }
        config_ = record.config;
        if (rotate && (config_.autoLog || config_.rotate))
            openNext();
        return;
    }
    if (!(config_.autoLog || config_.rotate))
        return;
    QByteArray prefix;
    if (config_.timestampLog &&
        (config_.timestampInterval == 0 ||
         record.timestampMs - lastTimestamp_ >= qint64(config_.timestampInterval) * 1000)) {
        lastTimestamp_ = record.timestampMs;
        const auto time =
            QDateTime::fromMSecsSinceEpoch(record.timestampMs).toString("HH:mm:ss.zzz").chopped(1);
        prefix = ("[" + time + "] ").toUtf8();
    }
    if (!config_.upgrade) {
        normal(record.bytes, prefix);
        return;
    }
    const auto raw = std::span(reinterpret_cast<const std::uint8_t*>(record.bytes.constData()),
                               static_cast<std::size_t>(record.bytes.size()));
    for (const auto& log : logs_.feed(raw))
        writeText(prefix +
                  QByteArray(reinterpret_cast<const char*>(log.data()), static_cast<qsizetype>(log.size())) +
                  "\n");
    for (const auto& frame : frames_.feed(raw)) {
        QString message;
        if (frame.opcode == 0x81) {
            message = QString("[升级协议] ACK  序号=%1").arg(frame.sequence);
            if (frame.payload.size() >= 3)
                message += QString("  进度=%1  成功标志=%2")
                               .arg(frame.payload[0] + int(frame.payload[1]) * 256)
                               .arg(frame.payload[2]);
        } else if (frame.opcode == 0x82) {
            message = QString("[升级协议] NACK  序号=%1  错误码=0x%2")
                          .arg(frame.sequence)
                          .arg(frame.payload.empty() ? 6 : frame.payload[0], 2, 16, QChar('0'));
        } else {
            message = QString("[升级协议] %1  序号=%2  总包数=%3  长度=%4")
                          .arg(frame.opcode == 0x83 ? "STATUS"
                               : frame.opcode == 1  ? "START"
                               : frame.opcode == 2  ? "DATA"
                               : frame.opcode == 3  ? "END"
                                                    : "OP")
                          .arg(frame.sequence)
                          .arg(frame.total)
                          .arg(frame.payload.size());
        }
        writeText(prefix + message.toUtf8() + "\n");
    }
}
void LogWriter::process() {
    QElapsedTimer budget;
    budget.start();
    bool done = false;
    do {
        Record record;
        quint64 dropped = 0;
        {
            QMutexLocker lock(&mutex_);
            dropped = dropped_;
            if (records_.empty()) {
                done = closing_;
                break;
            }
            record = std::move(records_.front());
            records_.pop_front();
            pendingBytes_ -= record.bytes.size();
        }
        if (dropped != notedDrops_) {
            writeText(
                QString("\n[警告] 日志写盘队列已满，丢弃 %1 字节\n").arg(dropped - notedDrops_).toUtf8());
            notedDrops_ = dropped;
            lineStart_ = true;
        }
        consume(record);
    } while (budget.elapsed() < 4);
    if (flushClock_.elapsed() >= 1000 && file_.isOpen()) {
        if (!file_.flush()) {
            failed_ = true;
            emit failure("日志刷盘失败：" + file_.errorString());
        }
        flushClock_.restart();
    }
    if (done) {
        timer_->stop();
        if (!config_.upgrade)
            normal({}, {}, true);
        if (file_.isOpen()) {
            file_.flush();
            file_.close();
        }
        emit finished();
    }
}
} // namespace portwave
