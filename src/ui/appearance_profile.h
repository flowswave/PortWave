#pragma once
#include <QColor>
#include <QEasingCurve>
#include <QMap>
#include <QString>

namespace portwave {
// GUI-thread value snapshots. Invalid stored controls fall back to standard;
// loading does not write settings. Colors and controls are independent; tab motion is fixed.
struct ControlStyle {
    QString id = "standard";
    int radius = 5, paddingY = 4, tabPaddingY = 6, toolbarHeight = 34, tabHeight = 28;
    static ControlStyle fromId(const QString& id);
};
struct AppearanceProfile {
    bool dark = true;
    ControlStyle controls;
    // A layout is an explicit one-shot request; color changes never apply it.
    QString layout = "keep";
    QString themeId = "chromatic";
    // Empty ID selects the current theme; explicit ID reads that theme globally.
    // No writes, I/O to sessions, or workspace-specific overrides.
    static AppearanceProfile load(const QString& themeId = {});
};
// Resource defaults plus existing per-theme user accent. Returns a validated
// color map; no file/session access. Caller owns the returned snapshot.
QString validatedThemeId(const QString& id);
QMap<QString, QColor> resolveThemeColors(const QString& id, bool userOverrides = true);
QMap<QString, QColor> resolveThemeColors(bool dark, bool userOverrides = true);
} // namespace portwave
