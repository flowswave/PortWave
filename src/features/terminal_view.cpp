#include "features/terminal_view.h"
#include "core/commands.h"
#include "editor/editor_interface.h"
#include <QApplication>
#include <QClipboard>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <cmath>

namespace portwave {
TerminalView::TerminalView(QWidget* parent) : QTextEdit(parent) {
    setReadOnly(true);
    setAcceptRichText(false);
    setFont(QFont("Consolas", 12));
    setUndoRedoEnabled(false);
    document()->setMaximumBlockCount(10000);
    document()->setDocumentMargin(16);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    recolorTimer_.setSingleShot(true);
    connect(&recolorTimer_, &QTimer::timeout, this, &TerminalView::recolor);
    setTheme(true);
}
void TerminalView::setTheme(bool dark) {
    dark_ = dark;
    setDisplayColors(appearanceColor("surface", dark, QColor(dark ? "#282c34" : "#ffffff")),
                     appearanceColor("text", dark, QColor(dark ? "#abb2bf" : "#253345")));
}
void TerminalView::setDisplayColors(const QColor& background, const QColor& foreground) {
    if (!background.isValid() || !foreground.isValid())
        return;
    displayBackground_ = background;
    auto p = palette();
    p.setColor(QPalette::Base, background);
    p.setColor(QPalette::Text, foreground);
    setPalette(p);
    colorCache_.clear();
    recolorPosition_ = 0;
    setProperty("recoloring", true);
    recolorTimer_.start(0);
    viewport()->update();
}
QColor TerminalView::readableColor(int rgb) {
    if (colorCache_.contains(rgb))
        return colorCache_[rgb];
    const auto bg = displayBackground_;
    const auto color = displayAnsiColor(QColor::fromRgb(static_cast<QRgb>(rgb)), bg, dark_);
    if (colorCache_.size() < 512)
        colorCache_.insert(rgb, color);
    return color;
}
void TerminalView::recolor() {
    QElapsedTimer budget;
    budget.start();
    auto block = document()->findBlock(recolorPosition_);
    while (block.isValid()) {
        struct Span {
            int start, length, raw;
        };
        QVector<Span> spans;
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            auto fragment = it.fragment();
            auto raw = fragment.charFormat().property(QTextFormat::UserProperty + 1);
            if (raw.isValid())
                spans.push_back({fragment.position(), fragment.length(), raw.toInt()});
        }
        for (const auto& span : spans) {
            QTextCursor cursor(document());
            cursor.setPosition(span.start);
            cursor.setPosition(span.start + span.length, QTextCursor::KeepAnchor);
            QTextCharFormat format;
            format.setForeground(readableColor(span.raw));
            cursor.mergeCharFormat(format);
        }
        block = block.next();
        if (budget.elapsed() >= 4)
            break;
    }
    if (block.isValid()) {
        recolorPosition_ = block.position();
        recolorTimer_.start(0);
    } else {
        setProperty("recoloring", false);
    }
}
void TerminalView::resumeLatest(std::string_view bytes, bool skipped) {
    if (skipped) {
        decoder_.reset();
        if (mode_ == 0) {
            const auto newline = bytes.find('\n');
            if (newline != std::string_view::npos)
                bytes.remove_prefix(newline + 1);
            if (column_)
                consume("\n");
        }
    }
    consume(bytes);
    followEnd();
}
void TerminalView::resetTerminal() {
    clear();
    decoder_.reset();
    column_ = 0;
    lastTimestamp_ = {};
}
void TerminalView::followEnd() {
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}
void TerminalView::consume(std::string_view bytes) {
    auto* bar = verticalScrollBar();
    bool follow = bar->value() >= bar->maximum() - 2;
    int old = bar->value();
    const int tailDistance = bar->maximum() - old;
    const auto now = QDateTime::currentDateTime();
    const bool timestampDue =
        timestampEnabled_ && (!lastTimestamp_.isValid() || timestampIntervalSeconds_ == 0 ||
                              lastTimestamp_.secsTo(now) >= timestampIntervalSeconds_);
    if (timestampDue)
        lastTimestamp_ = now;
    const QString stamp = timestampDue ? QString("[%1] ").arg(now.toString("hh:mm:ss.zzz")) : QString{};
    QTextCursor cursor(document());
    cursor.movePosition(QTextCursor::End);
    cursor.beginEditBlock();
    bool timestampPending = column_ == 0;
    auto insertTimestamp = [&] {
        if (stamp.isEmpty())
            return;
        cursor.insertText(stamp, QTextCharFormat{});
        column_ += stamp.size();
        timestampPending = false;
    };
    if (mode_ == 1) {
        QByteArray raw(bytes.data(), static_cast<qsizetype>(bytes.size()));
        cursor.insertText((timestampPending ? stamp : QString{}) +
                          QString::fromLatin1(raw.toHex(' ').toUpper()) + "\n");
        if (!stamp.isEmpty())
            lastTimestamp_ = now;
    } else if (mode_ == 2) {
        QByteArray raw(bytes.data(), static_cast<qsizetype>(bytes.size()));
        for (qsizetype offset = 0; offset < raw.size(); offset += 16) {
            auto chunk = raw.mid(offset, 16);
            QString ascii;
            for (auto c : chunk)
                ascii += c >= 32 && c <= 126 ? QChar::fromLatin1(c) : QChar('.');
            cursor.insertText((offset == 0 && timestampPending ? stamp : QString{}) +
                              QString::fromLatin1(chunk.toHex(' ').toUpper()).leftJustified(48, ' ') + "  " +
                              ascii + "\n");
        }
        if (!stamp.isEmpty())
            lastTimestamp_ = now;
    } else
        for (const auto& run : decoder_.feed(bytes)) {
            if (run.kind == TerminalRun::Kind::CarriageReturn) {
                column_ = 0;
                continue;
            }
            if (run.kind == TerminalRun::Kind::Backspace) {
                column_ = qMax(0, column_ - 1);
                continue;
            }
            if (run.kind == TerminalRun::Kind::Newline) {
                cursor.movePosition(QTextCursor::End);
                if (timestampPending)
                    insertTimestamp();
                cursor.insertBlock();
                timestampPending = true;
                if (newline_)
                    column_ = 0;
                continue;
            }
            cursor.movePosition(QTextCursor::End);
            cursor.movePosition(QTextCursor::StartOfBlock);
            if (timestampPending) {
                cursor.movePosition(QTextCursor::EndOfBlock);
                insertTimestamp();
                cursor.movePosition(QTextCursor::StartOfBlock);
            }
            const int len = cursor.block().length() - 1;
            if (run.kind == TerminalRun::Kind::EraseLine) {
                if (run.argument == 2) {
                    cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
                    cursor.removeSelectedText();
                    column_ = 0;
                } else {
                    cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, qMin(column_, len));
                    cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
                    cursor.removeSelectedText();
                }
                continue;
            }
            cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, qMin(column_, len));
            if (column_ > len)
                cursor.insertText(QString(column_ - len, ' '));
            const QString text = QString::fromUtf8(run.utf8.data(), static_cast<qsizetype>(run.utf8.size()));
            QTextCharFormat format;
            format.setFontWeight(run.style.bold ? QFont::Bold : QFont::Normal);
            if (run.style.foreground >= 0) {
                format.setProperty(QTextFormat::UserProperty + 1, run.style.foreground);
                format.setForeground(readableColor(run.style.foreground));
            }
            // Default foreground is inherited from the palette, so theme changes preserve document and
            // selection.
            if (run.style.background >= 0)
                format.setBackground(QColor::fromRgb(static_cast<QRgb>(run.style.background)));
            const int remaining = len - qMin(column_, len);
            if (remaining > 0)
                cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor,
                                    qMin(static_cast<int>(text.size()), remaining));
            cursor.insertText(text, format);
            column_ += static_cast<int>(text.size());
        }
    cursor.endEditBlock();
    // A device without LF must not grow one QTextBlock without a limit.
    if (document()->characterCount() > 4 * 1024 * 1024) {
        QTextCursor trim(document());
        trim.setPosition(0);
        trim.setPosition(document()->characterCount() - 2 * 1024 * 1024, QTextCursor::KeepAnchor);
        trim.removeSelectedText();
    }
    if (column_ > 32768) {
        QTextCursor end(document());
        end.movePosition(QTextCursor::End);
        end.insertBlock();
        column_ = 0;
    }
    if (scrollMode_ == 1 || (scrollMode_ == 0 && follow))
        bar->setValue(bar->maximum());
    else if (scrollMode_ == 2)
        bar->setValue(qMax(0, bar->maximum() - tailDistance));
    else
        bar->setValue(old);
}
void TerminalView::pasteToDevice() {
    auto text = normalizePaste(QApplication::clipboard()->text().toUtf8().toStdString());
    emit input(QByteArray(text.data(), static_cast<qsizetype>(text.size())));
}
void TerminalView::keyPressEvent(QKeyEvent* e) {
    const bool resumeKey =
        e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || e->key() == Qt::Key_Space;
    if (resumeKeyHeld_ && resumeKey) {
        e->accept();
        return;
    }
    if (receivePaused_ && !composing_ && resumeKey &&
        (e->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier) {
        resumeKeyHeld_ = true;
        emit resumeReceiveRequested();
        e->accept();
        return;
    }
    // After an explicit disconnect the terminal has no target to send to; Space /
    // Return reconnects the same connection instead of emitting TX bytes.
    if (disconnected_ && !composing_ && resumeKey &&
        (e->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier) {
        resumeKeyHeld_ = true;
        emit reconnectRequested();
        e->accept();
        return;
    }
    const auto mods = e->modifiers();
    const bool ctrl = mods.testFlag(Qt::ControlModifier), shift = mods.testFlag(Qt::ShiftModifier);
    if ((ctrl && e->key() == Qt::Key_C) || (ctrl && e->key() == Qt::Key_Insert)) {
        copy();
        return;
    }
    if ((ctrl && e->key() == Qt::Key_V) || (shift && e->key() == Qt::Key_Insert)) {
        pasteToDevice();
        return;
    }
    if (shift && (e->key() == Qt::Key_PageUp || e->key() == Qt::Key_PageDown)) {
        QTextEdit::keyPressEvent(e);
        return;
    }
    if (e->key() == Qt::Key_Down && verticalScrollBar()->value() < verticalScrollBar()->maximum() &&
        !e->isAutoRepeat()) {
        if (downPress_.isValid() && downPress_.elapsed() < 500) {
            followEnd();
            downPress_.invalidate();
        } else
            downPress_.start();
        return;
    }
    QByteArray inputBytes;
    if (ctrl && e->key() >= Qt::Key_A && e->key() <= Qt::Key_Z)
        inputBytes.append(static_cast<char>(e->key() - Qt::Key_A + 1));
    else if (ctrl && e->key() == Qt::Key_Space)
        inputBytes.append('\0');
    else if (ctrl && e->key() >= Qt::Key_BracketLeft && e->key() <= Qt::Key_Underscore)
        inputBytes.append(static_cast<char>(e->key() - 64));
    else
        switch (e->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            inputBytes = "\r";
            break;
        case Qt::Key_Backspace:
            inputBytes = shift ? "\b" : "\x7f";
            break;
        case Qt::Key_Tab:
            inputBytes = "\t";
            break;
        case Qt::Key_Escape:
            inputBytes = "\x1b";
            break;
        case Qt::Key_Up:
            inputBytes = "\x1b[A";
            break;
        case Qt::Key_Down:
            inputBytes = "\x1b[B";
            break;
        case Qt::Key_Right:
            inputBytes = "\x1b[C";
            break;
        case Qt::Key_Left:
            inputBytes = "\x1b[D";
            break;
        case Qt::Key_Home:
            inputBytes = "\x1b[H";
            break;
        case Qt::Key_End:
            inputBytes = "\x1b[F";
            break;
        case Qt::Key_Insert:
            inputBytes = "\x1b[2~";
            break;
        case Qt::Key_Delete:
            inputBytes = "\x1b[3~";
            break;
        case Qt::Key_PageUp:
            inputBytes = "\x1b[5~";
            break;
        case Qt::Key_PageDown:
            inputBytes = "\x1b[6~";
            break;
        default:
            if (e->key() >= Qt::Key_F1 && e->key() <= Qt::Key_F12) {
                const char* sequences[] = {"\x1bOP",   "\x1bOQ",   "\x1bOR",   "\x1bOS",
                                           "\x1b[15~", "\x1b[17~", "\x1b[18~", "\x1b[19~",
                                           "\x1b[20~", "\x1b[21~", "\x1b[23~", "\x1b[24~"};
                inputBytes = sequences[e->key() - Qt::Key_F1];
            } else
                inputBytes = e->text().toUtf8();
            break;
        }
    if (mods.testFlag(Qt::AltModifier) && !inputBytes.isEmpty())
        inputBytes.prepend('\x1b');
    if (!inputBytes.isEmpty())
        emit input(inputBytes);
    e->accept();
}
void TerminalView::keyReleaseEvent(QKeyEvent* e) {
    if (resumeKeyHeld_ &&
        (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || e->key() == Qt::Key_Space)) {
        if (!e->isAutoRepeat())
            resumeKeyHeld_ = false;
        e->accept();
        return;
    }
    QTextEdit::keyReleaseEvent(e);
}
void TerminalView::inputMethodEvent(QInputMethodEvent* e) {
    composing_ = !e->preeditString().isEmpty();
    if (!e->commitString().isEmpty())
        emit input(e->commitString().toUtf8());
    e->accept();
}
void TerminalView::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::RightButton) {
        pasteToDevice();
        e->accept();
        return;
    }
    QTextEdit::mouseReleaseEvent(e);
    if (e->button() == Qt::LeftButton && textCursor().hasSelection())
        copy();
}
void TerminalView::contextMenuEvent(QContextMenuEvent* e) {
    e->accept();
}
} // namespace portwave
