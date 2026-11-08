#pragma once
#include <QHash>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QPointer>
#include <QTabBar>
#include <functional>

namespace portwave {
class SnapChooser;
// GUI-thread controller for dragging connection tabs into seven reference
// layouts. The owner owns this object; the MDI area owns overlays/windows.
// Resolver is queried only while a tab exists. Invalid indices/cancelled drags
// leave panes unchanged. This controller never touches sessions or drafts.
class SnapLayout final : public QObject {
    Q_OBJECT
  public:
    using Resolver = std::function<QMdiSubWindow*(int)>;
    SnapLayout(QMdiArea* area, QTabBar* tabs, Resolver resolver, QObject* parent);
    ~SnapLayout() override;
    // GUI thread; registers a pane-owned title bar for subsequent drags after
    // its connection tab is hidden. Forget removes geometry only, never panes.
    void watchHeader(QWidget* header, QMdiSubWindow* window);
    void forgetWindow(QMdiSubWindow* window);
    // Explicit GUI layout request. Unknown/too-small presets return false with
    // no geometry changes; never called by a color or motion theme update.
    bool applyPreset(const QString& id);
  signals:
    // GUI-thread interaction lifetime, emitted once after drag threshold and
    // once after drop/cancel; hosts may suspend display work, never reception.
    void draggingChanged(bool dragging);
    void paneDetached(QMdiSubWindow* window);
    void paneDocked(QMdiSubWindow* window);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void moveDrag(const QPoint& global);
    void finish(bool cancel);
    void reflow();
    QRect target(int layout, int zone) const;
    bool enabled(int layout) const;
    QMdiArea* area_;
    QTabBar* tabs_;
    Resolver resolver_;
    QHash<QWidget*, QPointer<QMdiSubWindow>> headers_;
    SnapChooser* chooser_;
    QWidget* preview_;
    QPointer<QMdiSubWindow> active_;
    QHash<QMdiSubWindow*, QPair<int, int>> assignments_;
    QHash<QMdiSubWindow*, QRectF> presetAssignments_;
    QPoint press_, offset_;
    QRect original_;
    QPair<int, int> hovered_{-1, -1};
    bool dragging_{}, wasMaximized_{};
};
} // namespace portwave
