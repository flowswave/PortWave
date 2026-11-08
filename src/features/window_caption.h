#pragma once
#include <QIcon>
#include <QLabel>
#include <QPointer>
#include <QToolButton>
#include <QWidget>

namespace portwave {
// GUI-thread caption. Owner outlives this child. Main-window pinning is global;
// 32px log captions pin only above the serial owner and persist that preference.
// Moving, resizing and pinning never change document or session state.
class WindowCaption final : public QWidget {
    Q_OBJECT
  public:
    explicit WindowCaption(QWidget* window, const QString& title, int captionHeight = 38);
    // GUI-thread reparenting of a borrowed command bar. Caller retains its
    // lifetime and moves it back to its normal layout after passing nullptr.
    void setCommandBar(QWidget* commands);

  protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    QPointer<QWidget> window_;
    QPointer<QWidget> relativeMain_;
    QPointer<QWidget> commands_;
    QLabel* title_;
    QToolButton* pin_;
    bool relativePin_{}, raisePending_{};
    void queueRaise();
};
// GUI-thread icon factory shared by captions, file tabs, and the send pane.
QIcon pinIcon(bool checked = false);
QIcon documentIcon();
QIcon folderIcon();
// GUI-thread resize hit frame owned by window. Only five-pixel edge strips
// receive mouse events; centre remains untouched. No decorative active outline.
QWidget* decorateWindow(QWidget* window);
// GUI-thread, borrowed top-level window. Constrain normal geometry to the screen's
// available area; valid explicit area supports deterministic layout validation.
// Preserves fitting geometry, maximized/minimized state and minimum size constraints.
// No persistence or content access; returns silently for null/non-window arguments.
void fitWindowToScreen(QWidget* window, const QRect& available = {});
QIcon sendControlIcon(int kind, const QColor& color);
} // namespace portwave
