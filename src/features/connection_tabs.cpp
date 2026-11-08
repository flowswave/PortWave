#include "features/connection_tabs.h"
#include <QApplication>
#include <QPainter>
#include <QResizeEvent>
#include <QStyle>
#include <QStyleOptionTab>
#include <QStylePainter>
#include <QTimer>
#include <cmath>

namespace portwave {
namespace {
// Horizontal gap between the last tab and the "+" button, and the minimum breathing
// room the button keeps from the row edges when no tab sizes the row.
constexpr int kAddButtonGap = 8;
QColor token(const char* key, QColor fallback) {
    return qApp->property("portwave.theme.colors").toMap().value(key, fallback).value<QColor>();
}
QColor blend(QColor a, QColor b, qreal t) {
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}
} // namespace
ConnectionTabs::ConnectionTabs(QWidget* parent) : QTabBar(parent) {
    add_ = new QToolButton(this);
    add_->setObjectName("addConnectionTab");
    add_->setText("+");
    add_->setToolTip("新建连接");
    add_->setFixedSize(36, 34);
    connect(add_, &QToolButton::clicked, this, &ConnectionTabs::newConnectionRequested);
    indicator_ = new QWidget(this);
    indicator_->setObjectName("connectionTabIndicator");
    indicator_->setAttribute(Qt::WA_TransparentForMouseEvents);
    indicator_->setAttribute(Qt::WA_NoSystemBackground);
    indicator_->hide();
    animation_ = new QVariantAnimation(this);
    animation_->setDuration(150);
    animation_->setEasingCurve(QEasingCurve::Linear);
    animation_->setStartValue(0.0);
    animation_->setEndValue(1.0);
    connect(animation_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) { advance(value.toReal()); });
    connect(this, &QTabBar::currentChanged, this, [this] { updateIndicator(!structurePending_); });
    connect(this, &QTabBar::tabMoved, this, [this] { updateIndicator(false); });
}
void ConnectionTabs::setSelectionVisible(bool visible) {
    if (selectionVisible_ == visible)
        return;
    selectionVisible_ = visible;
    updateIndicator(false);
}
void ConnectionTabs::synchronize() {
    structurePending_ = true;
    updateIndicator(false);
    if (syncQueued_)
        return;
    syncQueued_ = true;
    QTimer::singleShot(0, this, [this] {
        syncQueued_ = false;
        updateIndicator(false);
        structurePending_ = false;
    });
}
void ConnectionTabs::tabInserted(int index) {
    QTabBar::tabInserted(index);
    if (animation_)
        synchronize();
}
void ConnectionTabs::tabRemoved(int index) {
    QTabBar::tabRemoved(index);
    if (animation_)
        synchronize();
}
void ConnectionTabs::advance(qreal t) {
    const qreal eased = 1.0 - std::pow(1.0 - t, 3.0);
    qreal left = start_.left() + (target_.left() - start_.left()) * eased;
    qreal right = start_.right() + (target_.right() - start_.right()) * eased;
    indicator_->setGeometry(
        QRectF(QPointF(left, target_.top()), QPointF(right, target_.bottom())).toAlignedRect());
    weights_.resize(count());
    for (int i = 0; i < count(); ++i)
        weights_[i] = startWeights_.value(i) * (1 - eased) + (i == currentIndex() ? eased : 0);
    update();
}
void ConnectionTabs::paintEvent(QPaintEvent*) {
    QStylePainter painter(this);
    for (int i = 0; i < count(); ++i) {
        if (!isTabVisible(i))
            continue;
        QStyleOptionTab option;
        initStyleOption(&option, i);
        option.state &= ~QStyle::State_Selected;
        painter.drawControl(QStyle::CE_TabBarTabShape, option);
    }
    if (indicator_->isVisible() && currentIndex() >= 0 && isTabVisible(currentIndex())) {
        painter.setRenderHint(QPainter::Antialiasing);
        const auto active = token("surface.active", palette().color(QPalette::Highlight));
        if (qApp->property("portwave.theme.id").toString() == "chromatic") {
            QLinearGradient fill(0, 0, 0, height());
            fill.setColorAt(0, active.lighter(135));
            fill.setColorAt(1, active.darker(120));
            painter.setBrush(fill);
        } else
            painter.setBrush(active);
        painter.setPen(QPen(palette().color(QPalette::Accent), 1.2));
        painter.drawRoundedRect(QRectF(indicator_->geometry()).adjusted(.6, .6, -.6, -.6), 7, 7);
    }
    const auto inactive = token("text.secondary", palette().color(QPalette::WindowText));
    const auto activeText = token("text", palette().color(QPalette::WindowText));
    for (int i = 0; i < count(); ++i) {
        if (!isTabVisible(i))
            continue;
        QStyleOptionTab option;
        initStyleOption(&option, i);
        option.state &= ~QStyle::State_Selected;
        const auto color = blend(inactive, activeText, weights_.value(i));
        for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
            option.palette.setColor(role, color);
        painter.drawControl(QStyle::CE_TabBarTabLabel, option);
    }
}
void ConnectionTabs::updateIndicator(bool animate) {
    animation_->stop();
    int right = 0;
    for (int i = 0; i < count(); ++i)
        if (isTabVisible(i))
            right = qMax(right, tabRect(i).right());
    add_->setVisible(isVisible());
    const int top = qMax(0, (qMax(height(), tabSlotHeight()) - add_->height()) / 2);
    add_->move(qMax(0, qMin(width() - add_->width(), right + kAddButtonGap)), top);
    const int index = currentIndex();
    if (!selectionVisible_ || !isVisible() || index < 0 || !isTabVisible(index)) {
        indicator_->hide();
        weights_ = QVector<qreal>(count(), 0);
        update();
        return;
    }
    displayedIndex_ = index;
    target_ = tabRect(index).adjusted(2, 2, -2, -2);
    const bool transition = animate && indicator_->isVisible();
    start_ = transition ? QRectF(indicator_->geometry()) : target_;
    startWeights_ = weights_;
    indicator_->show();
    if (transition) {
        advance(0);
        animation_->start();
    } else
        advance(1);
}
void ConnectionTabs::resizeEvent(QResizeEvent* event) {
    QTabBar::resizeEvent(event);
    updateIndicator(false);
}
void ConnectionTabs::tabLayoutChange() {
    QTabBar::tabLayoutChange();
    if (!animation_ || animation_->state() == QAbstractAnimation::Running ||
        (displayedIndex_ >= 0 && currentIndex() != displayedIndex_))
        return;
    updateIndicator(false);
}
void ConnectionTabs::hideEvent(QHideEvent* event) {
    animation_->stop();
    indicator_->hide();
    QTabBar::hideEvent(event);
}
void ConnectionTabs::showEvent(QShowEvent* event) {
    QTabBar::showEvent(event);
    updateIndicator(false);
}
int ConnectionTabs::tabSlotHeight() const {
    int measured = 0;
    for (int i = 0; i < count(); ++i)
        if (isTabVisible(i))
            measured = qMax(measured, tabRect(i).height());
    if (measured > 0)
        return measured;
    // Empty workspace: with no tab to size it, QTabBar only reports its minimum height and
    // the fixed-size "+" button would be pinned to the top edge (clipped, directly under the
    // toolbar). Ask the style for a tab-sized slot and keep the button plus the row's vertical
    // insets as a floor so the empty row is as tall as a row with tabs.
    QStyleOptionTab option;
    option.initFrom(this);
    option.text = QStringLiteral("W");
    const auto styled = style()->sizeFromContents(QStyle::CT_TabBarTab, &option, QSize(40, 12), this);
    const int insets = 2 * style()->pixelMetric(QStyle::PM_TabBarTabVSpace, &option, this);
    const int floorHeight = (add_ ? add_->height() : 0) + insets;
    return qMax(qMax(styled.height(), floorHeight), minimumHeight());
}
QSize ConnectionTabs::sizeHint() const {
    auto size = QTabBar::sizeHint();
    size.setHeight(qMax(size.height(), tabSlotHeight()));
    return size;
}
QSize ConnectionTabs::minimumSizeHint() const {
    auto size = QTabBar::minimumSizeHint();
    size.setHeight(qMax(size.height(), tabSlotHeight()));
    return size;
}
} // namespace portwave
