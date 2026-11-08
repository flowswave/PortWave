#pragma once
#include <QTabBar>
#include <QToolButton>
#include <QVariantAnimation>

namespace portwave {
// GUI-thread visual-only tabs. Selection signals are immediate. Owns the
// fixed 150 ms smooth transition and indicator; no session/configuration access.
class ConnectionTabs final : public QTabBar {
    Q_OBJECT
  public:
    explicit ConnectionTabs(QWidget* parent = nullptr);
    // GUI-only: force current geometry after blocked signals/structural changes.
    // Coalesces one final layout pass; owns no session or external state.
    void synchronize();
    // GUI-only: floating active panes have no selected docked tab.
    void setSelectionVisible(bool visible);
    // GUI-only: the row keeps a tab-sized height with no tab open, so the fixed-size
    // "+" button stays centred instead of being clipped against the toolbar.
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
  signals:
    void newConnectionRequested();

  protected:
    void tabInserted(int index) override;
    void tabRemoved(int index) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void tabLayoutChange() override;
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void updateIndicator(bool animate);
    // GUI-only: height of one tab slot; measured from a tab when present, otherwise
    // estimated from the current style. Never smaller than the "+" button plus insets.
    int tabSlotHeight() const;
    void advance(qreal progress);
    QToolButton* add_{};
    QWidget* indicator_{};
    QVariantAnimation* animation_{};
    QRectF start_, target_;
    QVector<qreal> weights_, startWeights_;
    int displayedIndex_ = -1;
    bool structurePending_{}, syncQueued_{}, selectionVisible_{true};
};
} // namespace portwave
