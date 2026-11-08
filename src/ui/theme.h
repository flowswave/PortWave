#pragma once
#include "ui/appearance_profile.h"
#include <QColor>
#include <QIcon>
#include <QObject>

namespace portwave {
struct DesignTokens {
    QColor shell, surface, text, muted, border, accent, selection;
    static DesignTokens forMode(bool dark);
    static DesignTokens forTheme(const QString& id);
};
class ThemeManager final : public QObject {
    Q_OBJECT
  public:
    explicit ThemeManager(QObject* parent = nullptr);
    bool dark() const { return dark_; }
    void apply(bool dark);
    // GUI-thread view update; unknown IDs fall back to chromatic. Never changes
    // sessions, documents, or user window arrangements. Control metrics are view-only.
    void applyTheme(const QString& id);
    QString themeId() const { return themeId_; }
    AppearanceProfile profile() const { return AppearanceProfile::load(); }
  signals:
    void changed(bool dark);

  private:
    bool dark_ = true;
    QString themeId_ = "chromatic";
};
class IconManager {
  public:
    static QIcon icon(const QString& name, bool dark = true, const QColor& color = {}, bool filled = false);
    // Application/taskbar mark: the wave glyph on a rounded plate, so Windows gets
    // a legible icon at 16 px instead of a thin transparent line drawing. Cached.
    static QIcon appIcon();
};
} // namespace portwave
