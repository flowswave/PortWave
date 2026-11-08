#include "features/window_caption.h"
#include <QAbstractButton>
#include <QApplication>
#include <QCursor>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QSettings>
#include <QTimer>
#include <QToolButton>
#include <QWindow>

namespace portwave {
namespace {
// Transparent centre, native edge/corner sizing even when child widgets cover the root.
// Repair only transient pointer/pressed state after native menus and modal
// dialogs. Checked state and keyboard focus belong to the control and survive.
class ButtonStateGuard final : public QObject {
  public:
    explicit ButtonStateGuard(QObject* parent) : QObject(parent) {
        setObjectName("buttonStateGuard");
        qApp->installEventFilter(this);
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        const auto kind = event->type();
        if (kind == QEvent::MouseButtonRelease || kind == QEvent::WindowActivate ||
            kind == QEvent::WindowDeactivate || kind == QEvent::FocusOut ||
            ((kind == QEvent::Hide || kind == QEvent::Close) && qobject_cast<QWidget*>(watched))) {
            if (!pending_) {
                pending_ = true;
                QTimer::singleShot(0, this, [this] {
                    pending_ = false;
                    if (QApplication::mouseButtons().testFlag(Qt::LeftButton))
                        return;
                    auto* under = QApplication::widgetAt(QCursor::pos());
                    for (auto* widget : QApplication::allWidgets())
                        if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
                            button->setDown(false);
                            const bool hover = button->isVisible() && under &&
                                               (under == button || button->isAncestorOf(under));
                            button->setAttribute(Qt::WA_UnderMouse, hover);
                            button->update();
                        }
                });
            }
        }
        return false;
    }

  private:
    bool pending_{};
};
// Fit after constructors/restoreGeometry have completed, never during a drag.
class WindowBoundsGuard final : public QObject {
  public:
    explicit WindowBoundsGuard(QWidget* window) : QObject(window), window_(window) {
        setObjectName("windowBoundsGuard");
        window->installEventFilter(this);
        for (auto* screen : QGuiApplication::screens())
            watch(screen);
        connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen* screen) { watch(screen); });
    }

  protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Show) {
            if (!handleConnected_ && window_->windowHandle()) {
                handleConnected_ = true;
                connect(window_->windowHandle(), &QWindow::screenChanged, this, [this] { queue(); });
            }
            queue();
        } else if (event->type() == QEvent::DevicePixelRatioChange ||
                   event->type() == QEvent::WindowStateChange)
            queue();
        return false;
    }

  private:
    QWidget* window_;
    bool pending_{}, handleConnected_{};
    void watch(QScreen* screen) {
        connect(screen, &QScreen::availableGeometryChanged, this, [this] { queue(); });
    }
    void queue() {
        if (pending_)
            return;
        pending_ = true;
        QTimer::singleShot(0, this, [this] {
            pending_ = false;
            if (window_->isVisible())
                fitWindowToScreen(window_);
        });
    }
};
class WindowOutline final : public QWidget {
  public:
    explicit WindowOutline(QWidget* window) : QWidget(window) {
        setObjectName("windowResizeFrame");
        setAttribute(Qt::WA_NoSystemBackground);
        setMouseTracking(true);
        window->installEventFilter(this);
        refresh();
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == parentWidget() &&
            (event->type() == QEvent::Resize || event->type() == QEvent::Show ||
             event->type() == QEvent::WindowStateChange || event->type() == QEvent::ChildAdded))
            refresh();
        return false;
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        const auto edges = edgesAt(event->position().toPoint());
        const bool diagonal = (edges.testFlag(Qt::LeftEdge) && edges.testFlag(Qt::TopEdge)) ||
                              (edges.testFlag(Qt::RightEdge) && edges.testFlag(Qt::BottomEdge));
        setCursor((edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge)) &&
                          (edges.testFlag(Qt::TopEdge) || edges.testFlag(Qt::BottomEdge))
                      ? (diagonal ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor)
                      : (edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge) ? Qt::SizeHorCursor
                                                                                       : Qt::SizeVerCursor));
    }
    void mousePressEvent(QMouseEvent* event) override {
        auto* host = parentWidget();
        if (event->button() == Qt::LeftButton && host->windowHandle())
            host->windowHandle()->startSystemResize(edgesAt(event->position().toPoint()));
    }

  private:
    Qt::Edges edgesAt(const QPoint& point) const {
        Qt::Edges edges;
        if (point.x() < 10)
            edges |= Qt::LeftEdge;
        if (point.x() >= width() - 10)
            edges |= Qt::RightEdge;
        if (point.y() < 10)
            edges |= Qt::TopEdge;
        if (point.y() >= height() - 10)
            edges |= Qt::BottomEdge;
        return edges;
    }
    void refresh() {
        auto* host = parentWidget();
        setGeometry(host->rect());
        setMask(QRegion(rect()).subtracted(QRegion(rect().adjusted(5, 5, -5, -5))));
        setVisible(host->isWindow() && !host->isMaximized() && !host->isMinimized());
        raise();
    }
};
class WaveLogo final : public QWidget {
  public:
    explicit WaveLogo(int size, QWidget* parent) : QWidget(parent) {
        setObjectName("portWaveLogo");
        setFixedSize(size, size);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        window()->windowIcon().paint(&painter, rect());
    }
};
class CaptionButton final : public QAbstractButton {
  public:
    CaptionButton(int kind, QWidget* parent) : QAbstractButton(parent), kind_(kind) {
        setFixedSize(40, 30);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (underMouse())
            painter.fillRect(rect(),
                             kind_ == 2 ? QColor("#d65368") : palette().color(QPalette::Window).lighter(125));
        painter.setPen(QPen(palette().color(QPalette::WindowText), 1.1));
        const qreal x = width() / 2.0, y = height() / 2.0;
        if (kind_ == 0)
            painter.drawLine(QPointF(x - 5, y + 3), QPointF(x + 5, y + 3));
        else if (kind_ == 1)
            painter.drawRect(QRectF(x - 4.5, y - 4.5, 9, 9));
        else {
            painter.drawLine(QPointF(x - 4, y - 4), QPointF(x + 4, y + 4));
            painter.drawLine(QPointF(x + 4, y - 4), QPointF(x - 4, y + 4));
        }
    }

