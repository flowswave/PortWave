#pragma once
#include <QFutureWatcher>
#include <QObject>
#include <QRegularExpression>
#include <QVector>
#include <atomic>
#include <memory>

namespace portwave {
struct SearchOptions {
    QString query;
    int mode{}; // 0 literal, 1 extended escapes, 2 regular expression.
    bool caseSensitive{}, wholeWord{}, dotAll{}, backward{}, wrap = true;
};
struct SearchDocument {
    QString id, path, text;
    int revision{};
    qsizetype anchor{}, selectionStart{}, selectionEnd{};
    bool fromFile{};
};
struct SearchHit {
    QString documentId, path, matched, preview;
    qsizetype position{}, length{};
    int line{}, revision{};
};
struct SearchBatch {
    QVector<SearchHit> hits;
    QStringList errors;
    int total{};
    bool finished{}, cancelled{}, truncated{};
};
QRegularExpression searchExpression(const SearchOptions& options, QString& error);
// GUI facade; scanning/decoding runs in QtConcurrent. Batches and cancellation are bounded.
class SearchService final : public QObject {
    Q_OBJECT
  public:
    using QObject::QObject;
    ~SearchService() override;
    void start(QVector<SearchDocument> sources, SearchOptions options, int limit = 50000);
    void cancel();
    bool busy() const { return busy_; }
  signals:
    void batchReady(const portwave::SearchBatch& batch);
    void progress(int value, int maximum);

  private:
    std::shared_ptr<std::atomic_bool> cancellation_;
    bool busy_{};
};
} // namespace portwave
Q_DECLARE_METATYPE(portwave::SearchHit)
Q_DECLARE_METATYPE(portwave::SearchBatch)
