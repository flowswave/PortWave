#include "features/snap_layout.h"
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QTimer>
#include <algorithm>
#include <array>

namespace portwave {
namespace {
// Same normalized cells, order, spacing, minimum sizes and expansion envelope
// as the reference snap_layout.py. Keep previews and actual panes in one space.
const std::array<QVector<QRectF>, 7> layouts{{
    {{0, 0, .5, 1}, {.5, 0, .5, 1}},
    {{0, 0, 2.0 / 3, 1}, {2.0 / 3, 0, 1.0 / 3, 1}},
    {{0, 0, 1.0 / 3, 1}, {1.0 / 3, 0, 2.0 / 3, 1}},
    {{0, 0, .5, 1}, {.5, 0, .5, .5}, {.5, .5, .5, .5}},
    {{0, 0, .5, .5}, {.5, 0, .5, .5}, {0, .5, .5, .5}, {.5, .5, .5, .5}},
    {{0, 0, 1.0 / 3, 1}, {1.0 / 3, 0, 1.0 / 3, 1}, {2.0 / 3, 0, 1.0 / 3, 1}},
    {{0, 0, .25, 1}, {.25, 0, .5, 1}, {.75, 0, .25, 1}},
}};
QRect partition(const QRect& area, const QRectF& fraction, int gap) {
    const int x = qRound(area.width() * fraction.x());
    const int y = qRound(area.height() * fraction.y());
    const int right = qRound(area.width() * (fraction.x() + fraction.width()));
    const int bottom = qRound(area.height() * (fraction.y() + fraction.height()));
    return QRect(area.x() + x, area.y() + y, right - x, bottom - y).adjusted(gap, gap, -gap, -gap);
}
} // namespace
class SnapChooser final : public QWidget {
  public:
    explicit SnapChooser(QWidget* parent) : QWidget(parent) {
        setObjectName("snapChooser");
        setAttribute(Qt::WA_TransparentForMouseEvents);
        hide();
    }
    QRect cell(int layout, int zone) const {
        const double width = (this->width() - 24.0 - 14 * 6) / 7;
        return partition(QRect(qRound(12 + layout * (width + 14)), 12, qRound(width), height() - 24),
                         layouts[layout][zone], 2);
    }
    bool expanded{};
    QPair<int, int> hovered{-1, -1};
    std::function<bool(int)> enabled;
    std::function<QString(int, int)> occupant;

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor accent = palette().color(QPalette::Accent);
        painter.setPen(palette().color(QPalette::Mid));
        painter.setBrush(palette().color(QPalette::Window));
        painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 12, 12);
        if (!expanded) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(accent);
            painter.drawRoundedRect(QRect(width() / 2 - 27, 5, 54, 3), 2, 2);
            return;
        }
        for (int layout = 0; layout < 7; ++layout)
            for (int zone = 0; zone < layouts[layout].size(); ++zone) {
                const bool selected = hovered == qMakePair(layout, zone);
                const auto name = occupant(layout, zone);
                painter.setOpacity(enabled(layout) ? 1.0 : .3);
                painter.setPen(selected ? accent : palette().color(QPalette::Mid));
                painter.setBrush(selected ? accent.lighter(150) : palette().color(QPalette::Base));
                const auto cellRect = cell(layout, zone);
                painter.drawRoundedRect(cellRect, 3, 3);
                if (!name.isEmpty()) {
                    painter.setPen(selected ? QColor("#21252b") : palette().color(QPalette::Text));
                    painter.drawText(cellRect.adjusted(3, 3, -3, -3), Qt::AlignCenter | Qt::TextWrapAnywhere,
                                     name);
                }
            }
    }
};
SnapLayout::SnapLayout(QMdiArea* area, QTabBar* tabs, Resolver resolver, QObject* parent)
    : QObject(parent), area_(area), tabs_(tabs), resolver_(std::move(resolver)) {
    chooser_ = new SnapChooser(area_->viewport());
    chooser_->enabled = [this](int layout) { return enabled(layout); };
    chooser_->occupant = [this](int layout, int zone) {
        for (auto it = assignments_.cbegin(); it != assignments_.cend(); ++it)
            if (it.value() == qMakePair(layout, zone))
                return it.key()->windowTitle();
        return QString{};
    };
    preview_ = new QWidget(area_->viewport());
    preview_->setObjectName("snapPreview");
    preview_->setAttribute(Qt::WA_TransparentForMouseEvents);
    preview_->setAttribute(Qt::WA_StyledBackground);
    preview_->hide();
    tabs_->setToolTip("拖动标签放置窗口；靠近工作区顶部选择七种分区；Esc 取消");
    tabs_->installEventFilter(this);
    area_->viewport()->installEventFilter(this);
}
SnapLayout::~SnapLayout() {
    qApp->removeEventFilter(this);
}
void SnapLayout::watchHeader(QWidget* header, QMdiSubWindow* window) {
    headers_.insert(header, window);
    header->installEventFilter(this);
    connect(header, &QObject::destroyed, this, [this, header] { headers_.remove(header); });
}
void SnapLayout::forgetWindow(QMdiSubWindow* window) {
    if (active_ == window)
        finish(true);
    assignments_.remove(window);
    presetAssignments_.remove(window);
}
QRect SnapLayout::target(int layout, int zone) const {
    return partition(area_->viewport()->rect(), layouts[layout][zone], 4);
}
bool SnapLayout::enabled(int layout) const {
    for (int zone = 0; zone < layouts[layout].size(); ++zone) {
        const auto rect = target(layout, zone);
        if (rect.width() < 240 || rect.height() < 160)
            return false;
    }
    return true;
}
void SnapLayout::moveDrag(const QPoint& global) {
    if (!active_)
        return;
    if (!dragging_) {
        if ((global - press_).manhattanLength() < QApplication::startDragDistance())
            return;
        dragging_ = true;
        emit draggingChanged(true);
        active_->showNormal();
        if (wasMaximized_)
            active_->resize(std::min(640, area_->viewport()->width()),
                            std::min(480, area_->viewport()->height()));
        offset_ =
            QPoint(qBound(0, offset_.x(), std::max(0, active_->width() - 20)), qBound(0, offset_.y(), 30));
        chooser_->expanded = false;
        chooser_->setGeometry((area_->viewport()->width() - 260) / 2, 8, 260, 14);
        chooser_->show();
    }
    const auto local = area_->viewport()->mapFromGlobal(global);
    QPoint position = local - offset_;
    position.setX(qBound(0, position.x(), std::max(0, area_->viewport()->width() - active_->width())));
    position.setY(qBound(0, position.y(), std::max(0, area_->viewport()->height() - active_->height())));
    active_->move(position);
    if (!chooser_->expanded && chooser_->geometry().adjusted(-45, -18, 45, 38).contains(local))
        chooser_->expanded = true;
    else if (chooser_->expanded && !chooser_->geometry().adjusted(-60, -40, 60, 75).contains(local))
        chooser_->expanded = false;
    const int width = chooser_->expanded ? std::min(1060, area_->viewport()->width() - 24)
                                         : std::min(260, area_->viewport()->width() - 24);
    chooser_->setGeometry((area_->viewport()->width() - width) / 2, 8, width, chooser_->expanded ? 108 : 14);
    chooser_->setProperty("expanded", chooser_->expanded);
    hovered_ = {-1, -1};
    if (chooser_->expanded) {
        const auto point = chooser_->mapFromGlobal(global);
        for (int layout = 0; layout < 7; ++layout)
            for (int zone = 0; zone < layouts[layout].size(); ++zone)
                if (enabled(layout) && chooser_->cell(layout, zone).contains(point))
                    hovered_ = {layout, zone};
    }
    chooser_->hovered = hovered_;
    if (hovered_.first >= 0) {
        preview_->setGeometry(target(hovered_.first, hovered_.second));
        preview_->show();
        preview_->raise();
    } else
        preview_->hide();
    chooser_->raise();
    chooser_->update();
}
void SnapLayout::finish(bool cancel) {
    QPointer<QMdiSubWindow> detached;
    if (active_ && dragging_) {
        if (cancel) {
            active_->setGeometry(original_);
            if (wasMaximized_)
                active_->showMaximized();
        } else {
            presetAssignments_.remove(active_);
            const bool hadOld = assignments_.contains(active_);
            const auto old = assignments_.take(active_);
            if (hovered_.first >= 0) {
                QMdiSubWindow* occupied = nullptr;
                for (auto it = assignments_.cbegin(); it != assignments_.cend(); ++it)
                    if (it.value() == hovered_)
                        occupied = it.key();
                if (occupied) {
                    assignments_.remove(occupied);
                    if (hadOld && old.first == hovered_.first && old != hovered_) {
                        assignments_.insert(occupied, old);
                        occupied->setGeometry(target(old.first, old.second));
                    } else
                        occupied->move(occupied->pos() + QPoint(30, 30));
                }
                assignments_.insert(active_, hovered_);
                connect(active_, &QObject::destroyed, this,
                        [this, window = active_.data()] { assignments_.remove(window); });
                active_->setGeometry(target(hovered_.first, hovered_.second));
            }
            active_->raise();
            area_->setActiveSubWindow(active_);
            detached = active_;
        }
    }
    const bool wasDragging = dragging_;
    active_.clear();
    dragging_ = false;
    chooser_->hide();
    preview_->hide();
    qApp->removeEventFilter(this);
    if (detached)
        emit paneDetached(detached);
    if (wasDragging)
        emit draggingChanged(false);
}
bool SnapLayout::applyPreset(const QString& id) {
    if (id != "single" && id != "columns" && id != "rows")
        return false;
    QVector<QMdiSubWindow*> windows;
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto* window = resolver_(i))
            windows.push_back(window);
    if (windows.isEmpty())
        return true;
    const bool columns = id == "columns";
    if (id != "single" && (columns ? area_->viewport()->width() / windows.size() < 240
                                   : area_->viewport()->height() / windows.size() < 160))
        return false;
    finish(true);
    const QSignalBlocker blocker(area_);
    QPointer<QMdiSubWindow> previous = area_->activeSubWindow();
    assignments_.clear();
    presetAssignments_.clear();
    for (int i = 0; i < windows.size(); ++i) {
        auto* window = windows[i];
        if (id == "single") {
            emit paneDocked(window);
            window->showMaximized();
        } else {
            emit paneDetached(window);
            window->showNormal();
            const qreal count = windows.size();
            const QRectF fraction =
                columns ? QRectF(i / count, 0, 1 / count, 1) : QRectF(0, i / count, 1, 1 / count);
            // Reuse existing two/three-column slots so later drag swaps and
            // chooser occupancy behave exactly like manually snapped panes.
            if (columns && (windows.size() == 2 || windows.size() == 3))
                assignments_.insert(window, {windows.size() == 2 ? 0 : 5, i});
            else
                presetAssignments_.insert(window, fraction);
            window->setGeometry(partition(area_->viewport()->rect(), fraction, 4));
        }
    }
    if (previous) {
        area_->setActiveSubWindow(previous);
        previous->raise();
        if (id == "single") {
            const QSignalBlocker tabsBlocker(tabs_);
            tabs_->setCurrentIndex(windows.indexOf(previous));
        }
    }
    return true;
}
void SnapLayout::reflow() {
    for (auto it = presetAssignments_.cbegin(); it != presetAssignments_.cend(); ++it)
        if (it.key() != active_ && !it.key()->isMaximized())
            it.key()->setGeometry(partition(area_->viewport()->rect(), it.value(), 4));
    for (auto it = assignments_.cbegin(); it != assignments_.cend(); ++it)
        if (it.key() != active_ && !it.key()->isMaximized())
            it.key()->setGeometry(target(it.value().first, it.value().second));
}
bool SnapLayout::eventFilter(QObject* watched, QEvent* event) {
    if (watched == area_->viewport() && event->type() == QEvent::Resize)
        QTimer::singleShot(0, this, [this] { reflow(); });
    if (active_ && event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        finish(true);
        return true;
    }
    const bool fromTabs = watched == tabs_;
    auto* header = qobject_cast<QWidget*>(watched);
    if (!fromTabs && !headers_.contains(header))
        return false;
    if (event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() != Qt::LeftButton)
            return false;
        if (auto* window = fromTabs ? resolver_(tabs_->tabAt(mouse->position().toPoint()))
                                    : headers_.value(header).data()) {
            active_ = window;
            press_ = mouse->globalPosition().toPoint();
            original_ = window->geometry();
            wasMaximized_ = window->isMaximized();
            offset_ = area_->viewport()->mapFromGlobal(press_) - window->pos();
            qApp->installEventFilter(this);
        }
    } else if (event->type() == QEvent::MouseMove && active_) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->buttons().testFlag(Qt::LeftButton)) {
            moveDrag(mouse->globalPosition().toPoint());
            return dragging_;
        }
    } else if (event->type() == QEvent::MouseButtonRelease && active_) {
        const bool handled = dragging_;
        finish(false);
        return handled;
    } else if (event->type() == QEvent::Hide && active_)
        finish(true);
    return false;
}
} // namespace portwave