  private:
    int kind_;
};
} // namespace
QIcon pinIcon(bool checked) {
    QPixmap image(24, 24);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(
        QPen(checked ? qApp->palette().color(QPalette::Accent) : qApp->palette().color(QPalette::WindowText),
             1.5));
    painter.translate(12, 11);
    if (!checked)
        painter.rotate(32);
    painter.drawLine(-4, -7, 4, -7);
    painter.drawLine(-3, -6, -3, -1);
    painter.drawLine(3, -6, 3, -1);
    painter.drawLine(-3, -1, -6, 2);
    painter.drawLine(3, -1, 6, 2);
    painter.drawLine(-6, 2, 6, 2);
    painter.drawLine(0, 2, 0, 9);
    return QIcon(image);
}
QIcon documentIcon() {
    QPixmap image(20, 20);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(qApp->palette().color(QPalette::Accent), 1.3));
    painter.drawPolyline(QPolygonF{{5, 2}, {12, 2}, {16, 6}, {16, 18}, {5, 18}, {5, 2}});
    painter.drawPolyline(QPolygonF{{12, 2}, {12, 6}, {16, 6}});
    for (int y : {9, 12, 15})
        painter.drawLine(8, y, 13, y);
    return QIcon(image);
}
QIcon folderIcon() {
    QPixmap image(20, 20);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(qApp->palette().color(QPalette::WindowText), 1.3));
    painter.drawPolyline(QPolygonF{{2, 16}, {2, 4}, {8, 4}, {10, 7}, {18, 7}, {16, 16}, {2, 16}});
    painter.drawPolyline(QPolygonF{{2, 8}, {18, 8}});
    return QIcon(image);
}
WindowCaption::WindowCaption(QWidget* window, const QString& title, int captionHeight)
    : QWidget(window), window_(window) {
    setObjectName("appChrome");
    setAttribute(Qt::WA_StyledBackground);
    setFixedHeight(relativePin_ ? 28 : 30);
    relativePin_ = captionHeight == 32;
    if (relativePin_) {
        relativeMain_ = window->parentWidget();
        while (relativeMain_ && relativeMain_->parentWidget())
            relativeMain_ = relativeMain_->parentWidget();
        if (relativeMain_)
            relativeMain_->installEventFilter(this);
    }
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(relativePin_ ? 9 : 12, 0, relativePin_ ? 0 : 4, 0);
    row->setSpacing(relativePin_ ? 5 : 9);
    auto* logo = new WaveLogo(relativePin_ ? 20 : 24, this);
    row->addWidget(logo);
    const auto fixedTitle = window->property("captionText").toString();
    title_ = new QLabel(fixedTitle.isEmpty() ? title : fixedTitle);
    row->addWidget(title_, 1);
    connect(window, &QWidget::windowTitleChanged, title_, [this, fixedTitle](const QString& value) {
        title_->setText(fixedTitle.isEmpty() ? value : fixedTitle);
    });
    auto* pin = pin_ = new QToolButton;
    pin->setObjectName("windowPin");
    pin->setAccessibleName(relativePin_ ? "固定在串口主窗口上方" : "主窗口置顶");
    pin->setToolTip(relativePin_ ? "固定在串口主窗口上方（其它软件仍可遮盖）" : "主窗口置顶");
    pin->setCheckable(true);
    pin->setIcon(pinIcon());
    pin->setFixedSize(relativePin_ ? 28 : 36, relativePin_ ? 28 : 30);
    row->addWidget(pin);
    connect(pin, &QToolButton::toggled, this, [this, pin](bool enabled) {
        pin->setIcon(pinIcon(enabled));
        if (relativePin_) {
            const auto identity = window_->property("logWorkspaceIdentity").toString();
            QSettings().setValue(
                identity.isEmpty() ? "logs/window_on_top" : "logs/workspace_pins/" + identity, enabled);
            queueRaise();
            return;
        }
        const auto geometry = window_->geometry();
        const bool maximized = window_->isMaximized();
        window_->setWindowFlag(Qt::WindowStaysOnTopHint, enabled);
        if (maximized)
            window_->showMaximized();
        else {
            window_->show();
            window_->setGeometry(geometry);
        }
    });
    if (relativePin_) {
        const auto identity = window_->property("logWorkspaceIdentity").toString();
        pin->setChecked(
            QSettings()
                .value(identity.isEmpty() ? "logs/window_on_top" : "logs/workspace_pins/" + identity, false)
                .toBool());
    }
    auto button = [&](int kind, const QString& tip, auto callback) {
        auto* control = new CaptionButton(kind, this);
        control->setObjectName("chromeControl");
        control->setToolTip(tip);
        control->setAccessibleName(tip);
        row->addWidget(control);
        connect(control, &QAbstractButton::clicked, this, callback);
    };
    button(0, "最小化", [this] { window_->showMinimized(); });
    button(1, "最大化 / 还原",
           [this] { window_->isMaximized() ? window_->showNormal() : window_->showMaximized(); });
    button(2, "关闭", [this] { window_->close(); });
    window->installEventFilter(this);
    decorateWindow(window);
}
void fitWindowToScreen(QWidget* window, const QRect& area) {
    if (!window || !window->isWindow() || window->isMaximized() || window->isMinimized())
        return;
    QRect available = area;
    if (!available.isValid()) {
        auto* screen = QGuiApplication::screenAt(window->frameGeometry().center());
        if (!screen && window->parentWidget())
            screen = window->parentWidget()->screen();
        if (!screen)
            screen = window->screen();
        if (!screen)
            return;
        available = screen->availableGeometry();
    }
    const auto frame = window->frameGeometry();
    const auto client = window->geometry();
    const QSize border(qMax(0, frame.width() - client.width()), qMax(0, frame.height() - client.height()));
    const auto size = client.size().boundedTo(available.size() - border).expandedTo(window->minimumSize());
    const QSize outer = size + border;
    const QPoint top(
        qBound(available.left(), frame.left(), qMax(available.left(), available.right() - outer.width() + 1)),
        qBound(available.top(), frame.top(), qMax(available.top(), available.bottom() - outer.height() + 1)));
    if (size != window->size())
        window->resize(size);
    if (top != frame.topLeft())
        window->move(top);
}
QWidget* decorateWindow(QWidget* window) {
    if (!window->findChild<QObject*>("windowBoundsGuard", Qt::FindDirectChildrenOnly))
        new WindowBoundsGuard(window);
    if (!qApp->findChild<QObject*>("buttonStateGuard"))
        new ButtonStateGuard(qApp);
    if (auto* existing = window->findChild<QWidget*>("windowResizeFrame", Qt::FindDirectChildrenOnly))
        return existing;
    return new WindowOutline(window);
}
void WindowCaption::setCommandBar(QWidget* commands) {
    auto* row = qobject_cast<QHBoxLayout*>(layout());
    if (commands_ && commands_ != commands)
        row->removeWidget(commands_);
    commands_ = commands;
    row->setStretch(1, commands ? 0 : 1);
    title_->setSizePolicy(commands ? QSizePolicy::Maximum : QSizePolicy::Preferred, QSizePolicy::Preferred);
    if (commands) {
        commands->setParent(this);
        row->insertWidget(2, commands, 1);
        commands->show();
    }
    setFixedHeight(commands ? commands->height() + 8 : relativePin_ ? 28 : 30);
}
QIcon sendControlIcon(int kind, const QColor& color) {
    QPixmap pixmap(48, 48);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(2, 2);
    painter.setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (kind == 0)
        for (int y : {6, 12, 18})
            painter.drawLine(4, y, 20, y);
    else if (kind == 1) {
        painter.drawEllipse(QRectF(3, 3, 18, 18));
        painter.drawLine(12, 6, 12, 12);
        painter.drawLine(12, 12, 16, 14);
    } else {
        painter.drawPolyline(QPolygonF{{3, 10}, {21, 3}, {15, 21}, {11, 13}, {3, 10}});
        painter.drawLine(11, 13, 21, 3);
    }
    return QIcon(pixmap);
}
bool WindowCaption::eventFilter(QObject* watched, QEvent* event) {
    if (watched == window_ && event->type() == QEvent::PaletteChange)
        pin_->setIcon(pinIcon(pin_->isChecked()));
    if (relativePin_ && watched == relativeMain_ &&
        (event->type() == QEvent::WindowActivate || event->type() == QEvent::ZOrderChange ||
         event->type() == QEvent::Show || event->type() == QEvent::WindowStateChange))
        queueRaise();
    if (relativePin_ && watched == window_ && event->type() == QEvent::Show && window_->windowHandle())
        window_->windowHandle()->setTransientParent(nullptr);
    if (watched == window_ && event->type() == QEvent::MouseButtonPress && !window_->isMaximized()) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        const auto point = mouse->position();
        Qt::Edges edges;
        if (point.x() < 6)
            edges |= Qt::LeftEdge;
        if (point.x() > window_->width() - 6)
            edges |= Qt::RightEdge;
        if (point.y() < 6)
            edges |= Qt::TopEdge;
        if (point.y() > window_->height() - 6)
            edges |= Qt::BottomEdge;
        if (edges && mouse->button() == Qt::LeftButton && window_->windowHandle())
            return window_->windowHandle()->startSystemResize(edges);
    }
    return QWidget::eventFilter(watched, event);
}
void WindowCaption::queueRaise() {
    if (!pin_->isChecked() || raisePending_)
        return;
    raisePending_ = true;
    QTimer::singleShot(0, this, [this] {
        raisePending_ = false;
        if (!pin_->isChecked() || !window_ || !relativeMain_ || !window_->isVisible() ||
            window_->isMinimized() || !relativeMain_->isVisible() || relativeMain_->isMinimized())
            return;
        auto* active = QApplication::activeWindow();
        if (active == relativeMain_ || active == window_)
            window_->raise();
    });
}
void WindowCaption::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && window_->windowHandle())
        window_->windowHandle()->startSystemMove();
}
void WindowCaption::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton)
        window_->isMaximized() ? window_->showNormal() : window_->showMaximized();
}
} // namespace portwave
