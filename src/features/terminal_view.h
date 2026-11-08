#pragma once
#include "core/ansi.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QStringDecoder>
#include <QTextEdit>
#include <QTimer>

namespace portwave {
class TerminalView final : public QTextEdit {
    Q_OBJECT
  public:
    explicit TerminalView(QWidget* parent = nullptr);
    void consume(std::string_view bytes);
    void setTheme(bool dark);
    // GUI-only receive pause affordance. Keyboard resume never emits TX bytes.
    void setReceivePaused(bool paused) { receivePaused_ = paused; }
    // GUI-only disconnected affordance: Space/Return asks the pane to reconnect
    // instead of emitting TX bytes. Only set after this session has been connected.
    void setDisconnected(bool disconnected) { disconnected_ = disconnected; }
    bool disconnected() const { return disconnected_; }
    // GUI-thread view colors; invalid colors are ignored. Recolors existing spans
    // in bounded batches without changing document text, selection or raw ANSI.
    void setDisplayColors(const QColor& background, const QColor& foreground);
    void setDisplayMode(int mode) { mode_ = mode; }
    void setNewline(bool value) { newline_ = value; }
    void setTimestampOptions(bool enabled, int intervalSeconds) {
        timestampEnabled_ = enabled;
        timestampIntervalSeconds_ = qMax(0, intervalSeconds);
    }
    void setScrollMode(int mode) { scrollMode_ = qBound(0, mode, 2); }
    // GUI-thread display-only resynchronization after intentionally skipping
    // receive backlog. Preserve existing text; reset partial ANSI/UTF-8 state.
    void resumeLatest(std::string_view bytes, bool skipped);
    void resetTerminal();
    void followEnd();
  signals:
    void input(const QByteArray& bytes);
    void resumeReceiveRequested();
    void reconnectRequested();

  protected:
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

  private:
    void pasteToDevice();
    QColor readableColor(int rgb);
    void recolor();
    AnsiDecoder decoder_;
    int mode_{}, column_{};
    QColor displayBackground_;
    bool receivePaused_{}, resumeKeyHeld_{}, composing_{}, disconnected_{};
    bool dark_ = true, newline_ = true;
    bool timestampEnabled_{};
    int timestampIntervalSeconds_{};
    int scrollMode_{};
    QDateTime lastTimestamp_;
    QElapsedTimer downPress_;
    QTimer recolorTimer_;
    int recolorPosition_{};
    QHash<int, QColor> colorCache_;
};
} // namespace portwave
