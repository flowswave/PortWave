#include "features/split_grip.h"
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QMouseEvent>
#include <QPainter>
#include <QWidget>
#include <algorithm>
#include <limits>

namespace portwave {
namespace {
constexpr int kGripThickness = 16;
constexpr int kGripLength = 64;
constexpr int kHitTolerance = 10;
constexpr int kMinSpan = 64;       // shortest shared edge that still shows a handle
constexpr int kJoinGap = 32;       // panes on one side are usually separated by a layout gap
constexpr int kMinPaneWidth = 240; // same floor as SnapLayout preset validation
constexpr int kMinPaneHeight = 160;

bool containsWindow(const QVector<QPointer<QMdiSubWindow>>& list, QMdiSubWindow* window) {
    for (const auto& entry : list)
        if (entry.data() == window)
            return true;
    return false;
}

bool samePanes(const QVector<QPointer<QMdiSubWindow>>& left, const QVector<QPointer<QMdiSubWindow>>& right) {
    if (left.size() != right.size())
        return false;
    for (const auto& entry : left)
        if (!containsWindow(right, entry.data()))
            return false;
    return true;
}

// Three dots on a soft pill. Vertical dividers get a vertical dot column, while
// stacked panes get a horizontal dot row, so the affordance matches the drag axis.
class GripDots final : public QWidget {
  public:
    explicit GripDots(QWidget* parent) : QWidget(parent) {
        setObjectName("splitGrip");
        setAttribute(Qt::WA_TranslucentBackground, false);
        setAttribute(Qt::WA_TransparentForMouseEvents, false);
    }
    void setOrientation(bool verticalDivider) {
        verticalDivider_ = verticalDivider;
        setCursor(verticalDivider_ ? Qt::SplitHCursor : Qt::SplitVCursor);
        const QSize size =
            verticalDivider_ ? QSize(kGripThickness, kGripLength) : QSize(kGripLength, kGripThickness);
        if (!isVisible() || size != this->size())
            setFixedSize(size);
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        auto body = palette().color(QPalette::Base);
        body.setAlpha(238);
        auto border = palette().color(QPalette::Mid);
        border.setAlpha(180);
        painter.setPen(QPen(border, 1));
        painter.setBrush(body);
        painter.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 8, 8);
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().color(QPalette::Accent));
        constexpr qreal radius = 2.5;
        constexpr int step = 10;
        for (int index = -1; index <= 1; ++index) {
            const QPoint center = verticalDivider_ ? QPoint(width() / 2, height() / 2 + index * step)
                                                   : QPoint(width() / 2 + index * step, height() / 2);
            painter.drawEllipse(QPointF(center), radius, radius);
        }
    }

  private:
    bool verticalDivider_{true};
};
} // namespace

