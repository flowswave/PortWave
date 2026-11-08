#include "services/search_service.h"
#include <QFile>
#include <QPromise>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QtConcurrent>
#include <exception>

namespace portwave {
namespace {
QString stripAnsi(const QString& input) {
    QString result = input;
    // Keep line breaks and ordinary text while removing complete ANSI CSI/OSC
    // sequences. The file search operates on the same plain text that the log
    // editor displays, so hit offsets remain valid when a result is activated.
    result.remove(QRegularExpression(QStringLiteral("\\x1B\\[[0-?]*[ -/]*[@-~]")));
    result.remove(QRegularExpression(QStringLiteral("\\x1B\\][^\\x07]*(?:\\x07|\\x1B\\\\)")));
    return result;
}
} // namespace
QRegularExpression searchExpression(const SearchOptions& options, QString& error) {
    QString query = options.query;
    if (query.isEmpty()) {
        error = "请输入查找内容";
        return {};
    }
    if (options.mode == 1) {
        QString expanded;
        for (qsizetype i = 0; i < query.size(); ++i) {
            auto c = query[i];
            if (c != '\\') {
                expanded += c;
                continue;
            }
            if (++i >= query.size()) {
                error = "扩展模式存在不完整转义";
                return {};
            }
            c = query[i];
            if (c == 'n')
                expanded += '\n';
            else if (c == 'r')
                expanded += '\r';
            else if (c == 't')
                expanded += '\t';
            else if (c == '0')
                expanded += QChar(0);
            else if (c == '\\')
                expanded += '\\';
            else if (c == 'x' || c == 'u') {
                int digits = c == 'x' ? 2 : 4;
                if (i + digits >= query.size()) {
                    error = "十六进制转义不完整";
                    return {};
                }
                bool ok = false;
                auto value = query.mid(i + 1, digits).toUShort(&ok, 16);
                if (!ok) {
                    error = "十六进制转义无效";
                    return {};
                }
                expanded += QChar(value);
                i += digits;
            } else {
                error = "不支持的扩展转义：\\" + QString(c);
                return {};
            }
        }
        query = expanded;
    }
    if (options.mode != 2)
        query = QRegularExpression::escape(query);
    if (options.wholeWord)
        query = "(?<![\\p{L}\\p{N}_])(?:" + query + ")(?![\\p{L}\\p{N}_])";
    auto flags = QRegularExpression::UseUnicodePropertiesOption | QRegularExpression::MultilineOption;
    if (!options.caseSensitive)
        flags |= QRegularExpression::CaseInsensitiveOption;
    if (options.dotAll && options.mode == 2)
        flags |= QRegularExpression::DotMatchesEverythingOption;
    QRegularExpression expression(query, flags);
    if (!expression.isValid())
        error = expression.errorString();
    return expression;
}
SearchService::~SearchService() {
    cancel();
}
void SearchService::cancel() {
    if (cancellation_)
        cancellation_->store(true);
    busy_ = false;
}
void SearchService::start(QVector<SearchDocument> sources, SearchOptions options, int limit) {
    cancel();
    QString error;
    auto expression = searchExpression(options, error);
    if (!error.isEmpty()) {
        emit batchReady({{}, {error}, 0, true, false, false});
        return;
    }
    cancellation_ = std::make_shared<std::atomic_bool>(false);
    auto token = cancellation_;
    busy_ = true;
    auto* watcher = new QFutureWatcher<SearchBatch>(this);
    connect(watcher, &QFutureWatcher<SearchBatch>::resultsReadyAt, this,
            [this, watcher, token](int begin, int end) {
                if (token->load())
                    return;
                for (int i = begin; i < end; ++i)
                    emit batchReady(watcher->resultAt(i));
            });
    connect(watcher, &QFutureWatcher<SearchBatch>::progressValueChanged, this,
            [this, watcher, token](int value) {
                if (!token->load())
                    emit progress(value, watcher->progressMaximum());
            });
    connect(watcher, &QFutureWatcher<SearchBatch>::finished, this, [this, watcher, token] {
        if (!token->load())
            busy_ = false;
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([sources = std::move(sources), expression, token,
                                          limit](QPromise<SearchBatch>& promise) {
        promise.setProgressRange(0, sources.size());
        SearchBatch batch;
        QStringList allErrors;
        int total = 0, processed = 0;
        try {
            for (auto source : sources) {
                if (token->load() || promise.isCanceled())
                    break;
                if (source.fromFile) {
                    QFile file(source.path);
                    if (!file.open(QIODevice::ReadOnly)) {
                        allErrors.push_back(source.path + "：" + file.errorString());
                        continue;
                    }
                    const qint64 snapshot = file.size();
                    QStringDecoder decoder(QStringDecoder::Utf8);
                    qint64 done = 0;
                    source.text.reserve(static_cast<qsizetype>(snapshot));
                    while (done < snapshot && !token->load()) {
                        auto part = file.read(qMin<qint64>(65536, snapshot - done));
                        if (part.isEmpty())
                            break;
                        done += part.size();
                        source.text += decoder(part);
                    }
                    if (token->load())
                        break;
                    if (done != snapshot || decoder.hasError()) {
                        allErrors.push_back(source.path + "：读取不完整或不是有效 UTF-8");
                        continue;
                    }
                    source.text.replace("\r\n", "\n");
                    source.text.replace("\n\r", "\n");
                    source.text = stripAnsi(source.text);
                }
                auto matches = expression.globalMatch(source.text);
                qsizetype lineOffset = 0;
                int line = 0;
                while (matches.hasNext() && !token->load()) {
                    const auto match = matches.next();
                    const auto pos = match.capturedStart();
                    while (lineOffset < pos) {
                        auto next = source.text.indexOf('\n', lineOffset);
                        if (next < 0 || next >= pos)
                            break;
                        ++line;
                        lineOffset = next + 1;
                    }
                    auto end = source.text.indexOf('\n', pos);
                    if (end < 0)
                        end = source.text.size();
                    auto begin = source.text.lastIndexOf('\n', pos > 0 ? pos - 1 : 0);
                    if (pos == 0)
                        begin = -1;
                    batch.hits.push_back({source.id, source.path, match.captured(),
                                          source.text.mid(begin + 1, qMin<qsizetype>(end - begin - 1, 2048)),
                                          pos, match.capturedLength(), line, source.revision});
                    ++total;
                    if (batch.hits.size() >= 200) {
                        batch.total = total;
                        promise.addResult(batch);
                        batch.hits.clear();
                    }
                    if (total >= limit)
                        break;
                }
                promise.setProgressValue(++processed);
                if (total >= limit)
                    break;
            }
        } catch (const std::bad_alloc&) {
            allErrors.push_back("内存不足，日志检索已停止；已有结果保留。");
        } catch (const std::exception&) {
            allErrors.push_back("日志读取或检索失败；已有结果保留。");
        }
        batch.total = total;
        batch.errors = std::move(allErrors);
        batch.finished = true;
        batch.cancelled = token->load();
        batch.truncated = total >= limit;
        promise.addResult(batch);
    }));
}
} // namespace portwave
