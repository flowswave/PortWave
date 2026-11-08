#pragma once
#include <QHash>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include <QVector>

class QMdiArea;
class QMdiSubWindow;
class QWidget;

namespace portwave {
// GUI-thread split handles for adjacent MDI panes. When the pointer rests on the
// shared edge of neighbouring panes for one second, a three-dot handle appears.
// Panes that share the same edge coordinate are grouped, so one handle can push a
// whole side (for example the two right panes of a 1:2 layout) with equal deltas
// instead of leaving a step between them. Handles are pure view state: they never
// touch sessions, drafts, logging or persisted configuration, and they hide as
// soon as the pointer leaves the edge.
class SplitGrip final : public QObject {
    Q_OBJECT
  public:
    explicit SplitGrip(QMdiArea* area, QObject* parent = nullptr);
    ~SplitGrip() override;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    // One shared edge, after merging panes that occupy the same coordinate.
    struct Edge {
        bool vertical{};                           // true: edge is vertical, drag changes widths
        int boundary{};                            // x (vertical) or y (horizontal), viewport space
        int offset{};                              // boundary minus leading pane edge at find time
        int from{}, to{};                          // span along the edge, viewport space
        QVector<QPointer<QMdiSubWindow>> leading;  // panes on the left / top side
        QVector<QPointer<QMdiSubWindow>> trailing; // panes on the right / bottom side
    };
    QVector<QMdiSubWindow*> panes() const;
    QRect paneRect(QMdiSubWindow* window) const;
    QVector<Edge> edges() const;
    Edge findEdge(const QPoint& position) const;
    int currentBoundary(const Edge& edge) const;
    void track(const QPoint& position);
    void showGrip();
    void hideGrip();
    void positionGrip();
    void beginDrag(const QPoint& position);
    void dragTo(const QPoint& position);
    void finishDrag(bool cancel, const QPoint& position);

    QMdiArea* area_;
    QWidget* grip_{};
    QTimer dwell_;
    Edge hovered_;
    QHash<QMdiSubWindow*, QRect> start_;
    QPoint dragOrigin_;
    bool dragging_{};
};
} // namespace portwave
