#include "ui/theme.h"
#include <QApplication>
#include <QFile>
#include <QPainter>
#include <QPalette>
#include <QSettings>
#include <QSvgRenderer>
#include <QVariantMap>
#include <cmath>

static void initResources() {
    Q_INIT_RESOURCE(resources);
}
namespace portwave {
DesignTokens DesignTokens::forMode(bool dark) {
    return forTheme(dark ? "dark" : "light");
}
DesignTokens DesignTokens::forTheme(const QString& id) {
    const auto colors = resolveThemeColors(id, false);
    return {colors.value("shell"),
            colors.value("surface"),
            colors.value("text"),
            colors.value("text.muted"),
            colors.value("border"),
            colors.value("accent"),
            colors.value("selection.background")};
}
ThemeManager::ThemeManager(QObject* parent) : QObject(parent) {
    initResources();
    // Old settings copied a shared dark default into the light reader profile.
    // Migrate only that exact legacy-default match once; retain other custom colors.
    QSettings settings;
    const QString migrated = "logs/appearance/light/legacyBackgroundMigrated";
    if (!settings.value(migrated, false).toBool()) {
        const auto legacy = settings.value("logs/appearance/background").toString();
        if ((legacy == "#282c34" || legacy == "#171b20" || legacy == "#08131f") &&
            settings.value("logs/appearance/light/background").toString() == legacy)
            settings.remove("logs/appearance/light/background");
        settings.setValue(migrated, true);
    }
    themeId_ = AppearanceProfile::load().themeId;
    dark_ = themeId_ != "light";
}
void ThemeManager::apply(bool dark) {
    applyTheme(dark ? "dark" : "light");
}
void ThemeManager::applyTheme(const QString& id) {
    themeId_ = validatedThemeId(id);
    const bool dark = themeId_ != "light";
    dark_ = dark;
    qApp->setFont(QFont("Microsoft YaHei UI", 9));
    const auto colors = resolveThemeColors(themeId_);
    const auto profile = AppearanceProfile::load(themeId_);
    QVariantMap snapshot;
    for (auto it = colors.begin(); it != colors.end(); ++it)
        snapshot.insert(it.key(), it.value());
    qApp->setProperty("portwave.theme.id", themeId_);
    qApp->setProperty("portwave.theme.colors", snapshot);
    qApp->setProperty("portwave.theme.dark", dark);
    QPalette p;
    p.setColor(QPalette::Window, colors.value("shell"));
    p.setColor(QPalette::WindowText, colors.value("text"));
    p.setColor(QPalette::Base, colors.value("surface"));
    p.setColor(QPalette::AlternateBase, colors.value("section.background"));
    p.setColor(QPalette::Text, colors.value("text"));
    p.setColor(QPalette::Button, colors.value("button.background"));
    p.setColor(QPalette::ButtonText, colors.value("button.foreground"));
    p.setColor(QPalette::Highlight,
               colors.value("selection.textBackground", colors.value("selection.background")));
    p.setColor(QPalette::Accent, colors.value("accent"));
    p.setColor(QPalette::HighlightedText, colors.value("selection.textForeground", colors.value("text")));
    p.setColor(QPalette::Mid, colors.value("border"));
    p.setColor(QPalette::PlaceholderText, colors.value("text.muted"));
    p.setColor(QPalette::Disabled, QPalette::Text, colors.value("text.disabled"));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, colors.value("text.disabled"));
    qApp->setPalette(p);
    QFile file(":/theme/widgets.qss");
    if (file.open(QIODevice::ReadOnly)) {
        auto stylesheet = QString::fromUtf8(file.readAll());
        if (profile.controls.id == "compact")
            stylesheet +=
                "\nQCheckBox[fluentToggle=\"true\"]::indicator {width:13px;height:13px;"
                "image:none;background:@surface.hover@;border:1px solid @check.border@;border-radius:2px;}"
                "QCheckBox[fluentToggle=\"true\"]::indicator:checked {background:@accent@;"
                "border-color:@accent@;image:url(\":/assets/check.svg\");}";
        if (themeId_ == "chromatic") {
            QFile extra(":/theme/chromatic.qss");
            if (extra.open(QIODevice::ReadOnly))
                stylesheet += QString::fromUtf8(extra.readAll());
        }
        if (themeId_ == "light") {
            QFile extra(":/theme/light.qss");
            if (extra.open(QIODevice::ReadOnly))
                stylesheet += QString::fromUtf8(extra.readAll());
        }
        for (auto it = colors.begin(); it != colors.end(); ++it)
            stylesheet.replace("@" + it.key() + "@", it.value().name());
        stylesheet.replace("@asset.downArrow@", dark ? "down-dark" : "down");
        stylesheet.replace("@asset.stepMode@", dark ? "dark" : "light");
        stylesheet.replace("@controls.radius@", QString::number(profile.controls.radius));
        stylesheet.replace("@controls.paddingY@", QString::number(profile.controls.paddingY));
        stylesheet.replace("@controls.tabPaddingY@", QString::number(profile.controls.tabPaddingY));
        qApp->setStyleSheet(stylesheet);
    }
    QSettings().setValue("appearance/theme", themeId_);
    QSettings().setValue("appearance/dark", dark);
    emit changed(dark);
}
QIcon IconManager::icon(const QString& name, bool dark, const QColor& color, bool filled) {
    QFile file(":/icons/" + name + ".svg");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    auto svg = file.readAll();
    if (filled)
        svg.replace("fill=\"none\"", "fill=\"currentColor\"");
    svg.replace("currentColor",
                color.isValid() ? color.name().toUtf8() : DesignTokens::forMode(dark).text.name().toUtf8());
    QSvgRenderer renderer(svg);
    QIcon result;
    for (int size : {20, 24, 32, 48}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        renderer.render(&painter);
        result.addPixmap(pixmap);
    }
    return result;
}
QIcon IconManager::appIcon() {
    // Share the exact ICO with the executable resource, application and windows.
    // This prevents taskbar icons from drifting from the selected artwork.
    static const QIcon cached(":/assets/PortWave.ico");
    return cached;
}

} // namespace portwave
