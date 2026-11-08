#include "ui/main_window.h"
#include "ui/theme.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("PortWaveCpp");
    QApplication::setApplicationName("PortWave");
    QApplication::setApplicationVersion(PORTWAVE_VERSION);
    QApplication::setStyle("Fusion");
    // Set before any window exists so the taskbar, Alt+Tab and pinned shortcuts
    // all use the same mark instead of the generic placeholder.
    QApplication::setWindowIcon(portwave::IconManager::appIcon());
    QCommandLineParser parser;
    parser.setApplicationDescription("PortWave C++20 / Qt 6 serial terminal");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"demo", "Create two simulated connections"});
    parser.addOption({"smoke-test", "Show the actual window, then close cleanly after 2 seconds"});
    parser.addPositionalArgument("file", "UTF-8 log snapshot to open");
    parser.process(app);
    std::unique_ptr<QTemporaryDir> smokeDirectory;
    if (parser.isSet("smoke-test")) {
        smokeDirectory = std::make_unique<QTemporaryDir>();
        QApplication::setOrganizationName("PortWaveCppSmoke");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, smokeDirectory->path());
        qputenv("PORTWAVE_TEST_LOG_DIR", smokeDirectory->path().toUtf8());
    }
    portwave::SettingsRepository settings;
    portwave::ThemeManager theme;
    theme.applyTheme(theme.themeId());
    portwave::MainWindow main(settings, theme);
    main.show();
    if (parser.isSet("demo")) {
        main.addTestConnection();
        main.addTestConnection();
    }
    if (!parser.positionalArguments().isEmpty())
        main.openFile(parser.positionalArguments().first());
    if (parser.isSet("smoke-test"))
        QTimer::singleShot(2000, &main, &QWidget::close);
    return app.exec();
}
