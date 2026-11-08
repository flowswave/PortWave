#include "services/file_service.h"
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QtConcurrent>
#include <exception>
#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#include <io.h>
#endif

namespace portwave {
namespace {
// Incremental RFC 3629 validation, including incomplete final code points.
class Utf8Validator {
  public:
    explicit Utf8Validator(FileReadCursor& cursor) : cursor_(cursor) {}
    bool feed(QByteArrayView bytes) {
        auto& remaining_ = cursor_.remaining;
        auto& minimum_ = cursor_.minimum;
        auto& maximum_ = cursor_.maximum;
        for (const unsigned char c : bytes) {
            if (remaining_) {
                if (c < minimum_ || c > maximum_)
                    return false;
                --remaining_;
                minimum_ = 0x80;
                maximum_ = 0xbf;
            } else if (c < 0x80) {
                continue;
            } else if (c >= 0xc2 && c <= 0xdf) {
                remaining_ = 1;
            } else if (c >= 0xe0 && c <= 0xef) {
                remaining_ = 2;
                minimum_ = c == 0xe0 ? 0xa0 : 0x80;
                maximum_ = c == 0xed ? 0x9f : 0xbf;
            } else if (c >= 0xf0 && c <= 0xf4) {
                remaining_ = 3;
                minimum_ = c == 0xf0 ? 0x90 : 0x80;
                maximum_ = c == 0xf4 ? 0x8f : 0xbf;
            } else {
                return false;
            }
        }
        return true;
    }
    bool complete() const { return cursor_.remaining == 0; }

  private:
    FileReadCursor& cursor_;
};
QString fileIdentity(QFile& file) {
#ifdef Q_OS_WIN
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(reinterpret_cast<HANDLE>(_get_osfhandle(file.handle())), &info))
        return QString("%1:%2:%3")
            .arg(info.dwVolumeSerialNumber)
            .arg(info.nFileIndexHigh)
            .arg(info.nFileIndexLow);
#endif
    return QString::number(QFileInfo(file).birthTime().toMSecsSinceEpoch());
}
} // namespace
FileService::~FileService() {
    cancel();
}
void FileService::cancel() {
    if (cancel_)
        cancel_->store(true);
}
void FileService::open(const QString& path, ChunkConsumer consume, std::optional<FileReadCursor> cursor,
                       bool allowIncomplete) {
    cancel();
    cancel_ = std::make_shared<std::atomic_bool>(false);
    auto token = cancel_;
    auto* watcher = new QFutureWatcher<FileResult>(this);
    connect(watcher, &QFutureWatcher<FileResult>::finished, this, [this, watcher, token] {
        auto result = watcher->result();
        watcher->deleteLater();
        if (!token->load())
            emit loaded(result);
    });
    watcher->setFuture(
        QtConcurrent::run([path, token, consume = std::move(consume), cursor, allowIncomplete] {
            FileResult result;
            result.path = path;
            try {
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly)) {
                    result.error = file.errorString();
                    return result;
                }
                if (!consume) {
                    result.error = "未提供文件读取接收器。";
                    return result;
                }
                const auto snapshot = file.size();
                result.snapshotSize = snapshot;
                result.modified = QFileInfo(file).lastModified().toMSecsSinceEpoch();
                result.cursor = cursor.value_or(FileReadCursor{});
                const auto identity = fileIdentity(file);
                if (cursor && (cursor->identity != identity || snapshot < cursor->offset ||
                               !file.seek(cursor->offset - cursor->guard.size()) ||
                               file.read(cursor->guard.size()) != cursor->guard)) {
                    result.sourceChanged = true;
                    result.error = "日志文件已截断、改写或被替换；已有内容保留，请关闭此标签后重新打开。";
                    return result;
                }
                result.cursor.identity = identity;
                if (!file.seek(result.cursor.offset)) {
                    result.error = file.errorString();
                    return result;
                }
                const auto end = cursor ? qMin(snapshot, result.cursor.offset + 256 * 1024) : snapshot;
                Utf8Validator validator(result.cursor);
                auto& sawCr = result.cursor.sawCr;
                auto& sawCrLf = result.cursor.sawCrLf;
                auto& previousCr = result.cursor.previousCr;
                while (result.cursor.offset < end) {
                    if (token->load()) {
                        result.cancelled = true;
                        return result;
                    }
                    auto chunk = file.read(qMin<qint64>(128 * 1024, end - result.cursor.offset));
                    if (chunk.isEmpty()) {
                        result.error = "文件读取不完整：" + file.errorString();
                        return result;
                    }
                    if (!validator.feed(chunk)) {
                        result.error = "文件包含无效 UTF-8；请先转换编码，原文件未修改。";
                        return result;
                    }
                    for (const char c : chunk) {
                        sawCrLf |= previousCr && c == '\n';
                        sawCr |= c == '\r';
                        previousCr = c == '\r';
                    }
                    result.bytesRead += chunk.size();
                    result.cursor.offset += chunk.size();
                    result.cursor.guard = (result.cursor.guard + chunk).right(64);
                    if (!result.cursor.bomResolved) {
                        chunk.prepend(result.cursor.pendingBom);
                        result.cursor.pendingBom.clear();
                        const QByteArray bom("\xef\xbb\xbf", 3);
                        if (chunk.size() < 3 && bom.startsWith(chunk)) {
                            result.cursor.pendingBom = chunk;
                            continue;
                        }
                        result.cursor.bomResolved = true;
                        if (chunk.startsWith(bom))
                            chunk.remove(0, 3);
                    }
                    const QByteArrayView bytes(chunk);
                    if (!consume(bytes, result.error)) {
                        if (result.error.isEmpty())
                            result.error = "无法构建日志文档。";
                        return result;
                    }
                }
                if (!allowIncomplete && !validator.complete())
                    result.error = "文件末尾包含不完整的 UTF-8 字符；原文件未修改。";
                result.lineEnding = sawCrLf ? "Windows (CR LF)" : sawCr ? "Mac (CR)" : "Unix (LF)";
            } catch (const std::bad_alloc&) {
                result.error = "内存不足，无法载入日志；已有文档保持不变。";
            } catch (const std::exception&) {
                result.error = "读取或构建日志文档失败；已有文档保持不变。";
            }
            return result;
        }));
}
} // namespace portwave
