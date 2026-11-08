#include "editor/scintilla_editor.h"
#include <ILoader.h>
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QFont>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <Qsci/qscilexercpp.h>
#include <Qsci/qsciscintilla.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace portwave {
QColor appearanceColor(const QString& role, bool dark, const QColor& fallback) {
    if (!qApp || !qApp->property("portwave.theme.dark").isValid() ||
        qApp->property("portwave.theme.dark").toBool() != dark)
        return fallback;
    const auto color = qApp->property("portwave.theme.colors").toMap().value(role).value<QColor>();
    return color.isValid() ? color : fallback;
}

namespace {
using DirectFunction = std::intptr_t (*)(std::intptr_t, unsigned int, std::uintptr_t, std::intptr_t);
std::intptr_t sendWide(QsciScintillaBase* editor, unsigned int message, std::uintptr_t w = 0,
                       std::intptr_t l = 0) {
    const auto function = reinterpret_cast<DirectFunction>(
        editor->SendScintillaPtrResult(QsciScintillaBase::SCI_GETDIRECTFUNCTION));
    const auto context = reinterpret_cast<std::intptr_t>(
        editor->SendScintillaPtrResult(QsciScintillaBase::SCI_GETDIRECTPOINTER));
    return function(context, message, w, l);
}
class ScintillaLoadBuffer final : public EditorLoadBuffer {
  public:
    ScintillaLoadBuffer(ILoader* value, bool large) : loader(value), largeFile(large) {}
    ~ScintillaLoadBuffer() override {
        if (loader)
            loader->Release();
    }
    bool appendUtf8(QByteArrayView bytes, QString& error) override {
        if (!loader) {
            error = "日志文档构建器已经释放。";
            return false;
        }
        const int status = loader->AddData(bytes.data(), bytes.size());
        if (status != 0) {
            error = status == 2 ? "内存不足，无法载入日志。" : "Scintilla 构建日志文档失败。";
            return false;
        }
        return true;
    }
    ILoader* loader;
    bool largeFile;
};
} // namespace
QColor readableAnsiColor(const QColor& foreground, const QColor& background) {
    auto luminance = [](const QColor& color) {
        auto channel = [](double value) {
            return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4);
        };
        return .2126 * channel(color.redF()) + .7152 * channel(color.greenF()) +
               .0722 * channel(color.blueF());
    };
    const double base = luminance(background);
    const bool light = base > .45;
    if (!foreground.isValid())
        return QColor(light ? "#253345" : "#e5e9ef");
    auto color = foreground;
    for (int i = 0; i <= 32; ++i) {
        const double value = luminance(color);
        if ((qMax(value, base) + .05) / (qMin(value, base) + .05) >= 4.5)
            return color;
        if (i < 32)
            color = QColor((light ? color.darker(112) : color.lighter(118)).name());
    }
    return QColor(light ? "#231b28" : "#ffffff");
}
QColor displayAnsiColor(const QColor& foreground, const QColor& background, bool dark) {
    constexpr QRgb original[] = {0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x0000ee, 0xcd00cd,
                                 0x00cdcd, 0xe5e5e5, 0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00,
                                 0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff};
    QColor mapped = foreground;
    for (int i = 0; i < 16; ++i)
        if (foreground.rgb() == QColor::fromRgb(original[i]).rgb()) {
            mapped = appearanceColor(QString("ansi.%1").arg(i), dark, foreground);
            break;
        }
    return readableAnsiColor(mapped, background);
}
MarkAppearance markAppearance(int index, bool dark, const QColor& viewBackground) {
    const QStringList names{"橙色", "蓝色", "青色", "绿色", "红色", "紫色", "黄色", "灰色"};
    const QStringList lightBack{"#ffe4c5", "#dcecff", "#d5f2ee", "#e2efd7",
                                "#f9dfe3", "#eadff5", "#f8edba", "#e8e5df"};
    const QStringList lightFore{"#743800", "#174873", "#15574e", "#315426",
                                "#7c283d", "#563379", "#625010", "#47413b"};
    const QStringList darkBack{"#59442f", "#30465f", "#294b4c", "#354b38",
                               "#573940", "#493a5d", "#504b2f", "#414650"};
    const QStringList darkFore{"#edc58f", "#b5d8fb", "#a4dcd5", "#bad9a6",
                               "#f0bac3", "#d6bbee", "#e6d79b", "#d0d4dd"};
    index = qBound(0, index, 7);
    const bool backgroundDark = viewBackground.isValid() ? viewBackground.lightnessF() < .5 : dark;
    QColor back((backgroundDark ? darkBack : lightBack)[index]);
    QColor fore((backgroundDark ? darkFore : lightFore)[index]);
    if (backgroundDark == dark) {
        back = appearanceColor(QString("mark.%1").arg(index), dark, back);
        fore = appearanceColor(QString("mark.%1.foreground").arg(index), dark, fore);
    }
    return {back, readableAnsiColor(fore, back), names[index]};
}
QIcon markSwatch(int index, bool dark, const QColor& background) {
    const auto value = markAppearance(index, dark, background);
    QPixmap pixels(48, 32);
    pixels.fill(Qt::transparent);
    QPainter painter(&pixels);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(value.foreground);
    painter.setBrush(value.background);
    painter.drawRoundedRect(QRectF(1, 1, 46, 30), 4, 4);
    painter.setFont(QFont("Consolas", 11));
    painter.drawText(pixels.rect(), Qt::AlignCenter, "Aa");
    return QIcon(pixels);
}
class ScintillaEditor final : public EditorInterface {
  public:
    ScintillaEditor(QWidget* parent, bool highlight) : editor_(new QsciScintilla(parent)) {
        editor_->setUtf8(true);
        editor_->setFont(QFont("Consolas", 12));
        editor_->setMarginsFont(QFont("Consolas", 10));
        editor_->setMarginLineNumbers(0, true);
        editor_->setMarginWidth(0, "000000");
        QObject::connect(editor_, &QsciScintilla::linesChanged, editor_, [this] { updateGutter(); });
        editor_->SendScintilla(QsciScintillaBase::SCI_SETMARGINLEFT, 0UL, 4L);
        editor_->setBraceMatching(QsciScintilla::SloppyBraceMatch);
        editor_->setAutoIndent(true);
        editor_->setTabWidth(4);
        editor_->setIndentationsUseTabs(false);
        editor_->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(editor_, &QWidget::customContextMenuRequested, editor_, [this](const QPoint& point) {
            QMenu menu;
            menu.addAction("复制", editor_, &QsciScintilla::copy);
            for (const bool all : {true, false}) {
                auto* colors = menu.addMenu(all ? "用格式标记全部相同文本" : "用格式标记选定的文本");
                colors->setEnabled(editor_->hasSelectedText());
                for (int i = 0; i < 8; ++i) {
                    const auto style = markAppearance(i, dark_, background_);
                    auto* action = colors->addAction(markSwatch(i, dark_, background_),
                                                     style.name + QString("（样式 %1）").arg(i + 1));
                    action->setObjectName((all ? "markAllStyle" : "markSelectionStyle") + QString::number(i));
                    QObject::connect(action, &QAction::triggered, editor_, [this, i, all] {
                        if (all)
                            markAllSelected(i);
                        else {
                            const auto range = selection();
                            mark({{range.first, range.second - range.first, i, false}}, false);
                        }
                    });
                }
            }
            auto* clear = menu.addMenu("清除格式标记");
            for (int i = 0; i < 8; ++i) {
                auto* action = clear->addAction(markSwatch(i, dark_, background_),
                                                markAppearance(i, dark_, background_).name);
                QObject::connect(action, &QAction::triggered, editor_, [this, i] { clearMarks(i); });
            }
            clear->addAction("清除全部样式", editor_, [this] { clearMarks(); });
            menu.addAction("复制标记的文本", editor_,
                           [this] { QApplication::clipboard()->setText(markedText()); });
            menu.exec(editor_->mapToGlobal(point));
        });
        if (highlight) {
            lexer_ = new QsciLexerCPP(editor_);
            lexer_->setDefaultFont(QFont("Consolas", 12));
            editor_->setLexer(lexer_);
        }
        setTheme(true);
    }
    ~ScintillaEditor() override {
        styleTimer_.stop();
        markAllTimer_.stop();
        delete editor_;
    }
    QWidget* widget() override { return editor_; }
    void setText(const QString& text) override {
        markAllTimer_.stop();
        editor_->setProperty("markingAll", false);
        marks_.clear();
        ansiSpans_.clear();
        hasAnsiStyling_ = false;
        editor_->setText(text);
        editor_->setModified(false);
        renderAnsiStyles();
    }
    std::shared_ptr<EditorLoadBuffer> createLoadBuffer(bool largeFile, QString& error,
                                                       qsizetype expectedBytes) override {
        try {
            const auto options = QsciScintillaBase::SC_DOCUMENTOPTION_TEXT_LARGE |
                                 (largeFile ? QsciScintillaBase::SC_DOCUMENTOPTION_STYLES_NONE : 0);
            auto* loader = reinterpret_cast<ILoader*>(
                sendWide(editor_, QsciScintillaBase::SCI_CREATELOADER,
                         static_cast<std::uintptr_t>(qMax<qsizetype>(0, expectedBytes)), options));
            if (!loader) {
                error = "无法创建日志文档构建器。";
                return {};
            }
            const auto release = [](ILoader* value) { value->Release(); };
            std::unique_ptr<ILoader, decltype(release)> guard(loader, release);
            auto buffer = std::make_unique<ScintillaLoadBuffer>(loader, largeFile);
            guard.release();
            return std::shared_ptr<EditorLoadBuffer>(std::move(buffer));
        } catch (const std::bad_alloc&) {
            error = "内存不足，无法创建日志文档。";
            return {};
        }
    }
    bool adoptLoadBuffer(EditorLoadBuffer& buffer, QString& error) override {
        markAllTimer_.stop();
        editor_->setProperty("markingAll", false);
        auto* loaded = dynamic_cast<ScintillaLoadBuffer*>(&buffer);
        if (!loaded || !loaded->loader) {
            error = "日志文档构建器无效。";
            return false;
        }
        auto* document = loaded->loader->ConvertToDocument();
        if (!document) {
            error = "无法完成日志文档构建。";
            return false;
        }
        // This adapter owns one unshared view and never exports QsciDocument.
        // SETDOCPOINTER acquires the view reference before the loader is released.
        sendWide(editor_, QsciScintillaBase::SCI_SETDOCPOINTER, 0, reinterpret_cast<std::intptr_t>(document));
        loaded->loader->Release();
        loaded->loader = nullptr;
        largeFile_ = loaded->largeFile;
        marks_.clear();
        ansiSpans_.clear();
        hasAnsiStyling_ = false;
        sendWide(editor_, QsciScintillaBase::SCI_SETUNDOCOLLECTION, 0);
        sendWide(editor_, QsciScintillaBase::SCI_EMPTYUNDOBUFFER);
        sendWide(editor_, QsciScintillaBase::SCI_SETSAVEPOINT);
        editor_->setUtf8(true);
        editor_->setReadOnly(true);
        setWrap(wrapRequested_);
        updateGutter();
        renderAnsiStyles();
        return true;
    }
    bool appendLogUtf8(QByteArrayView bytes, const QVector<EditorAnsiSpan>& styles, QString& error) override {
        if (bytes.isEmpty())
            return true;
        try {
            if (ansiSpans_.capacity() - ansiSpans_.size() < styles.size())
                ansiSpans_.reserve(qMax(ansiSpans_.size() + styles.size(), ansiSpans_.capacity() * 2));
        } catch (const std::bad_alloc&) {
            error = "内存不足，无法追加日志颜色；已有内容保留。";
            return false;
        }
        const auto view = viewState();
        const auto anchor = sendWide(editor_, QsciScintillaBase::SCI_GETANCHOR);
        const auto current = sendWide(editor_, QsciScintillaBase::SCI_GETCURRENTPOS);
        const auto start = sendWide(editor_, QsciScintillaBase::SCI_GETLENGTH);
        const bool readOnly = editor_->isReadOnly();
        editor_->setReadOnly(false);
        sendWide(editor_, QsciScintillaBase::SCI_APPENDTEXT, bytes.size(),
                 reinterpret_cast<std::intptr_t>(bytes.data()));
        const auto added = sendWide(editor_, QsciScintillaBase::SCI_GETLENGTH) - start;
        if (added != bytes.size()) {
            if (added > 0)
                sendWide(editor_, QsciScintillaBase::SCI_DELETERANGE, start, added);
            editor_->setReadOnly(readOnly);
            error = "内存不足，无法追加日志；已有内容保留。";
            return false;
        }
        // Scintilla may extend the preceding style/indicator into inserted text.
        // Reset just the appended range, never the old document or its markers.
        if (!largeFile_) {
            sendWide(editor_, QsciScintillaBase::SCI_STARTSTYLING, start);
            sendWide(editor_, QsciScintillaBase::SCI_SETSTYLING, added, 0);
        }
        for (int indicator = 8; indicator < 32; ++indicator) {
            sendWide(editor_, QsciScintillaBase::SCI_SETINDICATORCURRENT, indicator);
            sendWide(editor_, QsciScintillaBase::SCI_INDICATORCLEARRANGE, start, added);
        }
        ansiSpans_ += styles;
        renderAnsiStyles(false);
        sendWide(editor_, QsciScintillaBase::SCI_SETSEL, anchor, current);
        sendWide(editor_, QsciScintillaBase::SCI_SETSAVEPOINT);
        editor_->setReadOnly(readOnly);
        updateGutter();
        restoreViewState(view, false);
        return true;
    }
    QString text() const override { return editor_->text(); }
    bool find(const QString& q, bool re, bool cs, bool forward) override {
        return editor_->findFirst(q, re, cs, false, true, forward);
    }
    void setReadOnly(bool value) override { editor_->setReadOnly(value); }
    void setWrap(bool value) override {
        wrapRequested_ = value;
        editor_->setWrapMode(value && !largeFile_ ? QsciScintilla::WrapWord : QsciScintilla::WrapNone);
    }
    void goToLine(int line) override {
        editor_->setCursorPosition(qMax(0, line), 0);
        editor_->ensureLineVisible(qMax(0, line));
    }
    qsizetype position() const override {
        return sendWide(editor_, QsciScintillaBase::SCI_COUNTCODEUNITS, 0,
                        sendWide(editor_, QsciScintillaBase::SCI_GETCURRENTPOS));
    }
    std::pair<qsizetype, qsizetype> selection() const override {
        const auto begin = sendWide(editor_, QsciScintillaBase::SCI_GETSELECTIONSTART);
        const auto end = sendWide(editor_, QsciScintillaBase::SCI_GETSELECTIONEND);
        return {sendWide(editor_, QsciScintillaBase::SCI_COUNTCODEUNITS, 0, begin),
                sendWide(editor_, QsciScintillaBase::SCI_COUNTCODEUNITS, 0, end)};
    }
    void selectRange(qsizetype position, qsizetype length) override {
        const auto total = sendWide(editor_, QsciScintillaBase::SCI_COUNTCODEUNITS, 0,
                                    sendWide(editor_, QsciScintillaBase::SCI_GETLENGTH));
        position = qBound<qsizetype>(0, position, total);
        length = qBound<qsizetype>(0, length, total - position);
        const auto begin = sendWide(editor_, QsciScintillaBase::SCI_POSITIONRELATIVECODEUNITS, 0, position);
        const auto end = sendWide(editor_, QsciScintillaBase::SCI_POSITIONRELATIVECODEUNITS,
                                  static_cast<std::uintptr_t>(begin), length);
        sendWide(editor_, QsciScintillaBase::SCI_SETSEL, static_cast<std::uintptr_t>(begin), end);
        editor_->ensureCursorVisible();
    }
    void setNumbers(bool enabled) override {
        numbers_ = enabled;
        editor_->setMarginLineNumbers(0, enabled);
        updateGutter();
    }
    void setAppearance(const QFont& font, const QColor& background, const QColor& foreground) override {
        font_ = font;
        background_ = background;
        foreground_ = foreground;
        editor_->setFont(font);
        editor_->setMarginsFont(font);
        editor_->setStyleSheet(QString("QsciScintilla {font-family:'%1';font-size:%2pt;border:0;}")
                                   .arg(font.family())
                                   .arg(font.pointSizeF()));
        editor_->setPaper(background);
        editor_->setColor(foreground);
        editor_->setCaretForegroundColor(foreground);
        updateGutter();
        if (lexer_) {
            lexer_->setDefaultFont(font);
            lexer_->setDefaultPaper(background);
            lexer_->setPaper(background, -1);
        }
        renderAnsiStyles();
        renderMarks();
    }
    void setAnsiStyles(const QVector<EditorAnsiSpan>& styles, bool enabled) override {
        ansiSpans_ = styles;
        // Keep the editor's growing style cache independent from the feature
        // model, avoiding a copy of all previous spans at every tail append.
        ansiSpans_.detach();
        ansiEnabled_ = enabled;
        renderAnsiStyles();
    }
    void highlightMatches(const QVector<TextMark>& matches, bool append) override {
        searchOverlayDefined_ = true;
        editor_->indicatorDefine(QsciScintilla::RoundBoxIndicator, 16);
        editor_->setIndicatorForegroundColor(appearanceColor("search.match", dark_, QColor("#b87936")), 16);
        if (!dark_) {
            editor_->SendScintilla(QsciScintillaBase::SCI_INDICSETALPHA, 16UL, 170L);
            editor_->SendScintilla(QsciScintillaBase::SCI_INDICSETOUTLINEALPHA, 16UL, 220L);
        }
        editor_->setIndicatorDrawUnder(true, 16);
        sendWide(editor_, QsciScintillaBase::SCI_SETINDICATORCURRENT, 16);
        if (!append) {
            sendWide(editor_, QsciScintillaBase::SCI_INDICATORCLEARRANGE, 0,
                     sendWide(editor_, QsciScintillaBase::SCI_GETLENGTH));
            matchUnit_ = 0;
            matchByte_ = 0;
        }
        auto sorted = matches;
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.position < b.position; });
        auto& previous = matchUnit_;
        auto& bytePosition = matchByte_;
        for (const auto& match : sorted) {
            if (match.position < previous || match.length <= 0)
                continue;
            bytePosition = sendWide(editor_, QsciScintillaBase::SCI_POSITIONRELATIVECODEUNITS,
                                    static_cast<std::uintptr_t>(bytePosition), match.position - previous);
            if (bytePosition < 0)
                break;
            const auto end = sendWide(editor_, QsciScintillaBase::SCI_POSITIONRELATIVECODEUNITS,
                                      static_cast<std::uintptr_t>(bytePosition), match.length);
            if (end < bytePosition)
                break;
            sendWide(editor_, QsciScintillaBase::SCI_INDICATORFILLRANGE,
                     static_cast<std::uintptr_t>(bytePosition), end - bytePosition);
            previous = match.position;
        }
    }
    EditorViewState viewState() const override {
        EditorViewState state;
        editor_->getCursorPosition(&state.line, &state.column);
        state.lineCount = editor_->lines();
        state.firstVisibleLine = editor_->SendScintilla(QsciScintillaBase::SCI_GETFIRSTVISIBLELINE);
        state.horizontalOffset = editor_->SendScintilla(QsciScintillaBase::SCI_GETXOFFSET);
        return state;
    }
    void restoreViewState(const EditorViewState& state, bool cursor) override {
        if (cursor)
            editor_->setCursorPosition(qBound(0, state.line, qMax(0, editor_->lines() - 1)),
                                       qMax(0, state.column));
        editor_->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE, qMax(0, state.firstVisibleLine));
        editor_->SendScintilla(QsciScintillaBase::SCI_SETXOFFSET, qMax(0, state.horizontalOffset));
    }
    void mark(const QVector<TextMark>& marks, bool clearPrevious) override {
        if (clearPrevious)
            marks_.clear();
        for (auto mark : marks)
            if (mark.length > 0 && marks_.size() < 50000) {
                mark.color = qBound(0, mark.color, 7);
                marks_.push_back(mark);
            }
        renderMarks();
    }
    void clearMarks(int color = -1) override {
        markAllTimer_.stop();
        editor_->setProperty("markingAll", false);
        if (color < 0)
            marks_.clear();
        else
            marks_.erase(std::remove_if(marks_.begin(), marks_.end(),
                                        [color](const TextMark& mark) { return mark.color == color; }),
                         marks_.end());
        renderMarks();
    }
    QVector<TextMark> marks() const override { return marks_; }
    QString markedText() const override {
        auto source = editor_->text();
        auto marks = marks_;
        std::sort(marks.begin(), marks.end(),
                  [](const TextMark& a, const TextMark& b) { return a.position < b.position; });
        QStringList result;
        qsizetype end = 0;
        for (const auto& mark : marks) {
            auto start = qMax(end, mark.position);
            auto limit = qMin(source.size(), mark.position + mark.length);
            if (limit > start) {
                result.push_back(source.mid(start, limit - start));
                end = limit;
            }
        }
        return result.join('\n');
    }
    void setTheme(bool dark) override {
        dark_ = dark;
        const auto bg = appearanceColor("surface", dark, QColor(dark ? "#171b20" : "#ffffff"));
        const auto fg = appearanceColor("text", dark, QColor(dark ? "#e5e9ef" : "#253345"));
        const auto shell = appearanceColor("shell", dark, QColor(dark ? "#202327" : "#ffffff"));
        editor_->setPaper(bg);
        editor_->setColor(fg);
        editor_->setMarginsBackgroundColor(shell);
        editor_->setMarginsForegroundColor(
            appearanceColor("text.muted", dark, dark ? QColor("#9ca7b6") : QColor("#66758a")));
        editor_->setCaretForegroundColor(fg);
        editor_->setCaretLineBackgroundColor(
            appearanceColor("editor.currentLine", dark, dark ? QColor("#303640") : QColor("#eee8d5")));
        editor_->setCaretLineVisible(true);
        editor_->SendScintilla(QsciScintillaBase::SCI_SETCARETLINEVISIBLEALWAYS, 1UL);
        editor_->setSelectionBackgroundColor(
            appearanceColor(dark ? "selection.background" : "selection.textBackground", dark,
                            dark ? QColor("#2a3c4c") : QColor("#0052cc")));
        editor_->setSelectionForegroundColor(dark ? fg : QColor(Qt::white));
        // Light mode uses white selected text; chromatic retains ANSI foregrounds.
        editor_->SendScintilla(QsciScintillaBase::SCI_SETSELFORE, dark ? 0UL : 1UL, dark ? 0L : 0xffffffL);
        editor_->setIndicatorForegroundColor(appearanceColor("search.match", dark, QColor("#b87936")), 16);
        editor_->SendScintilla(QsciScintillaBase::SCI_INDICSETALPHA, 16UL,
                               dark ? (searchOverlayDefined_ ? 255L : 30L) : 170L);
        editor_->SendScintilla(QsciScintillaBase::SCI_INDICSETOUTLINEALPHA, 16UL, dark ? 50L : 220L);
        if (lexer_) {
            lexer_->setDefaultPaper(bg);
            lexer_->setDefaultColor(fg);
            lexer_->setPaper(bg, -1);
            lexer_->setColor(fg, -1);
            lexer_->setColor(
                appearanceColor("editor.keyword", dark, dark ? QColor("#c678dd") : QColor("#0067c0")),
                QsciLexerCPP::Keyword);
            lexer_->setColor(
                appearanceColor("editor.comment", dark, dark ? QColor("#98c379") : QColor("#25753b")),
                QsciLexerCPP::CommentLine);
            lexer_->setColor(
                appearanceColor("editor.string", dark, dark ? QColor("#e5c07b") : QColor("#ae7300")),
                QsciLexerCPP::DoubleQuotedString);
        }
        if (font_.family().isEmpty()) {
            font_ = QFont("Consolas", 12);
            background_ = bg;
            foreground_ = fg;
        }
        renderAnsiStyles();
        renderMarks();
    }

  private:
    void updateGutter() {
        editor_->setMarginWidth(0, numbers_ ? 26 + QFontMetrics(font_).horizontalAdvance('9') *
                                                       QString::number(qMax(1, editor_->lines())).size()
                                            : 0);
    }
    void renderAnsiStyles(bool reset = true) {
        styleTimer_.stop();
        styleTimer_.disconnect();
        if (!editor_ || lexer_)
            return;
        if (reset) {
            ansiStyleIds_.clear();
            nextAnsiStyle_ = 1;
            ansiNextSpan_ = ansiUnit_ = 0;
            ansiByte_ = 0;
        }
        editor_->SendScintilla(QsciScintillaBase::SCI_SETSTYLEBITS, 8UL);
        auto setStyle = [this](int style, const QColor& foreground, const QColor& background, bool bold) {
            editor_->SendScintilla(QsciScintillaBase::SCI_STYLESETFORE, static_cast<unsigned long>(style),
                                   foreground);
            editor_->SendScintilla(QsciScintillaBase::SCI_STYLESETBACK, static_cast<unsigned long>(style),
                                   background);
            editor_->SendScintilla(QsciScintillaBase::SCI_STYLESETBOLD, static_cast<unsigned long>(style),
                                   bold ? 1L : 0L);
            editor_->SendScintilla(QsciScintillaBase::SCI_STYLESETFONT, static_cast<std::uintptr_t>(style),
                                   font_.family().toUtf8().constData());
            editor_->SendScintilla(QsciScintillaBase::SCI_STYLESETSIZE, static_cast<unsigned long>(style),
                                   static_cast<long>(font_.pointSize()));
        };
        setStyle(0, foreground_, background_, false);
        setStyle(QsciScintillaBase::STYLE_DEFAULT, foreground_, background_, false);
        setStyle(QsciScintillaBase::STYLE_LINENUMBER,
                 appearanceColor("text.muted", dark_, QColor(dark_ ? "#9ca7b6" : "#66758a")), background_,
                 false);
        if (largeFile_)
            return; // STYLES_NONE: never allocate a full-text QString for styling.
        if (!ansiEnabled_ || ansiSpans_.isEmpty()) {
            if (!hasAnsiStyling_ || !reset) {
                const auto end = sendWide(editor_, QsciScintillaBase::SCI_GETLENGTH);
                ansiUnit_ += sendWide(editor_, QsciScintillaBase::SCI_COUNTCODEUNITS, ansiByte_, end);
                ansiByte_ = end;
                ansiNextSpan_ = ansiSpans_.size();
                return;
            }
            hasAnsiStyling_ = false;
            editor_->SendScintilla(QsciScintillaBase::SCI_STARTSTYLING, 0UL, 0xffUL);
            editor_->SendScintilla(
                QsciScintillaBase::SCI_SETSTYLING,
                static_cast<unsigned long>(sendWide(editor_, QsciScintillaBase::SCI_GETLENGTH)), 0L);
            return;
        }
        hasAnsiStyling_ = true;
        // Keep UTF-16 coordinates in the model; translate incrementally inside
        // Scintilla, without making QString/UTF-8 copies of the entire document.
        if (reset && !std::is_sorted(ansiSpans_.cbegin(), ansiSpans_.cend(),
                                     [](const auto& a, const auto& b) { return a.position < b.position; }))
            std::sort(ansiSpans_.begin(), ansiSpans_.end(),
                      [](const auto& a, const auto& b) { return a.position < b.position; });
        const auto process = [this, setStyle] {
            QElapsedTimer budget;
            budget.start();
            while (ansiNextSpan_ < ansiSpans_.size()) {
                const auto& span = ansiSpans_[ansiNextSpan_++];
                if (span.length <= 0 || span.position < ansiUnit_)
                    continue;
                const auto start = sendWide(editor_, QsciScintillaBase::SCI_POSITIONRELATIVECODEUNITS,
                                            ansiByte_, span.position - ansiUnit_);
                if (start < ansiByte_)
                    continue;
                const auto end =
                    sendWide(editor_, QsciScintillaBase::SCI_POSITIONRELATIVECODEUNITS, start, span.length);
                if (end < start)
                    continue;
                const QColor fore = span.foreground >= 0 ? QColor::fromRgb(span.foreground) : foreground_;
                const QColor back = span.background >= 0 ? QColor::fromRgb(span.background) : background_;
                const auto key = QString("%1/%2/%3").arg(fore.rgba()).arg(back.rgba()).arg(span.bold);
                int style = ansiStyleIds_.value(key, -1);
                if (style < 0) {
                    if (nextAnsiStyle_ == 32)
                        nextAnsiStyle_ = 40;
                    style = nextAnsiStyle_ <= 255 ? nextAnsiStyle_++ : 0;
                    ansiStyleIds_.insert(key, style);
                    if (style)
                        setStyle(style, displayAnsiColor(fore, back, dark_), back, span.bold);
                }
                sendWide(editor_, QsciScintillaBase::SCI_STARTSTYLING, ansiByte_);
                if (start > ansiByte_)
                    sendWide(editor_, QsciScintillaBase::SCI_SETSTYLING, start - ansiByte_, 0);
                sendWide(editor_, QsciScintillaBase::SCI_SETSTYLING, end - start, style);
                ansiByte_ = end;
                ansiUnit_ = span.position + span.length;
                if (budget.elapsed() >= 4)
                    break;
            }
            if (ansiNextSpan_ == ansiSpans_.size()) {
                const auto end = sendWide(editor_, QsciScintillaBase::SCI_GETLENGTH);
                const auto tail = end - ansiByte_;
                sendWide(editor_, QsciScintillaBase::SCI_STARTSTYLING, ansiByte_);
                if (tail > 0)
                    sendWide(editor_, QsciScintillaBase::SCI_SETSTYLING, tail, 0);
                ansiUnit_ += sendWide(editor_, QsciScintillaBase::SCI_COUNTCODEUNITS, ansiByte_, end);
                ansiByte_ = end;
                styleTimer_.stop();
            }
        };
        QObject::connect(&styleTimer_, &QTimer::timeout, editor_, process);
        styleTimer_.start(1);
        process(); // Small files and the first visible lines are styled immediately.
    }
    void markAllSelected(int color) {
        const auto needle = editor_->selectedText().toUtf8();
        if (needle.isEmpty())
            return;
        markAllTimer_.stop();
        markAllTimer_.disconnect();
        struct Job {
            QByteArray needle;
            QVector<TextMark> marks;
            std::intptr_t next{}, byte{};
            qsizetype unit{};
        };
        auto job = std::make_shared<Job>();
        job->needle = needle;
        editor_->setProperty("markingAll", true);
        const auto process = [this, job, color] {
            QElapsedTimer budget;
            budget.start();
            bool done = false;
            while (budget.elapsed() < 4 && job->marks.size() < 50000) {
                sendWide(editor_, QsciScintillaBase::SCI_SETSEARCHFLAGS, QsciScintillaBase::SCFIND_MATCHCASE);
                sendWide(editor_, QsciScintillaBase::SCI_SETTARGETSTART, job->next);
                const auto total = sendWide(editor_, QsciScintillaBase::SCI_GETLENGTH);
                const auto endOfChunk =
                    qMin<std::intptr_t>(total, job->next + 256 * 1024 + job->needle.size() - 1);
                sendWide(editor_, QsciScintillaBase::SCI_SETTARGETEND, endOfChunk);
                const auto start =
                    sendWide(editor_, QsciScintillaBase::SCI_SEARCHINTARGET, job->needle.size(),
                             reinterpret_cast<std::intptr_t>(job->needle.constData()));
                if (start < 0) {
                    if (endOfChunk == total) {
                        done = true;
                        break;
                    }
                    const auto next =
                        sendWide(editor_, QsciScintillaBase::SCI_POSITIONBEFORE, job->next + 256 * 1024);
                    job->unit += sendWide(editor_, QsciScintillaBase::SCI_COUNTCODEUNITS, job->byte, next);
                    job->byte = next;
                    job->next = next;
                    continue;
                }
                const auto end = sendWide(editor_, QsciScintillaBase::SCI_GETTARGETEND);
                job->unit += sendWide(editor_, QsciScintillaBase::SCI_COUNTCODEUNITS, job->byte, start);
                const auto length = sendWide(editor_, QsciScintillaBase::SCI_COUNTCODEUNITS, start, end);
                job->marks.push_back({job->unit, length, color, false});
                job->byte = start;
                job->next = end;
            }
            if (done || job->marks.size() >= 50000) {
                markAllTimer_.stop();
                mark(job->marks, false);
                editor_->setProperty("markingAll", false);
            }
        };
        QObject::connect(&markAllTimer_, &QTimer::timeout, editor_, process);
        markAllTimer_.start(1);
        process();
    }
    void renderMarks() {
        const auto bytes = sendWide(editor_, QsciScintillaBase::SCI_GETLENGTH);
        for (int i = 0; i < 8; ++i) {
            const auto style = markAppearance(i, dark_, background_);
            editor_->indicatorDefine(QsciScintilla::FullBoxIndicator, i + 8);
            editor_->setIndicatorForegroundColor(style.background, i + 8);
            editor_->setIndicatorDrawUnder(true, i + 8);
            sendWide(editor_, QsciScintillaBase::SCI_INDICSETALPHA, i + 8, 255);
            sendWide(editor_, QsciScintillaBase::SCI_INDICSETOUTLINEALPHA, i + 8, 255);
            editor_->indicatorDefine(QsciScintilla::TextColorIndicator, i + 24);
            editor_->setIndicatorForegroundColor(style.foreground, i + 24);
            for (int indicator : {i + 8, i + 24}) {
                sendWide(editor_, QsciScintillaBase::SCI_SETINDICATORCURRENT, indicator);
                sendWide(editor_, QsciScintillaBase::SCI_INDICATORCLEARRANGE, 0, bytes);
            }
        }
        auto marks = marks_;
        std::sort(marks.begin(), marks.end(),
                  [](const auto& a, const auto& b) { return a.position < b.position; });
        qsizetype previous = 0;
        std::intptr_t byte = 0;
        for (const auto& item : marks) {
            if (item.position < previous || item.length <= 0)
                continue;
            const auto start = sendWide(editor_, QsciScintillaBase::SCI_POSITIONRELATIVECODEUNITS, byte,
                                        item.position - previous);
            if (start < 0)
                continue;
            const auto end =
                sendWide(editor_, QsciScintillaBase::SCI_POSITIONRELATIVECODEUNITS, start, item.length);
            if (end < start)
                continue;
            for (int indicator : {item.color + 8, item.color + 24}) {
                sendWide(editor_, QsciScintillaBase::SCI_SETINDICATORCURRENT, indicator);
                sendWide(editor_, QsciScintillaBase::SCI_INDICATORFILLRANGE, start, end - start);
            }
            byte = start;
            previous = item.position;
        }
    }
    QPointer<QsciScintilla> editor_;
    QsciLexerCPP* lexer_{};
    QVector<TextMark> marks_;
    QVector<EditorAnsiSpan> ansiSpans_;
    QHash<QString, int> ansiStyleIds_;
    int nextAnsiStyle_{1};
    qsizetype ansiNextSpan_{}, ansiUnit_{};
    std::intptr_t ansiByte_{};
    QTimer styleTimer_, markAllTimer_;
    QFont font_;
    QColor background_, foreground_;
    bool ansiEnabled_{};
    bool searchOverlayDefined_{};
    bool hasAnsiStyling_{};
    bool numbers_{true};
    bool dark_ = true;
    bool largeFile_{}, wrapRequested_{};
    qsizetype matchUnit_{};
    std::intptr_t matchByte_{};
};
std::unique_ptr<EditorInterface> makeEditor(QWidget* parent, bool highlight) {
    return std::make_unique<ScintillaEditor>(parent, highlight);
}
} // namespace portwave
