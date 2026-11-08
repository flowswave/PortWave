#pragma once
#include <QApplication>
#include <QPainter>
#include <QStyledItemDelegate>

namespace portwave {
// GUI-thread plain-text result cells. One invariant text rectangle/font in all
// selection/focus states; the owning view owns this delegate. Does not alter data.
class StableResultDelegate final : public QStyledItemDelegate {
  public:
    explicit StableResultDelegate(QObject* parent) : QStyledItemDelegate(parent) {}
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyleOptionViewItem cell(option);
        initStyleOption(&cell, index);
        const auto text = cell.text;
        cell.text.clear();
        cell.state &= ~QStyle::State_HasFocus;
        painter->save();
        const auto* style = cell.widget ? cell.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &cell, painter, cell.widget);
        const bool lightSelection = cell.state.testFlag(QStyle::State_Selected) &&
                                    qApp->property("portwave.theme.id").toString() == "light";
        if (lightSelection && index.column() == 0)
            painter->fillRect(QRect(cell.rect.left(), cell.rect.top(), 3, cell.rect.height()),
                              QColor("#0c66e4"));
        painter->setFont(cell.font);
        const auto group = cell.state.testFlag(QStyle::State_Enabled) ? QPalette::Active : QPalette::Disabled;
        const auto role = cell.state.testFlag(QStyle::State_Selected) && !lightSelection
                              ? QPalette::HighlightedText
                              : QPalette::Text;
        painter->setPen(cell.palette.color(group, role));
        const auto rect = cell.rect.adjusted(8, 0, -8, 0);
        painter->setClipRect(rect);
        painter->drawText(rect, Qt::AlignLeft | Qt::AlignVCenter,
                          cell.fontMetrics.elidedText(text, Qt::ElideRight, rect.width()));
        painter->restore();
    }
};
} // namespace portwave
