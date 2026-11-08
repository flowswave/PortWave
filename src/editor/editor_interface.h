#pragma once
#include <QByteArrayView>
#include <QColor>
#include <QFont>
#include <QIcon>
#include <QString>
#include <QVector>
#include <memory>
#include <utility>
class QWidget;
namespace portwave {
// Pure display-color conversion, safe on any thread. Matches the reference's
// 4.5:1 ANSI contrast rule; never rewrites received or stored color codes.
QColor readableAnsiColor(const QColor& foreground, const QColor& background);
// GUI-thread display mapping of standard ANSI colors through current theme tokens,
// followed by readability adjustment. Does not change raw spans or saved bytes.
QColor displayAnsiColor(const QColor& foreground, const QColor& background, bool dark);
// GUI-thread view palette lookup. Read-only snapshot installed by the host;
// isolated views or a different mode use fallback. No ownership or I/O.
QColor appearanceColor(const QString& role, bool dark, const QColor& fallback);
// GUI-only theme-aware semantic mark style and matching menu swatch. No text I/O.
struct MarkAppearance {
    QColor background, foreground;
    QString name;
};
MarkAppearance markAppearance(int index, bool dark, const QColor& viewBackground = {});
QIcon markSwatch(int index, bool dark, const QColor& viewBackground = {});

struct TextMark {
    qsizetype position{}, length{};
    int color{};
    bool line{};
};
struct EditorAnsiSpan {
    qsizetype position{}, length{};
    int foreground{-1}, background{-1};
    bool bold{};
};
struct EditorViewState {
    int line{}, column{}, lineCount{}, firstVisibleLine{}, horizontalOffset{};
};
// Detached document builder. Created on the GUI thread; appendUtf8 is called
// serially by one worker. After that worker finishes, adopt on the GUI thread.
// No QWidget access during append. Ownership releases unfinished/cancelled data.
// Errors return false with a message; no partial document is published.
class EditorLoadBuffer {
  public:
    virtual ~EditorLoadBuffer() = default;
    virtual bool appendUtf8(QByteArrayView bytes, QString& error) = 0;
};
// GUI-thread only; concrete editor types never cross the business boundary.
class EditorInterface {
  public:
    virtual ~EditorInterface() = default;
    virtual QWidget* widget() = 0;
    virtual void setText(const QString& text) = 0;
    // GUI-thread creation/adoption, detached buffer owned by the caller. A
    // successful adoption consumes its document; failure leaves this editor intact.
    // expectedBytes is an allocation hint, never a file size limit; zero grows on demand.
    virtual std::shared_ptr<EditorLoadBuffer> createLoadBuffer(bool largeFile, QString& error,
                                                               qsizetype expectedBytes = 0) = 0;
    virtual bool adoptLoadBuffer(EditorLoadBuffer& buffer, QString& error) = 0;
    // GUI-thread, bounded complete UTF-8 tail from a successful background read.
    // Appends to the same document, styles only the new range; absolute UTF-16
    // spans belong to that tail. Preserves selection, scrolling and old marks.
    // False leaves existing text intact and reports error; caller retains cursor.
    virtual bool appendLogUtf8(QByteArrayView bytes, const QVector<EditorAnsiSpan>& styles,
                               QString& error) = 0;
    virtual QString text() const = 0;
    virtual bool find(const QString& query, bool regex, bool caseSensitive, bool forward) = 0;
    virtual void setTheme(bool dark) = 0;
    virtual void setReadOnly(bool value) = 0;
    virtual void setWrap(bool value) = 0;
    virtual void goToLine(int line) = 0;
    virtual qsizetype position() const = 0;
    virtual std::pair<qsizetype, qsizetype> selection() const = 0;
    virtual void selectRange(qsizetype position, qsizetype length) = 0;
    virtual void setNumbers(bool enabled) = 0;
    virtual void setAppearance(const QFont& font, const QColor& background, const QColor& foreground) = 0;
    // GUI thread; first batch is immediate, remaining ANSI styling is time sliced.
    // Replacing styles/document cancels prior batches; spans are UTF-16 positions.
    virtual void setAnsiStyles(const QVector<EditorAnsiSpan>& styles, bool enabled) = 0;
    // GUI-thread search overlay, independent of ANSI styles and user marks.
    // Positions are UTF-16 units; an empty list clears only this overlay.
    virtual void highlightMatches(const QVector<TextMark>& matches, bool append = false) = 0;
    // GUI-thread, cheap view snapshots. Restoring clamps the cursor to current
    // content; cursor=false restores scroll only. No document changes or I/O.
    virtual EditorViewState viewState() const = 0;
    virtual void restoreViewState(const EditorViewState& state, bool cursor = true) = 0;
    virtual void mark(const QVector<TextMark>& marks, bool clearPrevious) = 0;
    // GUI-thread owned snapshot used when cloning or refreshing a document.
    virtual QVector<TextMark> marks() const = 0;
    virtual void clearMarks(int color = -1) = 0;
    virtual QString markedText() const = 0;
};
} // namespace portwave