SplitGrip::SplitGrip(QMdiArea* area, QObject* parent) : QObject(parent), area_(area) {
    grip_ = new GripDots(area_->viewport());
    static_cast<GripDots*>(grip_)->setOrientation(true);
    grip_->hide();
    dwell_.setSingleShot(true);
    dwell_.setInterval(1000);
    connect(&dwell_, &QTimer::timeout, this, [this] { showGrip(); });
    // Only the viewport/mdi area is filtered locally; the application-wide
    // filter keeps hover tracking working while the pointer is over a pane.
    area_->viewport()->installEventFilter(this);
    qApp->installEventFilter(this);
}
SplitGrip::~SplitGrip() {
    qApp->removeEventFilter(this);
}
QVector<QMdiSubWindow*> SplitGrip::panes() const {
    QVector<QMdiSubWindow*> result;
    if (!area_)
        return result;
    for (auto* window : area_->subWindowList(QMdiArea::StackingOrder))
        if (window && window->isVisible() && !window->isMaximized() && !window->isMinimized())
            result.push_back(window);
    return result;
}
QRect SplitGrip::paneRect(QMdiSubWindow* window) const {
    // QMdiSubWindow geometry is already expressed in viewport coordinates.
    return window ? QRect(window->pos(), window->size()) : QRect();
}
QVector<SplitGrip::Edge> SplitGrip::edges() const {
    struct Pair {
        bool vertical;
        int boundary;
        int from, to;
        QMdiSubWindow* leading;
        QMdiSubWindow* trailing;
    };
    QVector<Pair> pairs;
    const auto windows = panes();
    for (auto* first : windows) {
        for (auto* second : windows) {
            if (first == second)
                continue;
            const QRect a = paneRect(first), b = paneRect(second);
            // Side-by-side pair: shared vertical edge.
            if (a.left() < b.left() && std::abs(a.right() - b.left()) <= kHitTolerance) {
                const int top = std::max(a.top(), b.top()), bottom = std::min(a.bottom(), b.bottom());
                if (bottom - top >= kMinSpan)
                    pairs.push_back({true, (a.right() + b.left()) / 2, top, bottom, first, second});
            }
            // Stacked pair: shared horizontal edge.
            if (a.top() < b.top() && std::abs(a.bottom() - b.top()) <= kHitTolerance) {
                const int left = std::max(a.left(), b.left()), right = std::min(a.right(), b.right());
                if (right - left >= kMinSpan)
                    pairs.push_back({false, (a.bottom() + b.top()) / 2, left, right, first, second});
            }
        }
    }
    // Group pairs by edge coordinate, then merge spans that keep one whole side
    // identical. That is what turns "one pane against two" into a single handle,
    // while many-to-many rows keep separate handles so each row stays adjustable.
    std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& left, const Pair& right) {
        if (left.vertical != right.vertical)
            return left.vertical > right.vertical;
        if (left.boundary != right.boundary)
            return left.boundary < right.boundary;
        return left.from < right.from;
    });
    QVector<Edge> result;
    for (const auto& pair : pairs) {
        Edge* target = nullptr;
        for (auto& edge : result) {
            if (edge.vertical != pair.vertical || std::abs(edge.boundary - pair.boundary) > kHitTolerance)
                continue;
            if (pair.from > edge.to + kJoinGap || pair.to < edge.from - kJoinGap)
                continue;
            if (!containsWindow(edge.leading, pair.leading) && !containsWindow(edge.trailing, pair.trailing))
                continue;
            target = &edge;
            break;
        }
        if (!target) {
            Edge edge;
            edge.vertical = pair.vertical;
            edge.boundary = pair.boundary;
            edge.from = pair.from;
            edge.to = pair.to;
            edge.offset = pair.vertical ? pair.boundary - paneRect(pair.leading).right()
                                        : pair.boundary - paneRect(pair.leading).bottom();
            edge.leading.push_back(pair.leading);
            edge.trailing.push_back(pair.trailing);
            result.push_back(edge);
            continue;
        }
        target->from = std::min(target->from, pair.from);
        target->to = std::max(target->to, pair.to);
        if (!containsWindow(target->leading, pair.leading))
            target->leading.push_back(pair.leading);
        if (!containsWindow(target->trailing, pair.trailing))
            target->trailing.push_back(pair.trailing);
    }
    return result;
}
SplitGrip::Edge SplitGrip::findEdge(const QPoint& position) const {
    Edge best;
    int bestDistance = kHitTolerance + 1;
    for (const auto& edge : edges()) {
        if (edge.leading.isEmpty() || edge.trailing.isEmpty())
            continue;
        const int distance =
            edge.vertical ? std::abs(position.x() - edge.boundary) : std::abs(position.y() - edge.boundary);
        if (distance >= bestDistance)
            continue;
        const int along = edge.vertical ? position.y() : position.x();
        if (along < edge.from || along > edge.to)
            continue;
        bestDistance = distance;
        best = edge;
    }
    return best;
}
int SplitGrip::currentBoundary(const Edge& edge) const {
    int extent = std::numeric_limits<int>::min();
    for (const auto& window : edge.leading) {
        if (!window)
            continue;
        const QRect rect = paneRect(window);
        extent = std::max(extent, edge.vertical ? rect.right() : rect.bottom());
    }
    return extent == std::numeric_limits<int>::min() ? edge.boundary : extent + edge.offset;
}
void SplitGrip::track(const QPoint& position) {
    const Edge edge = findEdge(position);
    if (edge.leading.isEmpty() || edge.trailing.isEmpty()) {
        dwell_.stop();
        hideGrip();
        hovered_ = Edge{};
        return;
    }
    const bool same = hovered_.vertical == edge.vertical && hovered_.boundary == edge.boundary &&
                      samePanes(hovered_.leading, edge.leading) &&
                      samePanes(hovered_.trailing, edge.trailing);
    if (same) {
        if (grip_->isVisible())
            positionGrip();
        return;
    }
    hovered_ = edge;
    hideGrip();
    dwell_.start();
}
void SplitGrip::showGrip() {
    if (!area_ || dragging_ || hovered_.leading.isEmpty() || hovered_.trailing.isEmpty())
        return;
    for (const auto& window : hovered_.leading + hovered_.trailing) {
        if (!window || !window->isVisible()) {
            hovered_ = Edge{};
            return;
        }
    }
    grip_->show();
    positionGrip();
    grip_->raise();
}
void SplitGrip::hideGrip() {
    if (grip_)
        grip_->hide();
}
void SplitGrip::positionGrip() {
    if (!area_ || !grip_ || hovered_.leading.isEmpty() || hovered_.trailing.isEmpty())
        return;
    // Orientation decides the fixed size, so it must precede the offset math.
    static_cast<GripDots*>(grip_)->setOrientation(hovered_.vertical);
    const int boundary = currentBoundary(hovered_);
    const int center = (hovered_.from + hovered_.to) / 2;
    if (hovered_.vertical) {
        grip_->move(boundary - grip_->width() / 2,
                    std::clamp(center - grip_->height() / 2, 0,
                               std::max(0, area_->viewport()->height() - grip_->height())));
    } else {
        grip_->move(std::clamp(center - grip_->width() / 2, 0,
                               std::max(0, area_->viewport()->width() - grip_->width())),
                    boundary - grip_->height() / 2);
    }
}
void SplitGrip::beginDrag(const QPoint& position) {
    if (hovered_.leading.isEmpty() || hovered_.trailing.isEmpty())
        return;
    dragging_ = true;
    dwell_.stop();
    dragOrigin_ = position;
    start_.clear();
    for (const auto& window : hovered_.leading + hovered_.trailing)
        if (window)
            start_.insert(window, window->geometry());
}
void SplitGrip::dragTo(const QPoint& position) {
    if (!dragging_ || hovered_.leading.isEmpty() || hovered_.trailing.isEmpty())
        return;
    const int minimum = hovered_.vertical ? kMinPaneWidth : kMinPaneHeight;
    int minDelta = std::numeric_limits<int>::min() / 2;
    int maxDelta = std::numeric_limits<int>::max() / 2;
    for (const auto& window : hovered_.leading + hovered_.trailing) {
        const QRect start = start_.value(window);
        const int extent = hovered_.vertical ? start.width() : start.height();
        const bool grows = containsWindow(hovered_.leading, window);
        if (grows)
            minDelta = std::max(minDelta, minimum - extent);
        else
            maxDelta = std::min(maxDelta, extent - minimum);
    }
    if (minDelta > maxDelta)
        return;
    const int delta =
        std::clamp(hovered_.vertical ? position.x() - dragOrigin_.x() : position.y() - dragOrigin_.y(),
                   minDelta, maxDelta);
    // Every pane on one side moves by the same delta, so a 1:2 split keeps the
    // two short panes aligned instead of leaving a step between them.
    for (const auto& window : hovered_.leading + hovered_.trailing) {
        if (!window)
            continue;
        const QRect start = start_.value(window);
        const bool grows = containsWindow(hovered_.leading, window);
        if (hovered_.vertical) {
            window->setGeometry(grows ? QRect(start.topLeft(), QSize(start.width() + delta, start.height()))
                                      : QRect(QPoint(start.x() + delta, start.y()),
                                              QSize(start.width() - delta, start.height())));
        } else {
            window->setGeometry(grows ? QRect(start.topLeft(), QSize(start.width(), start.height() + delta))
                                      : QRect(QPoint(start.x(), start.y() + delta),
                                              QSize(start.width(), start.height() - delta)));
        }
    }
    positionGrip();
}
void SplitGrip::finishDrag(bool cancel, const QPoint& position) {
    if (cancel) {
        for (auto entry = start_.cbegin(); entry != start_.cend(); ++entry)
            if (entry.key())
                entry.key()->setGeometry(entry.value());
    }
    dragging_ = false;
    start_.clear();
    dwell_.stop();
    // Keep the handle under the pointer at its new position; it hides as soon as
    // the pointer leaves the shared edge again.
    const Edge edge = cancel ? Edge{} : findEdge(position);
    if (!edge.leading.isEmpty() && !edge.trailing.isEmpty()) {
        hovered_ = edge;
        positionGrip();
        grip_->show();
        grip_->raise();
    } else {
        hovered_ = Edge{};
        hideGrip();
    }
}
bool SplitGrip::eventFilter(QObject* watched, QEvent* event) {
    if (!area_)
        return QObject::eventFilter(watched, event);
    const auto type = event->type();
    if ((type == QEvent::Resize || type == QEvent::ChildRemoved || type == QEvent::ChildAdded) &&
        watched == area_->viewport()) {
        if (!dragging_) {
            hideGrip();
            hovered_ = Edge{};
            dwell_.stop();
        }
        return QObject::eventFilter(watched, event);
    }
    if (type == QEvent::Leave && watched == area_->viewport()) {
        if (!dragging_) {
            hideGrip();
            hovered_ = Edge{};
            dwell_.stop();
        }
        return QObject::eventFilter(watched, event);
    }
    if (type != QEvent::MouseMove && type != QEvent::MouseButtonPress && type != QEvent::MouseButtonRelease)
        return QObject::eventFilter(watched, event);
    auto* mouse = static_cast<QMouseEvent*>(event);
    const QPoint position = area_->viewport()->mapFromGlobal(mouse->globalPosition().toPoint());
    if (type == QEvent::MouseMove) {
        if (dragging_) {
            if (mouse->buttons().testFlag(Qt::LeftButton))
                dragTo(position);
            return true;
        }
        if (mouse->buttons() != Qt::NoButton)
            return QObject::eventFilter(watched, event);
        track(position);
        return QObject::eventFilter(watched, event);
    }
    if (type == QEvent::MouseButtonPress) {
        if (mouse->button() == Qt::LeftButton && grip_ && grip_->isVisible() &&
            grip_->geometry().contains(position)) {
            beginDrag(position);
            return true;
        }
        return QObject::eventFilter(watched, event);
    }
    if (dragging_ && mouse->button() == Qt::LeftButton) {
        finishDrag(false, position);
        return true;
    }
    return QObject::eventFilter(watched, event);
}
} // namespace portwave
