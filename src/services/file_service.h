#pragma once
#include <QByteArrayView>
#include <QFutureWatcher>
#include <QObject>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace portwave {
// Value checkpoint owned by the reader. Copy into one worker, commit only after
// successful publication. Offset counts source bytes, including BOM/ANSI; guard
// is at most 64 bytes. UTF-8 and newline state continue across tail snapshots.
struct FileReadCursor {
    qint64 offset{};
    QString identity;
    QByteArray guard;
    QByteArray pendingBom;
    bool bomResolved{};
    int remaining{};
    unsigned char minimum{0x80}, maximum{0xbf};
    bool sawCr{}, sawCrLf{}, previousCr{};
};
struct FileResult {
    QString path, error;
    bool cancelled{};
    QString lineEnding;
    qint64 bytesRead{};
    FileReadCursor cursor;
    qint64 snapshotSize{}, modified{};
    bool sourceChanged{};
};
// GUI facade; snapshot I/O and UTF-8 validation occur in the thread pool.
// No fixed size cap. Consumer is invoked serially on that worker, with borrowed
// chunk bytes valid only during the call. It must not touch QWidget. Returning
// false aborts with its error. Completion publishes metadata, never full text.
class FileService final : public QObject {
    Q_OBJECT
  public:
    using QObject::QObject;
    ~FileService() override;
    using ChunkConsumer = std::function<bool(QByteArrayView, QString&)>;
    // A checkpoint requests only the unread tail (at most 256 KiB per call).
    // Source replacement/truncation returns sourceChanged without publishing.
    // allowIncomplete retains a partial final UTF-8 code point for a live log.
    // Consumer/state must be detached; GUI commits only on error-free loaded.
    void open(const QString& path, ChunkConsumer consume, std::optional<FileReadCursor> cursor = {},
              bool allowIncomplete = false);
    void cancel();
  signals:
    void loaded(const portwave::FileResult& result);

  private:
    std::shared_ptr<std::atomic_bool> cancel_;
};
} // namespace portwave
Q_DECLARE_METATYPE(portwave::FileResult)
