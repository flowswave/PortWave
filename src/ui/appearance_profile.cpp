#include "ui/appearance_profile.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <cmath>

namespace portwave {
ControlStyle ControlStyle::fromId(const QString& id) {
    if (id == "compact")
        return {id, 3, 2, 4, 30, 24};
    return {};
}
QString validatedThemeId(const QString& id) {
    return id == "light" ? id : "chromatic";
}
AppearanceProfile AppearanceProfile::load(const QString& requested) {
    QSettings settings;
    const auto fallback = settings.contains("appearance/dark") && !settings.value("appearance/dark").toBool()
                              ? QString("light")
                              : QString("chromatic");
    const auto id = requested.isEmpty()
                        ? validatedThemeId(settings.value("appearance/theme", fallback).toString())
                        : validatedThemeId(requested);
    const QString prefix = "appearance/" + id + "/";
    return {
        id != "light",
        ControlStyle::fromId(
            settings.value(prefix + "controls", settings.value("appearance/controls", "compact")).toString()),

        "keep", id};
}
QMap<QString, QColor> resolveThemeColors(bool dark, bool userOverrides) {
    return resolveThemeColors(QString(dark ? "dark" : "light"), userOverrides);
}
QMap<QString, QColor> resolveThemeColors(const QString& requested, bool userOverrides) {
    const auto id = validatedThemeId(requested);
    QFile file(":/appearance/colors.json");
    QMap<QString, QColor> result;
    if (file.open(QIODevice::ReadOnly)) {
        const auto object = QJsonDocument::fromJson(file.readAll()).object().value(id).toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            const QColor color(it.value().toString());
            if (color.isValid())
                result.insert(it.key(), color);
        }
    }
    const QString prefix = "appearance/" + id + "/";
    const QColor accent(QSettings().value(prefix + "active").toString());
    const auto linear = [](double value) {
        return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4);
    };
    const double accentLuminance =
        .2126 * linear(accent.redF()) + .7152 * linear(accent.greenF()) + .0722 * linear(accent.blueF());
    // Light controls must stay distinguishable from the reading surface, even
    // when an older profile saved a near-white accent. Keep the saved value intact.
    const bool visibleAccent = id != "light" || 1.05 / (accentLuminance + .05) >= 3.0;
    if (userOverrides && accent.isValid() && visibleAccent) {
        const bool custom = accent != result.value("accent");
        result["accent"] = accent;
        if (custom || id != "chromatic")
            result["button.primaryBackground"] = accent;
    }
    const auto selected = result.value("button.primaryBackground");
    const auto luminance = .2126 * linear(selected.redF()) + .7152 * linear(selected.greenF()) +
                           .0722 * linear(selected.blueF());
    result["button.primaryForeground"] = QColor(luminance > .179 ? "#000000" : "#ffffff");
    result["button.primaryEnd"] = luminance > .179 ? selected.lighter(110) : selected.darker(125);
    return result;
}
} // namespace portwave
