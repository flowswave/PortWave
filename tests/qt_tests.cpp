#include "editor/scintilla_editor.h"
#include "features/connection_tabs.h"
#include "features/find_dialog.h"
#include "features/snap_layout.h"
#include "features/stable_result_delegate.h"
#include "features/terminal_view.h"
#include "features/window_caption.h"
#include "infrastructure/log_writer.h"
#include "infrastructure/named_layout_store.h"
#include "infrastructure/workspace_archive.h"
#include "services/search_service.h"
#include "services/session.h"
#include "ui/appearance_profile.h"
#include "ui/connection_dialog.h"
#include "ui/keyboard_shortcuts.h"
#include "ui/main_window.h"
#include "ui/settings_dialog.h"
#include <QCheckBox>
#include <QClipboard>
#include <QCursor>
#include <QDateTimeEdit>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QFontInfo>
#include <QJsonDocument>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPaintEngine>
#include <QPainter>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyleOptionButton>
#include <QStyleOptionSpinBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <Qsci/qsciscintilla.h>
#include <QtConcurrent>
#include <cmath>
#include <miniz.h>
#include <miniz_zip.h>
#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#undef small // Windows RPC macro conflicts with the existing small-document test variable.
#endif

using namespace portwave;
// Observe actual delegate text draw calls without comparing background-dependent
// antialiasing blends. Selection may change color, never glyph placement/font.
class TextProbe final : public QPaintDevice {
  public:
    struct Draw {
        QPointF origin;
        QFont font;
        QString text;
        bool operator==(const Draw&) const = default;
    };
    class Engine final : public QPaintEngine {
      public:
        QVector<Draw> draws;
        Engine() : QPaintEngine(QPaintEngine::AllFeatures) {}
        bool begin(QPaintDevice*) override {
            setActive(true);
            return true;
        }
        bool end() override {
            setActive(false);
            return true;
        }
        void updateState(const QPaintEngineState&) override {}
        Type type() const override { return QPaintEngine::User; }
        void drawPixmap(const QRectF&, const QPixmap&, const QRectF&) override {}
        void drawPath(const QPainterPath&) override {}
        void drawTextItem(const QPointF& position, const QTextItem& item) override {
            draws.push_back({position, item.font(), item.text()});
        }
    };
    mutable Engine engine;
    QPaintEngine* paintEngine() const override { return &engine; }

  protected:
    int metric(PaintDeviceMetric metric) const override {
        switch (metric) {
        case PdmWidth:
            return 360;
        case PdmHeight:
            return 40;
        case PdmDpiX:
        case PdmDpiY:
        case PdmPhysicalDpiX:
        case PdmPhysicalDpiY:
            return 96;
        case PdmDepth:
            return 32;
        case PdmDevicePixelRatio:
            return 1;
        case PdmDevicePixelRatioScaled:
            return int(QPaintDevice::devicePixelRatioFScale());
        default:
            return 0;
        }
    }
};
class LogUrlRecorder final : public QObject {
    Q_OBJECT
  public:
    QList<QUrl> urls;
  public slots:
    void record(const QUrl& url) { urls.push_back(url); }
};
class Integration final : public QObject {
    Q_OBJECT
    QTemporaryDir testData_;
  private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("PortWaveCppTests");
        QCoreApplication::setApplicationName("Integration");
        QApplication::setStyle("Fusion");
        QStandardPaths::setTestModeEnabled(true);
        QFontDatabase::addApplicationFont("C:/Windows/Fonts/consola.ttf");
        QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
        qInfo() << "Font inventory" << QFontDatabase::families().filter("Consolas")
                << QFontDatabase::families().filter("YaHei") << "resolved"
                << QFontInfo(QFont("Consolas", 8)).family()
                << QFontInfo(QFont("Microsoft YaHei UI", 9)).family();
        qputenv("PORTWAVE_TEST_LOG_DIR", testData_.path().toUtf8());
    }
    void init() {
        QSettings().remove("logs/open");
        QSettings().remove("logs/workspaces_v2");
        QSettings().remove("appearance");
        QSettings().remove("logs/appearance");
        QSettings().remove("logs/window_on_top");
        QSettings().setValue("sender/pinned", true);
    }
    void bottomStatusPositionsMatchAcrossThemes() {
        SettingsRepository repository;
        ThemeManager theme;
        theme.applyTheme("chromatic");
        MainWindow main(repository, theme);
        main.resize(1100, 740);
        main.show();
        main.addTestConnection();
        auto* pane = main.findChild<TerminalPane*>();
        QVERIFY(pane);
        const auto cleanup = qScopeGuard([&] {
            main.close();
            QTest::qWait(200);
        });
        QTRY_VERIFY(pane->session()->connected());
        auto* label = pane->fileStatusWidget();
        QTRY_COMPARE(label->cursor().shape(), Qt::PointingHandCursor);
        auto* status = main.findChild<QLabel*>("mainConnectionStatus");
        const QString path = "D:/Logs/test-current.log";
        pane->session()->logPathChanged(path);
        QTest::qWait(50);
        const int chromaticRight = label->mapTo(&main, QPoint(label->width(), 0)).x();
        const int chromaticStatusLeft = status->mapTo(&main, QPoint(0, 0)).x();
        for (const auto& id : {QString("light"), QString("chromatic")}) {
            theme.applyTheme(id);
            QCoreApplication::processEvents();
            QCOMPARE(label->alignment(), Qt::AlignRight | Qt::AlignVCenter);
            QCOMPARE(label->text(), QString("test-current.log"));
            QVERIFY(label->toolTip().startsWith(path));
            QVERIFY(status->text().contains("已连接"));
            QVERIFY(status->toolTip().contains("接收模式"));
            QCOMPARE(status->mapTo(&main, QPoint(0, 0)).x(), chromaticStatusLeft);
            QVERIFY(qAbs(label->mapTo(&main, QPoint(label->width(), 0)).x() - chromaticRight) <= 1);
            const auto preview = qEnvironmentVariable("PORTWAVE_PREVIEWS");
            if (!preview.isEmpty()) {
                QDir().mkpath(preview);
                main.grab().save(preview + "/bottom-status-" + id + ".png");
            }
        }
        main.close();
        QTRY_VERIFY_WITH_TIMEOUT(!main.isVisible(), 5000);
    }
    void lightButtonsAndCompactTogglesHaveNormalSize() {
        SettingsRepository repository;
        const auto previous = repository.loadConnections();
        QString error;
        const auto cleanup = qScopeGuard([&] { repository.saveConnections(previous, error); });
        ConnectionConfig config;
        config.id = "button-size";
        config.kind = "tcp";
        QVERIFY(repository.saveConnections({config}, error));
        ThemeManager theme;
        for (const auto& control : {QString("standard"), QString("compact")}) {
            QSettings().setValue("appearance/light/controls", control);
            theme.applyTheme("light");
            SettingsDialog settings(repository, theme, {}, nullptr, nullptr, "发送与命令");
            settings.resize(1100, 800);
            settings.show();
            QCoreApplication::processEvents();
            for (auto* box : settings.findChildren<QCheckBox*>()) {
                if (!box->property("fluentToggle").toBool())
                    continue;
                box->ensurePolished();
                for (const bool checked : {false, true}) {
                    const QSignalBlocker blocker(box);
                    box->setChecked(checked);
                    QStyleOptionButton option;
                    option.initFrom(box);
                    const auto indicator =
                        box->style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, box);
                    QCOMPARE(indicator.size(), QSize(40, 22));
                }
            }
            for (auto* category : settings.findChildren<QPushButton*>("settingsCategory")) {
                category->click();
                QCoreApplication::processEvents();
                for (auto* button : settings.findChildren<QPushButton*>())
                    if (button->isVisible())
                        QVERIFY2(button->height() >= 30, qPrintable(button->text()));
            }
            // Capture the user's page under both saved control styles.
            for (auto* category : settings.findChildren<QPushButton*>("settingsCategory"))
                if (category->text() == "发送与命令")
                    category->click();
            const auto preview = qEnvironmentVariable("PORTWAVE_PREVIEWS");
            if (!preview.isEmpty()) {
                QDir().mkpath(preview);
                QTest::qWait(50);
                settings.grab().save(preview + "/send-buttons-" + control + ".png");
            }
            settings.reject();
            MainWindow main(repository, theme);
            main.resize(1100, 740);
            main.show();
            QCoreApplication::processEvents();
            for (auto* toolbar : main.findChildren<QToolBar*>())
                for (auto* button : toolbar->findChildren<QToolButton*>())
                    if (button->isVisible()) {
                        QVERIFY(button->height() >= 26);
                        QVERIFY(button->height() <= toolbar->height());
                    }
            if (!preview.isEmpty())
                main.grab().save(preview + "/main-buttons-" + control + ".png");
            main.close();
        }
    }
    void lightContrastAndChromaticIsolation() {
        ThemeManager theme;
        theme.applyTheme("chromatic");
        const auto chromaticColors = resolveThemeColors(QString("chromatic"));
        const auto chromaticStyle = qApp->styleSheet();
        const auto chromaticSelection = qApp->palette().color(QPalette::Highlight);
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        auto editor = makeEditor(&host);
        layout->addWidget(editor->widget());
        auto* native = qobject_cast<QsciScintilla*>(editor->widget());
        editor->setText("[WARN] temperature = 72.3 C\n[INFO] temperature = 71.2 C\n");
        editor->selectRange(7, 11);
        const auto pointer = native->SendScintillaPtrResult(QsciScintillaBase::SCI_GETDOCPOINTER);
        const auto text = editor->text();
        QSettings().setValue("appearance/light/active", "#f7e8fa");
        theme.applyTheme("light");
        QCOMPARE(resolveThemeColors(QString("light")).value("accent"), QColor("#0c66e4"));
        QCOMPARE(QSettings().value("appearance/light/active").toString(), QString("#f7e8fa"));
        QCOMPARE(qApp->palette().color(QPalette::Highlight), QColor("#0052cc"));
        QCOMPARE(qApp->palette().color(QPalette::HighlightedText), QColor(Qt::white));
        editor->setTheme(false);
        editor->setAppearance(QFont("Consolas", 12), QColor("#fbfcfe"), QColor("#172b4d"));
        editor->highlightMatches({{34, 11, 0, false}}, false);
        QCOMPARE(native->SendScintilla(QsciScintillaBase::SCI_INDICGETALPHA, 16), 170L);
        QCOMPARE(editor->selection(), (std::pair<qsizetype, qsizetype>{7, 18}));
        SearchDocument document;
        document.id = "contrast-preview";
        document.path = "t3";
        for (int i = 0; i < 8; ++i)
            document.text +=
                QString("[21:37:%1] t3 sensor: [WARN] temperature = %2 C\n").arg(40 + i).arg(72.0 + i / 10.0);
        FindDialog dialog([document](bool) { return QVector<SearchDocument>{document}; },
                          [](const SearchHit&) { return true; }, true, "search/cache/contrast-preview");
        dialog.show();
        dialog.findChild<QComboBox*>("findQuery")->setEditText("temperature");
        dialog.searchAll(false);
        auto* results = dialog.findChild<QTreeWidget*>();
        QTRY_COMPARE(results->topLevelItemCount(), 8);
        results->topLevelItem(2)->setSelected(true);
        const auto preview = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        if (!preview.isEmpty()) {
            QDir().mkpath(preview);
            dialog.resize(1000, 650);
            QTest::qWait(80);
            dialog.grab().save(preview + "/search-light-contrast.png");
            host.resize(750, 360);
            host.show();
            native->setFocus();
            native->SendScintilla(QsciScintillaBase::SCI_SETXOFFSET, 0UL);
            QTest::qWait(80);
            host.grab().save(preview + "/word-selection-light.png");
        }
        theme.applyTheme("chromatic");
        editor->setTheme(true);
        QCOMPARE(resolveThemeColors(QString("chromatic")), chromaticColors);
        QCOMPARE(qApp->styleSheet(), chromaticStyle);
        QCOMPARE(qApp->palette().color(QPalette::Highlight), chromaticSelection);
        QCOMPARE(native->SendScintilla(QsciScintillaBase::SCI_INDICGETALPHA, 16), 255L);
        QCOMPARE(native->SendScintilla(QsciScintillaBase::SCI_INDICGETOUTLINEALPHA, 16), 50L);
        QCOMPARE(native->SendScintillaPtrResult(QsciScintillaBase::SCI_GETDOCPOINTER), pointer);
        QCOMPARE(editor->text(), text);
        QCOMPARE(editor->selection(), (std::pair<qsizetype, qsizetype>{7, 18}));
        dialog.close();
    }
    void lightReaderMigratesLegacyBackgroundAndPreservesDocuments() {
        QSettings settings;
        settings.setValue("logs/appearance/background", "#282c34");
        settings.setValue("logs/appearance/light/background", "#282c34");
        settings.setValue("logs/appearance/chromatic/background", "#08131f");
        ThemeManager theme;
        QVERIFY(!settings.contains("logs/appearance/light/background"));
        theme.applyTheme("chromatic");
        QApplication::setWindowIcon(IconManager::appIcon());
        SettingsRepository repository;
        MainWindow main(repository, theme);
        auto* logs = main.findChild<LogWorkspace*>();
        QVERIFY(logs);
        logs->addExample();
        auto* native = logs->findChild<QsciScintilla*>();
        QVERIFY(native);
        const auto pointer = native->SendScintillaPtrResult(QsciScintillaBase::SCI_GETDOCPOINTER);
        const auto text = native->text();
        const auto paper = [native] {
            const auto bgr =
                native->SendScintilla(QsciScintillaBase::SCI_STYLEGETBACK, QsciScintillaBase::STYLE_DEFAULT);
            return QColor(bgr & 255, (bgr >> 8) & 255, (bgr >> 16) & 255);
        };
        QCOMPARE(paper(), QColor("#08131f"));
        theme.applyTheme("light");
        QCOMPARE(paper(), QColor("#fbfcfe"));
        QCOMPARE(native->SendScintillaPtrResult(QsciScintillaBase::SCI_GETDOCPOINTER), pointer);
        QCOMPARE(native->text(), text);
        logs->addExample();
        for (auto* editor : logs->findChildren<QsciScintilla*>()) {
            // paper() caches the non-lexer color; inspect the actual rendered styles.
            for (const int style : {0, int(QsciScintillaBase::STYLE_DEFAULT)}) {
                const auto bgr = editor->SendScintilla(QsciScintillaBase::SCI_STYLEGETBACK, style);
                QCOMPARE(QColor(bgr & 255, (bgr >> 8) & 255, (bgr >> 16) & 255), QColor("#fbfcfe"));
            }
        }
        const auto preview = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        if (!preview.isEmpty()) {
            QDir().mkpath(preview);
            main.resize(1100, 720);
            main.show();
            logs->resize(1000, 700);
            logs->show();
            QTest::qWait(100);
            main.grab().save(preview + "/main-light.png");
            logs->grab().save(preview + "/log-light.png");
        }
        settings.setValue("logs/appearance/light/background", "#f5f0df");
        ThemeManager reopened;
        QCOMPARE(settings.value("logs/appearance/light/background").toString(), QString("#f5f0df"));
        main.close();
        logs->close();
    }
    void freshProfileDefaultsToChromaticAndTwelvePoint() {
        QSettings settings;
        settings.remove("appearance");
        settings.remove("appearance/dark");
        QCOMPARE(AppearanceProfile{}.themeId, QString("chromatic"));
        QCOMPARE(AppearanceProfile::load().controls.id, QString("compact"));
        QCOMPARE(AppearanceProfile::load("light").controls.id, QString("compact"));
        QCOMPARE(AppearanceProfile::load("chromatic").controls.id, QString("compact"));
        settings.setValue("appearance/light/controls", "standard");
        QCOMPARE(AppearanceProfile::load("light").controls.id, QString("standard"));
        settings.remove("appearance/light/controls");
        ThemeManager theme;
        QCOMPARE(theme.themeId(), QString("chromatic"));
        theme.applyTheme(theme.themeId());
        SettingsRepository repository;
        SettingsDialog dialog(repository, theme, {}, nullptr, nullptr, "日志文件");
        QCOMPARE(dialog.findChild<QSpinBox*>("terminalReadingSize")->value(), 12);
        QCOMPARE(dialog.findChild<QSpinBox*>("logReadingSize")->value(), 12);
        const auto preview = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        if (!preview.isEmpty()) {
            QDir().mkpath(preview);
            dialog.show();
            QTest::qWait(80);
            dialog.grab().save(preview + "/log-file-settings-chromatic.png");
        }
        settings.setValue("appearance/theme", "light");
        settings.setValue("appearance/light/size", 15);
        settings.setValue("logs/appearance/light/size", 16);
        ThemeManager saved;
        QCOMPARE(saved.themeId(), QString("light"));
        SettingsDialog restored(repository, saved, {}, nullptr);
        QCOMPARE(restored.findChild<QSpinBox*>("terminalReadingSize")->value(), 15);
        QCOMPARE(restored.findChild<QSpinBox*>("logReadingSize")->value(), 16);
    }
    void logOpenPreferencesAreStaged() {
        SettingsRepository repository;
        const auto previous = repository.loadConnections();
        QString error;
        QVERIFY(repository.saveConnections({}, error));
        const auto restore = qScopeGuard([&] { repository.saveConnections(previous, error); });
        ThemeManager theme;
        theme.applyTheme("chromatic");
        {
            SettingsDialog settings(repository, theme, {}, nullptr, nullptr, "日志文件");
            settings.findChild<QRadioButton*>("logOpenExternal")->setChecked(true);
            QCOMPARE(repository.loadLogOpenPreferences().action, LogOpenAction::PortWave);
            settings.reject();
        }
        QCOMPARE(repository.loadLogOpenPreferences().action, LogOpenAction::PortWave);
        {
            SettingsDialog settings(repository, theme, {}, nullptr, nullptr, "日志文件");
            settings.findChild<QRadioButton*>("logOpenExternal")->setChecked(true);
            auto* path = settings.findChild<QLineEdit*>("logOpenProgramPath");
            QVERIFY(!path->isEnabled());
            for (auto* button : settings.findChildren<QPushButton*>("logOpenProgramChoice"))
                if (button->property("choiceIndex").toInt() == 1)
                    button->click();
            QVERIFY(path->isEnabled());
            path->setText(testData_.filePath("missing-program.exe"));
            QVERIFY(!settings.apply());
            QCOMPARE(repository.loadLogOpenPreferences().action, LogOpenAction::PortWave);
            path->setText(QCoreApplication::applicationFilePath());
            QVERIFY(settings.apply());
        }
        auto saved = repository.loadLogOpenPreferences();
        QCOMPARE(saved.action, LogOpenAction::ExternalFile);
        QVERIFY(saved.customProgram);
        QCOMPARE(saved.program, QCoreApplication::applicationFilePath());
        SettingsDialog restored(repository, theme, {}, nullptr, nullptr, "日志文件");
        QVERIFY(restored.findChild<QRadioButton*>("logOpenExternal")->isChecked());
        QCOMPARE(restored.findChild<QLineEdit*>("logOpenProgramPath")->text(), saved.program);
        restored.findChild<QRadioButton*>("logOpenFolder")->setChecked(true);
        QVERIFY(restored.apply());
        QCOMPARE(repository.loadLogOpenPreferences().action, LogOpenAction::Folder);
    }
    void logDoubleClickUsesSavedAction() {
        SettingsRepository repository;
        LogUrlRecorder recorder;
        QDesktopServices::setUrlHandler("file", &recorder, "record");
        const auto cleanup = qScopeGuard([] { QDesktopServices::unsetUrlHandler("file"); });
        QTemporaryDir directory;
        QFile file(directory.filePath("日志 with spaces.log"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("test\n");
        file.close();
        ConnectionConfig config;
        config.id = "log-open-routing";
        config.kind = "test";
        Session session(config);
        TerminalPane pane(&session);
        session.logPathChanged(file.fileName());
        auto* label = pane.fileStatusWidget();
        QSignalSpy internal(&pane, &TerminalPane::openLog);
        QSignalSpy status(&pane, &TerminalPane::status);
        const auto doubleClick = [&] { QTest::mouseDClick(label, Qt::LeftButton); };
        doubleClick();
        QCOMPARE(internal.size(), 1);
        QString error;
        LogOpenPreferences preferences;
        preferences.action = LogOpenAction::ExternalFile;
        QVERIFY(repository.saveLogOpenPreferences(preferences, error));
        doubleClick();
        QCOMPARE(recorder.urls.size(), 1);
        QCOMPARE(recorder.urls.last().toLocalFile(), file.fileName());
        preferences.action = LogOpenAction::Folder;
        QVERIFY(repository.saveLogOpenPreferences(preferences, error));
        doubleClick();
        QCOMPARE(recorder.urls.last().toLocalFile(), directory.path());
        preferences.action = LogOpenAction::ExternalFile;
        preferences.customProgram = true;
        preferences.program = directory.filePath("missing.exe");
        QVERIFY(repository.saveLogOpenPreferences(preferences, error));
        doubleClick();
        QCOMPARE(status.size(), 1);
        QCOMPARE(recorder.urls.size(), 2);
        QCOMPARE(internal.size(), 1);
        preferences.program.clear();
        QVERIFY(repository.saveLogOpenPreferences(preferences, error));
        doubleClick();
        QCOMPARE(recorder.urls.size(), 3);
        QCOMPARE(recorder.urls.last().toLocalFile(), file.fileName());
    }
    void logWindowEdgesAndGeometry() {
        QRect geometry;
        {
            LogWorkspace workspace;
            workspace.showNormal();
            workspace.setGeometry(20, 30, 620, 460);
            QTest::qWait(50);
            auto* frame = workspace.findChild<QWidget*>("windowResizeFrame", Qt::FindDirectChildrenOnly);
            QVERIFY(frame && frame->isVisible());
            const auto mask = frame->mask();
            const int right = frame->width() - 1, bottom = frame->height() - 1;
            for (const auto& point : {QPoint(0, 0), QPoint(right / 2, 0), QPoint(right, 0),
                                      QPoint(0, bottom / 2), QPoint(right, bottom / 2), QPoint(0, bottom),
                                      QPoint(right / 2, bottom), QPoint(right, bottom)})
                QVERIFY(mask.contains(point));
            QVERIFY(!mask.contains(frame->rect().center()));
            geometry = workspace.geometry();
            workspace.close();
        }
        LogWorkspace restored;
        restored.show();
        QTest::qWait(30);
        QCOMPARE(restored.geometry(), geometry);
        restored.close();
    }
    void logOpenResponsiveness() {
        QTemporaryDir directory;
        QFile file(directory.filePath("ansi-load.log"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray line = "[12:34:56.789] module \x1b[33mWARN temperature sample UTF-8 "
                                "\xe4\xb8\xad\xe6\x96\x87 \x1b[0m serial receive stress sample\r\n";
        for (int i = 0; i < 180000; ++i)
            QCOMPARE(file.write(line), qint64(line.size()));
        file.close();
        LogDocumentView view;
        view.show();
        QTest::qWait(100);
        auto* tabs = view.findChild<QTabWidget*>();
        QElapsedTimer elapsed, pulse;
        elapsed.start();
        pulse.start();
        qint64 longest = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, &view, [&] { longest = qMax(longest, pulse.restart()); });
        heartbeat.start(1);
        view.openPath(file.fileName());
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 30000);
        const auto opened = elapsed.elapsed();
        qInfo() << "attach stages" << tabs->currentWidget()->property("loadAdoptMs")
                << tabs->currentWidget()->property("loadOptionsMs")
                << tabs->currentWidget()->property("loadStyleMs")
                << tabs->currentWidget()->property("loadPublishMs");
        QTest::qWait(1000);
        qInfo("log-open bytes=%lld ready-ms=%lld max-gui-gap-ms=%lld", file.size(), opened, longest);
        QVERIFY(
            view.searchSources(false).front().text.contains(QString::fromUtf8("\xe4\xb8\xad\xe6\x96\x87")));
    }
    void chromaticThemeIsGlobalAndPreservesSessions() {
        ThemeManager theme;
        QSettings().setValue("appearance/controls", "compact");
        QSettings().setValue("appearance/motion", "off");
        theme.applyTheme("chromatic");
        QCOMPARE(theme.themeId(), QString("chromatic"));
        QVERIFY(theme.dark());
        QVERIFY(!qApp->styleSheet().contains('@'));
        SettingsRepository repository;
        SettingsDialog settings(repository, theme, {}, nullptr);
        auto* themeChoice = settings.findChild<QComboBox*>("appearanceTheme");
        auto* controls = settings.findChild<QComboBox*>("appearanceControls");
        QCOMPARE(themeChoice->count(), 3);
        QVERIFY(!settings.findChild<QComboBox*>("appearancePreset"));
        controls->setCurrentIndex(0);
        themeChoice->setCurrentIndex(1);
        QCOMPARE(controls->currentData().toString(), QString("compact"));
        themeChoice->setCurrentIndex(2);
        QCOMPARE(controls->currentData().toString(), QString("standard"));
        QVERIFY(settings.apply());
        QCOMPARE(theme.themeId(), QString("chromatic"));
        ThemeManager restored;
        QCOMPARE(restored.themeId(), QString("chromatic"));
        MainWindow window(repository, theme);
        const auto cleanup = qScopeGuard([&] {
            window.close();
            QTest::qWait(200);
        });
        window.resize(1280, 820);
        window.show();
        window.addTestConnection();
        window.addTestConnection();
        auto* area = window.findChild<QMdiArea*>();
        auto* pane = qobject_cast<TerminalPane*>(area->activeSubWindow()->widget());
        auto* draft = pane->controlsWidget()->findChild<QPlainTextEdit*>("sendEdit");
        draft->setPlainText("theme draft preserved");
        QTRY_COMPARE(pane->session()->config().sendDraft, QString("theme draft preserved"));
        auto* document = pane->terminal()->document();
        const auto config = pane->session()->config().toJson();
        auto* active = area->activeSubWindow();
        auto* toolbar = window.findChild<QToolBar*>();
        for (const auto& id : {"light", "dark", "chromatic"}) {
            theme.applyTheme(id);
            QCoreApplication::processEvents();
            QCOMPARE(theme.profile().controls.id,
                     QString(id == QString("chromatic") ? "standard" : "compact"));
            QCOMPARE(pane->terminal()->document(), document);
            QCOMPARE(pane->session()->config().toJson(), config);
            QCOMPARE(draft->toPlainText(), QString("theme draft preserved"));
            QCOMPARE(area->activeSubWindow(), active);
        }
        QCOMPARE(toolbar->parentWidget()->objectName(), QString("shellHeader"));
        QCOMPARE(toolbar->toolButtonStyle(), Qt::ToolButtonIconOnly);
        QVERIFY(!window.findChild<QWidget*>("windowGlowOutline"));
        QVERIFY(!pane->property("activeConnection").toBool());
        const auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        if (!destination.isEmpty()) {
            QTest::qWait(100);
            window.grab().save(destination + "/chromatic-main.png");
            for (auto* button : settings.findChildren<QPushButton*>())
                if (button->text() == "颜色与字体")
                    button->click();
            settings.resize(1100, 780);
            settings.show();
            QCoreApplication::processEvents();
            settings.grab().save(destination + "/chromatic-settings.png");
            settings.hide();
            pane->find();
            auto* finder = pane->findChild<FindDialog*>();
            QCoreApplication::processEvents();
            finder->grab().save(destination + "/chromatic-search.png");
            finder->close();
        }
        window.close();
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
    }
    void appearanceLayersAndTabMotion() {
        ThemeManager theme;
        QSettings().setValue("appearance/motion", "off");
        QSettings().setValue("appearance/dark/motion", "off");
        theme.apply(true);
        SettingsRepository repository;
        SettingsDialog settings(repository, theme, {}, nullptr);
        QVERIFY(!settings.findChild<QComboBox*>("appearanceMotion"));
        ConnectionTabs tabs;
        tabs.setObjectName("connectionTabs");
        tabs.setExpanding(false);
        tabs.resize(600, 48);
        tabs.addTab("连接一");
        tabs.addTab("连接名称比较长的二");
        tabs.addTab("三");
        tabs.show();
        QCoreApplication::processEvents();
        auto* animation = tabs.findChild<QVariantAnimation*>();
        auto* indicator = tabs.findChild<QWidget*>("connectionTabIndicator");
        QVERIFY(animation && indicator);
        const auto start = indicator->geometry();
        tabs.setCurrentIndex(1);
        QCOMPARE(animation->duration(), 150);
        QCOMPARE(animation->state(), QAbstractAnimation::Running);
        QCOMPARE(tabs.currentIndex(), 1);
        QCOMPARE(indicator->geometry(), start);
        animation->setCurrentTime(80);
        QVERIFY(indicator->geometry() != start);
        const auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        if (!destination.isEmpty())
            tabs.grab().save(destination + "/tab-motion-middle.png");
        const auto interrupted = indicator->geometry();
        tabs.setCurrentIndex(0);
        QCOMPARE(indicator->geometry(), interrupted);
        tabs.setCurrentIndex(2);
        QTRY_COMPARE(animation->state(), QAbstractAnimation::Stopped);
        QCOMPARE(indicator->geometry(), tabs.tabRect(2).adjusted(2, 2, -2, -2));
        tabs.setCurrentIndex(1);
        tabs.resize(650, 48);
        QCOMPARE(animation->state(), QAbstractAnimation::Stopped);
        QCOMPARE(indicator->geometry(), tabs.tabRect(1).adjusted(2, 2, -2, -2));
        tabs.hide();
        tabs.show();
        QVERIFY(indicator->isVisible());
    }
    void twoThemesAndConnectDisconnect() {
        QCOMPARE(validatedThemeId("dark"), QString("chromatic"));
        QCOMPARE(validatedThemeId("light"), QString("light"));
        ThemeManager theme;
        theme.applyTheme("chromatic");
        SettingsRepository repository;
        MainWindow window(repository, theme);
        const auto cleanup = qScopeGuard([&] {
            window.close();
            QTest::qWait(200);
        });
        window.addTestConnection();
        auto* pane = qobject_cast<TerminalPane*>(window.findChild<QMdiArea*>()->activeSubWindow()->widget());
        QTRY_VERIFY(pane->session()->connected());
        auto* power = window.findChild<QAction*>("toolbar_power");
        power->trigger();
        QTRY_VERIFY(!pane->session()->connected());
        QTRY_VERIFY(power->isEnabled());
        power->trigger();
        QTRY_VERIFY(pane->session()->connected());
        QVERIFY(!pane->receivePaused());
        auto* toolbar = window.findChild<QToolBar*>();
        QCOMPARE(toolbar->widgetForAction(power)->contextMenuPolicy(), Qt::DefaultContextMenu);
    }
    void windowBoundsPreserveAndRecover() {
        QWidget window;
        window.setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
        decorateWindow(&window);
        const QRect area(100, 50, 1280, 690);
        window.setGeometry(120, 80, 900, 600);
        fitWindowToScreen(&window, area);
        QCOMPARE(window.geometry(), QRect(120, 80, 900, 600));
        window.setGeometry(3000, 2000, 1500, 900);
        fitWindowToScreen(&window, area);
        QCOMPARE(window.geometry(), area);
        window.setGeometry(-5000, -5000, 900, 600);
        window.show();
        QCoreApplication::processEvents();
        QVERIFY(window.screen()->availableGeometry().contains(window.frameGeometry()));
        window.showMaximized();
        QCoreApplication::processEvents();
        fitWindowToScreen(&window, area);
        QVERIFY(window.isMaximized());
    }
    void fullHdWindowFit() {
        const auto scale = QApplication::primaryScreen()->devicePixelRatio();
        const QSize budget(qFloor(1920 / scale), qFloor(1040 / scale));
        ThemeManager theme;
        theme.applyTheme("chromatic");
        QSettings().remove("window/geometry");
        QSettings().remove("dialogs/settings/geometry");
        QSettings().remove("search/windows");
        SettingsRepository repository;
        MainWindow window(repository, theme);
        const auto cleanup = qScopeGuard([&] {
            window.close();
            QTest::qWait(200);
        });
        window.addTestConnection();
        window.show();
        const auto check = [&](QWidget& widget, const QString& name) {
            qInfo() << name << "DPR" << scale << "default" << widget.size() << "physical"
                    << widget.size() * scale << "1080p-default-fit"
                    << (widget.width() <= budget.width() && widget.height() <= budget.height());
            fitWindowToScreen(&widget, QRect(QPoint(0, 0), budget));
            widget.show();
            QCoreApplication::processEvents();
            QVERIFY(widget.width() <= budget.width() && widget.height() <= budget.height());
            const auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
            if (!destination.isEmpty())
                widget.grab().save(destination + "/" + name + ".png");
        };
        check(window, "main");
        SettingsDialog settings(repository, theme, {}, window.findChild<LogWorkspace*>(), nullptr,
                                "日志文件");
        check(settings, "settings");
        settings.hide();
        for (bool cache : {false, true}) {
            FindDialog finder([](bool) { return QVector<SearchDocument>{}; },
                              [](const SearchHit&) { return false; }, cache, "tests/fullhd");
            finder.openFind();
            check(finder, cache ? "serial-search" : "log-search");
            finder.reject();
        }
    }
    void logFontsAndWorkspacesFollowActiveConnection() {
        ThemeManager theme;
        theme.apply(true);
        SettingsRepository repository;
        MainWindow window(repository, theme);
        const auto cleanup = qScopeGuard([&] {
            window.close();
            QTest::qWait(200);
        });
        window.resize(1280, 900);
        window.show();
        window.addTestConnection();
        window.addTestConnection();
        auto* area = window.findChild<QMdiArea*>();
        auto windows = area->subWindowList();
        auto* pane = qobject_cast<TerminalPane*>(windows.last()->widget());
        auto* logs = window.findChild<LogWorkspace*>();
        auto* workspaces = logs->findChild<QTabBar*>("logWorkspaceTabs");
        QCOMPARE(workspaces->tabText(workspaces->currentIndex()), pane->session()->config().name);
        QCOMPARE(pane->terminal()->font().pointSize(), 12);
        area->setActiveSubWindow(windows.first());
        auto* first = qobject_cast<TerminalPane*>(windows.first()->widget());
        QCOMPARE(workspaces->tabText(workspaces->currentIndex()), first->session()->config().name);
        const auto history = logs->createWorkspace("history");
        auto* tabs = window.findChild<ConnectionTabs*>("connectionTabs");
        QTest::mouseClick(tabs, Qt::LeftButton, Qt::NoModifier, tabs->tabRect(0).center());
        QCOMPARE(workspaces->tabText(workspaces->currentIndex()), first->session()->config().name);
        const auto count = workspaces->count();
        window.addTestConnection();
        QCOMPARE(workspaces->count(), count + 1);
        pane = qobject_cast<TerminalPane*>(area->activeSubWindow()->widget());
        QCOMPARE(workspaces->tabText(workspaces->currentIndex()), pane->session()->config().name);
        QTemporaryDir directory;
        QFile file(directory.filePath("fonts.log"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("line 1\nline 2\n");
        file.close();
        logs->openConnectionLog(pane->session()->config().id, pane->session()->config().name,
                                file.fileName());
        auto* fileTabs = logs->activeView()->findChild<QTabWidget*>("logDocumentTabs");
        QTRY_COMPARE(fileTabs->count(), 1);
        auto* native = qobject_cast<QsciScintilla*>(fileTabs->currentWidget());
        QCOMPARE(native->SendScintilla(QsciScintillaBase::SCI_STYLEGETSIZE, QsciScintillaBase::STYLE_DEFAULT),
                 12L);
        const auto source = logs->activeView()->searchSources(false).first();
        auto* results = logs->activeView()->findChild<QTreeWidget*>();
        auto* group = new QTreeWidgetItem(results, {"line"});
        auto* result = new QTreeWidgetItem(group, {"line 1"});
        SearchHit hit;
        hit.documentId = source.id;
        hit.path = source.path;
        hit.revision = source.revision;
        hit.position = 0;
        hit.length = 4;
        result->setData(0, Qt::UserRole, QVariant::fromValue(hit));
        auto* stale = new QTreeWidgetItem(group, {"stale"});
        hit.position = 5;
        ++hit.revision;
        stale->setData(0, Qt::UserRole, QVariant::fromValue(hit));
        results->setCurrentItem(result);
        QTimer::singleShot(20, results, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            QVERIFY(menu);
            auto* action = menu->findChild<QAction*>("markAllResults2");
            QVERIFY(action && !action->icon().isNull());
            action->trigger();
            menu->close();
        });
        QMetaObject::invokeMethod(results, "customContextMenuRequested", Qt::DirectConnection,
                                  Q_ARG(QPoint, QPoint(-1, -1)));
        QVERIFY(native->SendScintilla(QsciScintillaBase::SCI_INDICATORVALUEAT, 10UL, 0L) != 0);
        QCOMPARE(native->SendScintilla(QsciScintillaBase::SCI_INDICATORVALUEAT, 10UL, 5L), 0L);
        const auto pointer = native->SendScintillaPtrResult(QsciScintillaBase::SCI_GETDOCPOINTER);
        QTest::qWait(150);
        const auto config = pane->session()->config().toJson();
        SettingsDialog settings(repository, theme, {pane}, logs, nullptr, "日志文件");
        auto* size = settings.findChild<QSpinBox*>("logReadingSize");
        auto* font = settings.findChild<QFontComboBox*>("logReadingFont");
        QVERIFY(size && font);
        QCOMPARE(size->value(), 12);
        QCOMPARE(settings.findChild<QSpinBox*>("terminalReadingSize")->value(), 12);
        QSignalSpy themes(&theme, &ThemeManager::changed);
        size->setValue(15);
        QVERIFY(settings.apply());
        QCOMPARE(themes.count(), 0);
        QCOMPARE(native->SendScintilla(QsciScintillaBase::SCI_STYLEGETSIZE, QsciScintillaBase::STYLE_DEFAULT),
                 15L);
        QCOMPARE(pane->terminal()->font().pointSize(), 12);
        QCOMPARE(pane->session()->config().toJson(), config);
        QCOMPARE(native->SendScintillaPtrResult(QsciScintillaBase::SCI_GETDOCPOINTER), pointer);
        QCOMPARE(QSettings().value("logs/appearance/dark/size").toInt(), 15);
        const auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        if (!destination.isEmpty()) {
            settings.resize(1100, 850);
            settings.show();
            QTest::qWait(30);
            settings.grab().save(destination + "/log-font-settings.png");
            settings.hide();
        }
        size->setValue(21);
        settings.reject();
        SettingsDialog reopened(repository, theme, {pane}, logs);
        QCOMPARE(reopened.findChild<QSpinBox*>("logReadingSize")->value(), 15);
        logs->show();
        QCoreApplication::processEvents();
        auto* menu = logs->findChild<QToolButton*>("workspaceMenuButton");
        QVERIFY(menu->width() >= 36 && menu->height() >= 32);
        QVERIFY(menu->parentWidget()->rect().contains(menu->geometry()));
        if (!destination.isEmpty())
            logs->grab().save(destination + "/log-workspace-menu-button.png");
    }
    void markAllSelectedHasThemeSwatchesAndContrast() {
        ThemeManager theme;
        QWidget host;
        host.resize(1000, 650);
        auto editor = makeEditor(&host);
        auto* native = qobject_cast<QsciScintilla*>(editor->widget());
        native->setGeometry(host.rect());
        host.show();
        const auto triggerMark = [&](int color) {
            QTimer::singleShot(20, native, [&, color] {
                auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                QVERIFY(menu);
                auto* action = menu->findChild<QAction*>("markAllStyle" + QString::number(color));
                QVERIFY(action && !action->icon().isNull());
                const auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
                if (!destination.isEmpty())
                    menu->grab().save(destination + "/mark-context-menu.png");
                action->trigger();
                menu->close();
            });
            QMetaObject::invokeMethod(native, "customContextMenuRequested", Qt::DirectConnection,
                                      Q_ARG(QPoint, QPoint(60, 45)));
        };
        theme.apply(true);
        editor->setTheme(true);
        editor->setAppearance(QFont("Consolas", 12), QColor("#282c34"), QColor("#abb2bf"));
        editor->setText(QString("中标") + QString(300000, 'x') + QString("中标"));
        editor->selectRange(0, 2);
        triggerMark(1);
        QTRY_VERIFY(!native->property("markingAll").toBool());
        QCOMPARE(editor->marks().size(), 2);
        QCOMPARE(editor->marks().last().position, qsizetype(300002));
        QVERIFY(native->SendScintilla(QsciScintillaBase::SCI_INDICATORVALUEAT, 9UL, 300006L) != 0);
        QVERIFY(native->SendScintilla(QsciScintillaBase::SCI_INDICATORVALUEAT, 25UL, 300006L) != 0);
        const auto pointer = native->SendScintillaPtrResult(QsciScintillaBase::SCI_GETDOCPOINTER);
        editor->setText("mark plain mark\nmark");
        editor->selectRange(0, 4);
        triggerMark(0);
        QTRY_VERIFY(!native->property("markingAll").toBool());
        QCOMPARE(editor->marks().size(), 3);
        auto luminance = [](QColor color) {
            auto linear = [](double c) {
                return c <= .04045 ? c / 12.92 : std::pow((c + .055) / 1.055, 2.4);
            };
            return .2126 * linear(color.redF()) + .7152 * linear(color.greenF()) +
                   .0722 * linear(color.blueF());
        };
        for (const auto& id : {"dark", "light", "chromatic"}) {
            theme.applyTheme(id);
            editor->setTheme(theme.dark());
            const auto background = appearanceColor("surface", theme.dark(), Qt::white);
            editor->setAppearance(QFont("Consolas", 12), background,
                                  appearanceColor("text", theme.dark(), Qt::black));
            QCOMPARE(native->SendScintillaPtrResult(QsciScintillaBase::SCI_GETDOCPOINTER), pointer);
            QCOMPARE(editor->marks().size(), 3);
            for (int i = 0; i < 8; ++i) {
                const auto style = markAppearance(i, theme.dark(), background);
                const auto a = luminance(style.background), b = luminance(style.foreground);
                QVERIFY((qMax(a, b) + .05) / (qMin(a, b) + .05) >= 4.5);
                QVERIFY(!markSwatch(i, theme.dark(), background).isNull());
            }
            QCOMPARE(native->SendScintilla(QsciScintillaBase::SCI_INDICGETALPHA, 8UL), 255L);
            QCOMPARE(native->SendScintilla(QsciScintillaBase::SCI_INDICGETSTYLE, 24UL),
                     long(QsciScintilla::TextColorIndicator));
            const auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
            if (!destination.isEmpty())
                host.grab().save(destination + "/marks-" + id + ".png");
        }
        const auto custom = markAppearance(0, true, Qt::white);
        QVERIFY(custom.background.lightnessF() > .5);
        editor->setText(QString("mark ").repeated(60000));
        editor->selectRange(0, 4);
        triggerMark(0);
        editor->clearMarks();
        QTest::qWait(40);
        QVERIFY(editor->marks().isEmpty());
    }
    void historyPanelAndSenderStretchOnlyEditor_data() {
        QTest::addColumn<QString>("themeId");
        for (const auto& id : {"dark", "light", "chromatic"})
            QTest::newRow(id) << QString(id);
    }
    void historyPanelAndSenderStretchOnlyEditor() {
        QFETCH(QString, themeId);
        ThemeManager theme;
        theme.applyTheme(themeId);
        SettingsRepository repository;
        MainWindow window(repository, theme);
        const auto cleanup = qScopeGuard([&] {
            window.close();
            QTest::qWait(200);
        });
        window.resize(1280, 900);
        window.show();
        window.addTestConnection();
        auto* area = window.findChild<QMdiArea*>();
        auto* pane = qobject_cast<TerminalPane*>(area->activeSubWindow()->widget());
        QTRY_VERIFY(pane->session()->connected());
        auto* controls = pane->controlsWidget();
        auto* bar = controls->findChild<QWidget*>("shortcutBar");
        auto* draft = controls->findChild<QPlainTextEdit*>("sendEdit");
        auto* splitter = window.findChild<QSplitter*>();
        splitter->setSizes({600, 170});
        QCoreApplication::processEvents();
        const auto barHeight = bar->height(), editHeight = draft->height();
        splitter->setSizes({300, 470});
        QCoreApplication::processEvents();
        QCOMPARE(bar->height(), barHeight);
        QVERIFY(draft->height() > editHeight + 100);
        draft->setPlainText("AT+STATUS");
        controls->findChild<QPushButton*>("sendNow")->click();
        auto* history = controls->findChild<QToolButton*>("sendHistory");
        history->menu()->popup(history->mapToGlobal(QPoint(0, history->height())));
        QCoreApplication::processEvents();
        auto* list = history->menu()->findChild<QListWidget*>("sendHistoryList");
        QVERIFY(list && list->count() > 0);
        QCOMPARE(list->item(0)->data(Qt::UserRole).toString(), QString("AT+STATUS"));
        const auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        if (!destination.isEmpty()) {
            history->menu()->grab().save(destination + "/send-history-" + themeId + ".png");
            window.grab().save(destination + "/sender-expanded-" + themeId + ".png");
        }
        draft->setPlainText("other");
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                          list->visualItemRect(list->item(0)).center());
        QCOMPARE(draft->toPlainText(), QString("AT+STATUS"));
        QVERIFY(!history->menu()->isVisible());
    }
    void emptyLineSendShowsInlineHintAndSkipsSend() {
        ThemeManager theme;
        theme.applyTheme("dark");
        SettingsRepository repository;
        MainWindow window(repository, theme);
        const auto cleanup = qScopeGuard([&] {
            window.close();
            QTest::qWait(200);
        });
        window.resize(1180, 760);
        window.show();
        window.addTestConnection();
        auto* area = window.findChild<QMdiArea*>();
        auto* pane = qobject_cast<TerminalPane*>(area->activeSubWindow()->widget());
        QTRY_VERIFY(pane->session()->connected());
        auto* controls = pane->controlsWidget();
        auto* draft = controls->findChild<QPlainTextEdit*>("sendEdit");
        auto* hint = controls->findChild<QLabel*>("sendHint");
        auto* sendNow = controls->findChild<QPushButton*>("sendNow");
        auto* history = controls->findChild<QToolButton*>("sendHistory");
        QCheckBox* currentLine = nullptr;
        for (auto* box : controls->findChildren<QCheckBox*>())
            if (box->text() == QString::fromUtf8("发送当前行"))
                currentLine = box;
        QVERIFY(draft && hint && sendNow && history && currentLine);
        QVERIFY(!hint->isVisible());
        // The hint lives inside the send row, never in the status bar or a dialog.
        QCOMPARE(hint->parentWidget(), controls->findChild<QWidget*>("terminalSend"));
        currentLine->setChecked(true);
        const auto sendCount = [&] {
            history->menu()->popup(history->mapToGlobal(QPoint(0, history->height())));
            QCoreApplication::processEvents();
            auto* list = history->menu()->findChild<QListWidget*>("sendHistoryList");
            const int entries = list ? list->count() : -1;
            history->menu()->close();
            QCoreApplication::processEvents();
            return entries;
        };
        QCOMPARE(sendCount(), 1); // placeholder item only, no transmission yet
        draft->setPlainText(QString());
        sendNow->click();
        QVERIFY(hint->isVisible());
        QCOMPARE(hint->text(), QString::fromUtf8("当前行为空，未发送"));
        const QRect draftInRow(draft->mapTo(hint->parentWidget(), QPoint(0, 0)), draft->size());
        QVERIFY(draftInRow.contains(hint->geometry()));
        QCOMPARE(sendCount(), 1); // nothing sent, history stayed empty
        const auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        if (!destination.isEmpty()) {
            sendNow->click();
            QTest::qWait(120);
            window.grab().save(destination + "/send-empty-line-hint.png");
            hint->parentWidget()->grab().save(destination + "/send-empty-line-hint-detail.png");
        }
        // Two-second lifetime: still visible at 1 s, gone afterwards.
        sendNow->click();
        QVERIFY(hint->isVisible());
        QTest::qWait(1000);
        QVERIFY(hint->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!hint->isVisible(), 2500);
        QCOMPARE(sendCount(), 1); // still nothing transmitted while the hint expired
        // Whitespace-only multi-line draft without the current-line option.
        currentLine->setChecked(false);
        draft->setPlainText(QString::fromUtf8("   \n\t\n"));
        sendNow->click();
        QVERIFY(hint->isVisible());
        QCOMPARE(hint->text(), QString::fromUtf8("发送内容为空，未发送"));
        QCOMPARE(sendCount(), 1);
        // A blank current line between real lines is skipped the same way.
        currentLine->setChecked(true);
        draft->setPlainText(QString::fromUtf8("AT\n   \nOK"));
        auto cursor = draft->textCursor();
        cursor.movePosition(QTextCursor::Start);
        cursor.movePosition(QTextCursor::NextBlock);
        draft->setTextCursor(cursor);
        sendNow->click();
        QVERIFY(hint->isVisible());
        QCOMPARE(sendCount(), 1);
        // Regular sends still work and do not raise the hint.
        QTRY_VERIFY_WITH_TIMEOUT(!hint->isVisible(), 2500); // previous hint expired
        draft->setPlainText(QStringLiteral("AT+X"));
        sendNow->click();
        QVERIFY(!hint->isVisible());
        history->menu()->popup(history->mapToGlobal(QPoint(0, history->height())));
        QCoreApplication::processEvents();
        auto* list = history->menu()->findChild<QListWidget*>("sendHistoryList");
        QVERIFY(list && list->count() > 0);
        QCOMPARE(list->item(0)->data(Qt::UserRole).toString(), QStringLiteral("AT+X"));
        history->menu()->close();
        window.close();
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
    }
    void splitGripResizesAdjacentPanes() {
        ThemeManager theme;
        theme.applyTheme("dark");
        SettingsRepository repository;
        MainWindow window(repository, theme);
        const auto cleanup = qScopeGuard([&] {
            window.close();
            QTest::qWait(200);
        });
        window.resize(1280, 820);
        window.show();
        window.addTestConnection();
        window.addTestConnection();
        auto* area = window.findChild<QMdiArea*>();
        auto* snap = window.findChild<SnapLayout*>();
        QVERIFY(area && snap);
        auto* viewport = area->viewport();
        auto* grip = viewport->findChild<QWidget*>("splitGrip");
        QVERIFY(grip && !grip->isVisible());
        const auto sendMouse = [&](QEvent::Type type, const QPoint& point, Qt::MouseButton button,
                                   Qt::MouseButtons buttons) {
            QMouseEvent event(type, point, viewport->mapToGlobal(point), button, buttons, Qt::NoModifier);
            QCoreApplication::sendEvent(viewport, &event);
        };
        const auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        QVERIFY(snap->applyPreset("columns"));
        QCoreApplication::processEvents();
        auto windows = area->subWindowList(QMdiArea::StackingOrder);
        QCOMPARE(windows.size(), 2);
        auto* left = windows[0]->pos().x() < windows[1]->pos().x() ? windows[0] : windows[1];
        auto* right = left == windows[0] ? windows[1] : windows[0];
        const int leftWidth = left->width(), rightWidth = right->width(), rightX = right->pos().x();
        const QPoint shared((left->geometry().right() + right->geometry().left()) / 2,
                            (qMax(left->pos().y(), right->pos().y()) +
                             qMin(left->pos().y() + left->height(), right->pos().y() + right->height())) /
                                2);
        sendMouse(QEvent::MouseMove, shared, Qt::NoButton, Qt::NoButton);
        QTest::qWait(350);
        QVERIFY(!grip->isVisible()); // dwell is one second, not immediate
        QTRY_VERIFY_WITH_TIMEOUT(grip->isVisible(), 1600);
        QVERIFY(grip->geometry().contains(shared));
        if (!destination.isEmpty()) {
            QTest::qWait(80);
            window.grab().save(destination + "/split-grip-columns.png");
            const QPoint offset = viewport->mapTo(&window, QPoint(0, 0));
            window.grab()
                .copy(grip->geometry().translated(offset).adjusted(-36, -36, 36, 36))
                .scaled(360, 360, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                .save(destination + "/split-grip-columns-detail.png");
        }
        const QPoint origin = grip->geometry().center(), moved = origin + QPoint(120, 0);
        sendMouse(QEvent::MouseButtonPress, origin, Qt::LeftButton, Qt::LeftButton);
        sendMouse(QEvent::MouseMove, moved, Qt::NoButton, Qt::LeftButton);
        sendMouse(QEvent::MouseButtonRelease, moved, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(left->width(), leftWidth + 120);
        QCOMPARE(right->width(), rightWidth - 120);
        QCOMPARE(right->pos().x(), rightX + 120);
        const QPoint away(viewport->width() - 24, viewport->height() - 24);
        sendMouse(QEvent::MouseMove, away, Qt::NoButton, Qt::NoButton);
        QTRY_VERIFY(!grip->isVisible());
        // Stacked panes: the same handle works on the horizontal edge.
        QVERIFY(snap->applyPreset("rows"));
        QCoreApplication::processEvents();
        windows = area->subWindowList(QMdiArea::StackingOrder);
        auto* top = windows[0]->pos().y() < windows[1]->pos().y() ? windows[0] : windows[1];
        auto* bottom = top == windows[0] ? windows[1] : windows[0];
        const int topHeight = top->height(), bottomHeight = bottom->height();
        const QPoint sharedRow((qMax(top->pos().x(), bottom->pos().x()) +
                                qMin(top->pos().x() + top->width(), bottom->pos().x() + bottom->width())) /
                                   2,
                               (top->geometry().bottom() + bottom->geometry().top()) / 2);
        sendMouse(QEvent::MouseMove, sharedRow, Qt::NoButton, Qt::NoButton);
        QTRY_VERIFY_WITH_TIMEOUT(grip->isVisible(), 1600);
        QVERIFY(grip->geometry().contains(sharedRow));
        if (!destination.isEmpty()) {
            QTest::qWait(80);
            window.grab().save(destination + "/split-grip-rows.png");
            const QPoint offset = viewport->mapTo(&window, QPoint(0, 0));
            window.grab()
                .copy(grip->geometry().translated(offset).adjusted(-36, -36, 36, 36))
                .scaled(360, 360, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                .save(destination + "/split-grip-rows-detail.png");
        }
        const QPoint rowOrigin = grip->geometry().center(), rowMoved = rowOrigin + QPoint(0, 90);
        sendMouse(QEvent::MouseButtonPress, rowOrigin, Qt::LeftButton, Qt::LeftButton);
        sendMouse(QEvent::MouseMove, rowMoved, Qt::NoButton, Qt::LeftButton);
        sendMouse(QEvent::MouseButtonRelease, rowMoved, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(top->height(), topHeight + 90);
        QCOMPARE(bottom->height(), bottomHeight - 90);
        sendMouse(QEvent::MouseMove, away, Qt::NoButton, Qt::NoButton);
        QTRY_VERIFY(!grip->isVisible());
        window.close();
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
    }
    void splitGripPushesGroupedPanes() {
        ThemeManager theme;
        theme.applyTheme("dark");
        SettingsRepository repository;
        MainWindow window(repository, theme);
        window.resize(1280, 820);
        window.show();
        window.addTestConnection();
        window.addTestConnection();
        window.addTestConnection();
        auto* area = window.findChild<QMdiArea*>();
        QVERIFY(area);
        auto* viewport = area->viewport();
        auto* grip = viewport->findChild<QWidget*>("splitGrip");
        QVERIFY(grip && !grip->isVisible());
        const auto sendMouse = [&](QEvent::Type type, const QPoint& point, Qt::MouseButton button,
                                   Qt::MouseButtons buttons) {
            QMouseEvent event(type, point, viewport->mapToGlobal(point), button, buttons, Qt::NoModifier);
            QCoreApplication::sendEvent(viewport, &event);
        };
        const QRect bounds = viewport->rect();
        constexpr int gap = 4;
        // One pane on the left, two stacked panes on the right: a 1:2 split.
        auto windows = area->subWindowList(QMdiArea::StackingOrder);
        QCOMPARE(windows.size(), 3);
        for (auto* pane : windows)
            pane->showNormal();
        QCoreApplication::processEvents();
        const int leftWidth = bounds.width() * 45 / 100;
        windows[0]->setGeometry(
            QRect(bounds.left() + gap, bounds.top() + gap, leftWidth - gap * 2, bounds.height() - gap * 2));
        const int rightX = bounds.left() + leftWidth + gap;
        const int rightWidth = bounds.width() - leftWidth - gap * 2;
        const int middleY = bounds.top() + bounds.height() / 2;
        windows[1]->setGeometry(QRect(rightX, bounds.top() + gap, rightWidth, bounds.height() / 2 - gap * 2));
        windows[2]->setGeometry(QRect(rightX, middleY + gap, rightWidth, bounds.height() / 2 - gap * 3));
        QCoreApplication::processEvents();
        auto* left = windows[0];
        auto* rightTop = windows[1]->pos().y() <= windows[2]->pos().y() ? windows[1] : windows[2];
        auto* rightBottom = rightTop == windows[1] ? windows[2] : windows[1];
        const int leftWidth0 = left->width(), rightWidth0 = rightTop->width();
        const int rightTopHeight0 = rightTop->height();
        const int rightTopX0 = rightTop->pos().x(), rightBottomX0 = rightBottom->pos().x();
        const int shared = (left->geometry().right() + rightTop->geometry().left()) / 2;
        const int leftCenterY = left->geometry().center().y();
        const QPoint middle(shared, leftCenterY);
        sendMouse(QEvent::MouseMove, middle, Qt::NoButton, Qt::NoButton);
        QTest::qWait(350);
        QVERIFY(!grip->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(grip->isVisible(), 1600);
        QVERIFY(grip->geometry().contains(middle));
        // The handle belongs to the whole shared edge, so it stays centred on the
        // tall pane instead of jumping to the junction of the two short panes.
        QVERIFY(qAbs(grip->geometry().center().y() - leftCenterY) <= 40);
        const auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        if (!destination.isEmpty()) {
            QTest::qWait(80);
            window.grab().save(destination + "/split-grip-1x2.png");
            const QPoint offset = viewport->mapTo(&window, QPoint(0, 0));
            window.grab()
                .copy(grip->geometry().translated(offset).adjusted(-40, -40, 40, 40))
                .scaled(360, 360, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                .save(destination + "/split-grip-1x2-detail.png");
        }
        const QPoint junction(shared, (rightTop->geometry().bottom() + rightBottom->geometry().top()) / 2);
        sendMouse(QEvent::MouseMove, junction, Qt::NoButton, Qt::NoButton);
        QTest::qWait(150);
        QVERIFY(grip->isVisible());
        QVERIFY(qAbs(grip->geometry().center().y() - leftCenterY) <= 40);
        const QPoint origin = grip->geometry().center(), moved = origin + QPoint(100, 0);
        sendMouse(QEvent::MouseButtonPress, origin, Qt::LeftButton, Qt::LeftButton);
        sendMouse(QEvent::MouseMove, moved, Qt::NoButton, Qt::LeftButton);
        sendMouse(QEvent::MouseButtonRelease, moved, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(left->width(), leftWidth0 + 100);
        QCOMPARE(rightTop->width(), rightWidth0 - 100);
        QCOMPARE(rightBottom->width(), rightWidth0 - 100);
        QCOMPARE(rightTop->pos().x(), rightTopX0 + 100);
        QCOMPARE(rightBottom->pos().x(), rightBottomX0 + 100);
        QCOMPARE(rightTop->height(), rightTopHeight0); // stacking survives the push
        const QPoint away(bounds.width() - 24, bounds.height() - 24);
        sendMouse(QEvent::MouseMove, away, Qt::NoButton, Qt::NoButton);
        QTRY_VERIFY(!grip->isVisible());
        // Four quadrants: rows stay independently adjustable, no over-merging.
        window.addTestConnection();
        windows = area->subWindowList(QMdiArea::StackingOrder);
        QCOMPARE(windows.size(), 4);
        for (auto* pane : windows)
            pane->showNormal();
        QCoreApplication::processEvents();
        const int columnWidth = (bounds.width() - gap * 3) / 2;
        const int rowHeight = (bounds.height() - gap * 3) / 2;
        for (int index = 0; index < windows.size(); ++index) {
            const int column = index % 2, row = index / 2;
            windows[index]->setGeometry(QRect(bounds.left() + gap + column * (columnWidth + gap),
                                              bounds.top() + gap + row * (rowHeight + gap), columnWidth,
                                              rowHeight));
        }
        QCoreApplication::processEvents();
        auto* topLeft = windows[0];
        auto* topRight = windows[1];
        auto* bottomLeft = windows[2];
        auto* bottomRight = windows[3];
        const int topLeftWidth0 = topLeft->width(), bottomLeftWidth0 = bottomLeft->width();
        const int topRightX0 = topRight->pos().x(), bottomRightX0 = bottomRight->pos().x();
        const QPoint upperEdge((topLeft->geometry().right() + topRight->geometry().left()) / 2,
                               topLeft->geometry().center().y());
        sendMouse(QEvent::MouseMove, upperEdge, Qt::NoButton, Qt::NoButton);
        QTRY_VERIFY_WITH_TIMEOUT(grip->isVisible(), 1600);
        QVERIFY(grip->geometry().contains(upperEdge));
        if (!destination.isEmpty()) {
            QTest::qWait(80);
            window.grab().save(destination + "/split-grip-2x2.png");
        }
        const QPoint quadOrigin = grip->geometry().center(), quadMoved = quadOrigin + QPoint(80, 0);
        sendMouse(QEvent::MouseButtonPress, quadOrigin, Qt::LeftButton, Qt::LeftButton);
        sendMouse(QEvent::MouseMove, quadMoved, Qt::NoButton, Qt::LeftButton);
        sendMouse(QEvent::MouseButtonRelease, quadMoved, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(topLeft->width(), topLeftWidth0 + 80);
        QCOMPARE(topRight->pos().x(), topRightX0 + 80);
        QCOMPARE(bottomLeft->width(), bottomLeftWidth0);
        QCOMPARE(bottomRight->pos().x(), bottomRightX0);
        sendMouse(QEvent::MouseMove, away, Qt::NoButton, Qt::NoButton);
        QTRY_VERIFY(!grip->isVisible());
        window.close();
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
    }
    void themeControlContrast() {
        ThemeManager theme;
        const auto luminance = [](const QColor& color) {
            const auto linear = [](double v) {
                return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4);
            };
            return .2126 * linear(color.redF()) + .7152 * linear(color.greenF()) +
                   .0722 * linear(color.blueF());
        };
        for (const auto& id : {"dark", "light", "chromatic"}) {
            theme.applyTheme(id);
            const auto colors = qApp->property("portwave.theme.colors").toMap();
            for (const auto& pair :
                 QList<QPair<QString, QString>>{{"text", "surface"},
                                                {"text", "shell"},
                                                {"button.foreground", "button.background"},
                                                {"button.primaryForeground", "button.primaryBackground"},
                                                {"text", "selection.background"}}) {
                const auto a = luminance(colors.value(pair.first).value<QColor>());
                const auto b = luminance(colors.value(pair.second).value<QColor>());
                const auto ratio = (qMax(a, b) + .05) / (qMin(a, b) + .05);
                qInfo() << id << pair << ratio;
                QVERIFY2(ratio >= 4.5, qPrintable(QString(id) + ": " + pair.first + "/" + pair.second));
            }
            QVERIFY(!qApp->styleSheet().contains("@"));
        }
    }
    void appAndLogWindowsShareNativeIcon() {
        ThemeManager theme;
        theme.applyTheme("chromatic");
        const auto icon = IconManager::appIcon();
        const auto previous = QApplication::windowIcon();
        QApplication::setWindowIcon(icon);
        const auto cleanup = qScopeGuard([&] { QApplication::setWindowIcon(previous); });
        SettingsRepository repository;
        MainWindow main(repository, theme);
        LogWorkspace logs;
        main.show();
        logs.show();
        const auto id = logs.createWorkspace("图标检查");
        logs.detachWorkspace(id);
        auto* floating = logs.activeView()->window();
        const auto preview = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        for (auto* window : {static_cast<QWidget*>(&main), static_cast<QWidget*>(&logs), floating}) {
            QCOMPARE(window->windowIcon().pixmap(32).toImage(), icon.pixmap(32).toImage());
#ifdef Q_OS_WIN
            if (QGuiApplication::platformName() == "windows") {
                const auto hwnd = reinterpret_cast<HWND>(window->winId());
                QVERIFY(SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0) != 0);
                QVERIFY(SendMessageW(hwnd, WM_GETICON, ICON_BIG, 0) != 0);
                QVERIFY((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) == 0);
            }
#endif
        }
        if (!preview.isEmpty()) {
            QDir().mkpath(preview);
            QTest::qWait(100);
            main.grab().save(preview + "/main-icon.png");
            logs.grab().save(preview + "/log-icon.png");
            floating->grab().save(preview + "/floating-log-icon.png");
        }
        logs.restoreSingleWindow();
        logs.close();
        main.close();
    }
    void appIconCoversShellRenderingSizes() {
        // The taskbar, Alt-Tab and Explorer render 16-32 px variants, so the icon must
        // carry real pixels at every shell size instead of a single scalable glyph.
        const QIcon icon = IconManager::appIcon();
        QVERIFY(!icon.isNull());
        for (const int size : {16, 24, 32, 48, 64, 128, 256}) {
            const QPixmap pixmap = icon.pixmap(QSize(size, size), 1.0);
            QVERIFY2(!pixmap.isNull(), qPrintable(QStringLiteral("missing size %1").arg(size)));
            QCOMPARE(pixmap.size(), QSize(size, size));
            const QImage image = pixmap.toImage();
            int opaque = 0;
            for (int y = 0; y < image.height(); y += 2) {
                for (int x = 0; x < image.width(); x += 2) {
                    if (qAlpha(image.pixel(x, y)) > 200) {
                        ++opaque;
                    }
                }
            }
            QVERIFY2(opaque > 0, qPrintable(QStringLiteral("transparent icon at %1").arg(size)));
        }
    }
    void numericButtonsVisibleAndClickableInAllThemes() {
        ThemeManager theme;
        for (const auto& id : {"dark", "light", "chromatic"}) {
            theme.applyTheme(id);
            QWidget host;
            auto* layout = new QVBoxLayout(&host);
            QSpinBox integer;
            integer.setRange(0, 20);
            integer.setValue(5);
            integer.setFixedSize(150, 40);
            QDoubleSpinBox decimal;
            decimal.setRange(0, 20);
            decimal.setValue(5);
            decimal.setFixedSize(150, 40);
            QDateTimeEdit date;
            date.setFixedSize(240, 40);
            layout->addWidget(&integer);
            layout->addWidget(&decimal);
            layout->addWidget(&date);
            host.show();
            QCoreApplication::processEvents();
            for (auto* spin : QList<QAbstractSpinBox*>{&integer, &decimal, &date}) {
                QStyleOptionSpinBox option;
                option.initFrom(spin);
                option.frame = true;
                option.buttonSymbols = QAbstractSpinBox::UpDownArrows;
                option.stepEnabled = QAbstractSpinBox::StepUpEnabled | QAbstractSpinBox::StepDownEnabled;
                auto up =
                    spin->style()->subControlRect(QStyle::CC_SpinBox, &option, QStyle::SC_SpinBoxUp, spin);
                auto down =
                    spin->style()->subControlRect(QStyle::CC_SpinBox, &option, QStyle::SC_SpinBoxDown, spin);
                QVERIFY(up.width() >= 24 && up.height() >= 14 && down.width() >= 24 && down.height() >= 14);
                if (spin == &integer) {
                    QTest::mouseClick(spin, Qt::LeftButton, Qt::NoModifier, up.center());
                    QCOMPARE(integer.value(), 6);
                    QTest::mouseClick(spin, Qt::LeftButton, Qt::NoModifier, down.center());
                    QCOMPARE(integer.value(), 5);
                }
            }
            const auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
            if (!destination.isEmpty())
                host.grab().save(destination + "/numeric-inputs-" + id + ".png");
        }
    }
    void smallSettingsApplyPerformance() {
        ThemeManager theme;
        theme.apply(true);
        SettingsRepository repository;
        ConnectionConfig config;
        config.id = "apply-performance";
        config.kind = "test";
        config.pauseReceive = true;
        config.bufferLines = 30000;
        Session session(config);
        TerminalPane pane(&session);
        const auto cleanup = qScopeGuard([&] {
            pane.shutdown();
            session.stop();
        });
        pane.resize(1000, 700);
        pane.show();
        QByteArray sample;
        for (int i = 0; i < 20000; ++i)
            sample += "line \x1b[33mWARN\x1b[0m sample\r\n";
        pane.terminal()->consume(std::string_view(sample.constData(), sample.size()));
        SettingsDialog dialog(repository, theme, {&pane}, nullptr);
        auto* row = dialog.findChild<QWidget*>("settingRow_sendCR");
        QVERIFY(row);
        auto* option = row->findChild<QCheckBox*>();
        option->setChecked(!option->isChecked());
        QSignalSpy themes(&theme, &ThemeManager::changed);
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(dialog.apply());
        qInfo("small-apply ms=%lld theme-updates=%lld", elapsed.elapsed(), qint64(themes.count()));
        QCOMPARE(themes.count(), 0);
        QVERIFY(dialog.apply());
        QCOMPARE(themes.count(), 0);
    }
    void tabStructureAndPlainSlide() {
        ThemeManager theme;
        theme.applyTheme("chromatic");
        ConnectionTabs tabs;
        tabs.setObjectName("connectionTabs");
        tabs.setExpanding(false);
        tabs.resize(700, 48);
        tabs.addTab("t2");
        tabs.addTab("t3");
        tabs.show();
        QCoreApplication::processEvents();
        tabs.setCurrentIndex(1);
        auto* animation = tabs.findChild<QVariantAnimation*>();
        auto* indicator = tabs.findChild<QWidget*>("connectionTabIndicator");
        QVERIFY(animation && indicator);
        animation->setCurrentTime(70);
        {
            QSignalBlocker blocker(&tabs);
            tabs.setTabVisible(1, false);
            tabs.synchronize();
        }
        QCoreApplication::processEvents();
        QCOMPARE(indicator->isVisible(), tabs.currentIndex() >= 0 && tabs.isTabVisible(tabs.currentIndex()));
        if (indicator->isVisible())
            QCOMPARE(indicator->geometry(), tabs.tabRect(tabs.currentIndex()).adjusted(2, 2, -2, -2));
        tabs.addTab("t4");
        tabs.setCurrentIndex(2);
        QCoreApplication::processEvents();
        QCOMPARE(indicator->geometry(), tabs.tabRect(2).adjusted(2, 2, -2, -2));
        auto* add = tabs.findChild<QToolButton*>("addConnectionTab");
        QVERIFY(add && add->width() >= 32 && add->height() >= 32);
        const auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        if (!destination.isEmpty())
            tabs.grab().save(destination + "/tabs-after-detach-new.png");
        tabs.setCurrentIndex(0);
        const auto width = indicator->width();
        animation->setCurrentTime(75);
        QCOMPARE(animation->duration(), 150);
        QVERIFY(qAbs(indicator->width() - width) <= 1);
        {
            QSignalBlocker blocker(&tabs);
            tabs.setTabVisible(0, false);
            tabs.setTabVisible(2, false);
            tabs.synchronize();
        }
        QVERIFY(!indicator->isVisible());
    }
    // Empty workspace: no tab sizes the row, and the fixed-size "+" used to be pinned to the
    // top edge and clipped against the toolbar. The empty row must match the row height that
    // appears with the first tab, so the button keeps its clearance and does not jump.
    void emptyTabRowKeepsAddButtonClearOfToolbar() {
        ThemeManager theme;
        theme.applyTheme("chromatic");
        QWidget host;
        auto* body = new QVBoxLayout(&host);
        body->setContentsMargins(0, 0, 0, 0);
        body->setSpacing(0);
        auto* tabs = new ConnectionTabs;
        tabs->setObjectName("connectionTabs");
        tabs->setExpanding(false);
        tabs->setMinimumHeight(theme.profile().controls.tabHeight);
        body->addWidget(tabs);
        body->addStretch(1);
        host.resize(700, 200);
        host.show();
        QCoreApplication::processEvents();
        auto* add = tabs->findChild<QToolButton*>("addConnectionTab");
        QVERIFY(add && add->isVisible());
        const int emptyRow = tabs->height();
        const QRect emptyButton = add->geometry();
        QVERIFY2(emptyButton.bottom() < tabs->height(), "add button clipped by the empty tab row");
        QVERIFY2(emptyButton.top() >= 2, "add button sits against the toolbar edge");
        QVERIFY(tabs->minimumSizeHint().height() >= add->height());
        const auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        if (!destination.isEmpty())
            host.grab().save(destination + "/empty-tab-row.png");
        tabs->addTab("t2");
        QCoreApplication::processEvents();
        QVERIFY2(qAbs(tabs->height() - emptyRow) <= 2, "tab row changes height when the first tab opens");
        QCOMPARE(add->height(), emptyButton.height());
        QVERIFY(qAbs(add->y() - emptyButton.top()) <= 2);
        QVERIFY(add->geometry().bottom() < tabs->height());
    }
    void floatingActiveHasNoGhostTabAndRemainingTabActivates() {
        ThemeManager theme;
        theme.applyTheme("chromatic");
        SettingsRepository repository;
        MainWindow window(repository, theme);
        const auto cleanup = qScopeGuard([&] {
            window.close();
            QTest::qWait(200);
        });
        window.show();
        window.addTestConnection();
        window.addTestConnection();
        auto* area = window.findChild<QMdiArea*>();
        auto windows = area->subWindowList();
        auto* snap = window.findChild<SnapLayout*>();
        QVERIFY(snap && windows.size() == 2);
        QVERIFY(snap->applyPreset("columns"));
        auto* first = qobject_cast<TerminalPane*>(windows[0]->widget());
        first->findChild<QToolButton*>("dockTerminal")->click();
        area->setActiveSubWindow(windows[1]);
        QCoreApplication::processEvents();
        auto* tabs = window.findChild<ConnectionTabs*>("connectionTabs");
        auto* indicator = tabs->findChild<QWidget*>("connectionTabIndicator");
        QVERIFY(tabs->isVisible());
        QVERIFY(tabs->isTabVisible(0) && !tabs->isTabVisible(1));
        QVERIFY(!indicator->isVisible());
        QCOMPARE(tabs->currentIndex(), 0);
        QTest::mouseClick(tabs, Qt::LeftButton, Qt::NoModifier, tabs->tabRect(0).center());
        QCOMPARE(area->activeSubWindow(), windows[0]);
        QVERIFY(indicator->isVisible());
        QCOMPARE(indicator->geometry(), tabs->tabRect(0).adjusted(2, 2, -2, -2));
    }
    void pauseReceiveKeepsFileAndKeyboardDoesNotSend() {
        ThemeManager theme;
        theme.apply(true);
        SettingsRepository repository;
        MainWindow window(repository, theme);
        const auto cleanup = qScopeGuard([&] {
            window.close();
            QTest::qWait(200);
        });
        window.show();
        window.addTestConnection();
        auto* area = window.findChild<QMdiArea*>();
        auto* pane = qobject_cast<TerminalPane*>(area->activeSubWindow()->widget());
        auto* session = pane->session();
        QSignalSpy paths(session, &Session::logPathChanged);
        QSignalSpy tx(pane->terminal(), &TerminalView::input);
        auto* power = window.findChild<QAction*>("toolbar_power");
        QTRY_VERIFY(session->connected());
        QTRY_VERIFY(!paths.isEmpty());
        const auto path = paths.first().first().toString();
        QTest::qWait(1100);
        power->trigger();
        QVERIFY(pane->receivePaused());
        QVERIFY(session->connected());
        QTest::qWait(1100);
        const auto stable = pane->terminal()->toPlainText();
        const auto bytes = QFileInfo(path).size();
        QTest::qWait(100);
        QCOMPARE(pane->terminal()->toPlainText(), stable);
        const auto preview = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        if (!preview.isEmpty())
            window.grab().save(preview + "/receive-paused-same-log.png");
        auto* draft = pane->controlsWidget()->findChild<QPlainTextEdit*>("sendEdit");
        QTest::keyClick(draft, Qt::Key_Space);
        QVERIFY(pane->receivePaused());
        QTest::keyClick(pane->terminal(), Qt::Key_Return);
        QVERIFY(!pane->receivePaused());
        QCOMPARE(tx.count(), 0);
        QTest::qWait(1100);
        QVERIFY(QFileInfo(path).size() > bytes);
        QCOMPARE(paths.count(), 1);
        power->trigger();
        QTest::keyClick(pane->terminal(), Qt::Key_Space);
        QVERIFY(!pane->receivePaused());
        QCOMPARE(tx.count(), 0);
        QCOMPARE(paths.count(), 1);
        QCOMPARE(power->property("connectionState").toString(), QString("connected"));
    }
    void shortcutSuffixAndToggleEditor() {
        ThemeManager theme;
        theme.applyTheme("chromatic");
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QByteArray received;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            auto* peer = server.nextPendingConnection();
            connect(peer, &QTcpSocket::readyRead, peer, [&, peer] { received += peer->readAll(); });
        });
        ConnectionConfig config;
        config.id = "suffix";
        config.kind = "tcp";
        config.tcpPort = server.serverPort();
        Session session(config);
        TerminalPane pane(&session);
        const auto cleanup = qScopeGuard([&] {
            pane.shutdown();
            session.stop();
            QTest::qWait(200);
        });
        session.start();
        pane.show();
        QTRY_VERIFY(session.connected());
        for (int flags = 0; flags < 4; ++flags) {
            QJsonObject command{{"name", "test suffix"},
                                {"text", "A\\n"},
                                {"hex", false},
                                {"appendCR", bool(flags & 1)},
                                {"appendLF", bool(flags & 2)}};
            pane.setCommands({command});
            for (auto* button : pane.controlsWidget()->findChildren<QPushButton*>("quickCommand"))
                if (button->text() == "test suffix")
                    button->click();
            QByteArray expected = "A\n";
            if (flags & 1)
                expected += '\r';
            if (flags & 2)
                expected += '\n';
            QTRY_COMPARE(received, expected);
            received.clear();
        }
        QTimer::singleShot(30, &pane, [&] {
            auto* dialog = pane.findChild<QDialog*>("quickCommandDialog");
            if (!dialog)
                return;
            auto* cr = dialog->findChild<QCheckBox*>("quickAppendCR");
            auto* lf = dialog->findChild<QCheckBox*>("quickAppendLF");
            QVERIFY(cr && lf);
            QVERIFY(cr->property("fluentToggle").toBool());
            cr->setChecked(false);
            lf->setChecked(true);
            const auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
            if (!destination.isEmpty())
                dialog->grab().save(destination + "/quick-command-toggles.png");
            dialog->accept();
        });
        pane.manageCommand(0);
        const auto definitions =
            QJsonDocument::fromJson(QSettings().value("commands/groups").toByteArray()).object();
        QVERIFY(!definitions.isEmpty());
    }
    void simulationSpeedAndDefaultDisplayLimit() {
        QCOMPARE(ConnectionConfig{}.bufferLines, 500);
        QCOMPARE(ConnectionConfig::fromJson({}).bufferLines, 500);
        ConnectionConfig saved;
        saved.id = "simulation-default";
        saved.kind = "test";
        auto json = saved.toJson();
        json["bufferLines"] = 900;
        QCOMPARE(ConnectionConfig::fromJson(json).bufferLines, 900);
        SettingsRepository repository;
        QString error;
        QVERIFY(repository.saveConnections({saved}, error));
        ConnectionDialog dialog(repository);
        dialog.show();
        auto* limit = dialog.findChild<QSpinBox*>("receiveDisplayMaxLines");
        auto* speed = dialog.findChild<QSpinBox*>("simulationIntervalMs");
        QVERIFY(limit && speed);
        QCOMPARE(limit->value(), 500);
        QCOMPARE(speed->value(), 10);
        auto* scroll = dialog.findChild<QScrollArea*>();
        if (scroll)
            scroll->ensureWidgetVisible(speed);
        const auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        if (!destination.isEmpty())
            dialog.grab().save(destination + "/new-connection-limits-speed.png");
        dialog.reject();
        saved.simulationIntervalMs = 30;
        Session session(saved);
        TerminalPane pane(&session);
        const auto cleanup = qScopeGuard([&] {
            pane.shutdown();
            session.stop();
            QTest::qWait(200);
        });
        session.start();
        QTRY_VERIFY(session.connected());
        pane.setReceivePaused(true);
        QByteArray lines;
        for (int i = 0; i < 700; ++i)
            lines += "line\n";
        pane.terminal()->consume(std::string_view(lines.constData(), lines.size()));
        QVERIFY(pane.terminal()->document()->blockCount() <= 500);
        pane.setReceivePaused(false);
        // Pause display consumption to measure generated lines in the bounded RX queue.
        pane.pauseDisplay(true);
        session.take(1048576);
        QTest::qWait(240);
        const auto slow = QByteArray::fromStdString(session.take(1048576)).count('\n');
        auto fast = session.config();
        fast.simulationIntervalMs = 2;
        session.applyConfig(fast);
        QTest::qWait(40);
        session.take(1048576);
        QTest::qWait(240);
        const auto quick = QByteArray::fromStdString(session.take(1048576)).count('\n');
        QVERIFY(slow >= 3 && slow <= 15);
        QVERIFY(quick > slow * 3);
        QCOMPARE(ConnectionConfig::fromJson(fast.toJson()).simulationIntervalMs, 2);
    }
    void popupButtonsReleaseTransientState() {
        ThemeManager theme;
        theme.applyTheme("chromatic");
        SettingsRepository repository;
        MainWindow window(repository, theme);
        window.show();
        window.addTestConnection();
        const auto cleanup = qScopeGuard([&] {
            window.close();
            QTest::qWait(200);
        });
        auto* toolbar = window.findChild<QToolBar*>();
        for (const auto& actionName : {"toolbar_settings", "toolbar_plug"}) {
            auto* action = window.findChild<QAction*>(actionName);
            auto* button = qobject_cast<QToolButton*>(toolbar->widgetForAction(action));
            QVERIFY(button);
            QTimer::singleShot(40, &window, [] {
                if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                    dialog->reject();
            });
            QTest::mouseClick(button, Qt::LeftButton);
            QCursor::setPos(window.mapToGlobal(QPoint(700, 300)));
            QEvent activation(QEvent::WindowActivate);
            QCoreApplication::sendEvent(&window, &activation);
            QCoreApplication::processEvents();
            QVERIFY(!button->isDown());
            QVERIFY(!button->underMouse());
        }
        auto* area = window.findChild<QMdiArea*>();
        auto* pane = qobject_cast<TerminalPane*>(area->activeSubWindow()->widget());
        for (const auto& name : {"sendHistory", "sendOptionsMenu"}) {
            auto* button = pane->controlsWidget()->findChild<QToolButton*>(name);
            QVERIFY(button && button->menu());
            button->menu()->popup(button->mapToGlobal(QPoint(0, button->height())));
            QCoreApplication::processEvents();
            button->menu()->close();
            QCursor::setPos(window.mapToGlobal(QPoint(700, 300)));
            QCoreApplication::processEvents();
            QVERIFY(!button->isDown());
            QVERIFY(!button->underMouse());
        }
        const auto preview = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        if (!preview.isEmpty())
            window.grab().save(preview + "/toolbar-after-dialogs-close.png");
    }
    void unifiedSenderToolbarAndPower() {
        ThemeManager theme;
        SettingsRepository repository;
        MainWindow window(repository, theme);
        const auto cleanup = qScopeGuard([&] {
            window.close();
            QTest::qWait(200);
        });
        window.resize(1280, 820);
        window.show();
        auto* power = window.findChild<QAction*>("toolbar_power");
        QVERIFY(power && !power->isEnabled());
        window.addTestConnection();
        auto* area = window.findChild<QMdiArea*>();
        auto* pane = qobject_cast<TerminalPane*>(area->activeSubWindow()->widget());
        auto* controls = pane->controlsWidget();
        QTRY_VERIFY(pane->session()->connected());
        auto* options = controls->findChild<QToolButton*>("sendOptionsMenu");
        auto* history = controls->findChild<QToolButton*>("sendHistory");
        auto* interval = controls->findChild<QDoubleSpinBox*>("sendInterval");
        auto* unit = controls->findChild<QComboBox*>("sendIntervalUnit");
        auto* target = controls->findChild<QComboBox*>("sendTargetCombo");
        QVERIFY(options && history && interval && unit && target);
        auto* popup = options->menu();
        QVERIFY(popup && popup->findChild<QWidget*>("sendOptionsPanel"));
        QCOMPARE(interval->window(), static_cast<QWidget*>(popup));
        auto* logView = window.findChild<LogWorkspace*>()->activeView();
        logView->addExample();
        for (const auto& id : {"dark", "light", "chromatic"}) {
            theme.applyTheme(id);
            QCoreApplication::processEvents();
            const auto connectedColor = appearanceColor("status.connected", theme.dark(), Qt::green);
            QCOMPARE(power->icon().pixmap(24, 24).toImage(),
                     IconManager::icon("power", theme.dark(), connectedColor).pixmap(24, 24).toImage());
            auto* connectionTabs = window.findChild<ConnectionTabs*>("connectionTabs");
            QCOMPARE(connectionTabs->tabIcon(0).pixmap(8, 8).toImage().pixelColor(3, 3), connectedColor);
            auto* senderPin = controls->findChild<QToolButton*>("senderPin");
            QVERIFY(senderPin);
            QCOMPARE(senderPin->icon().pixmap(24, 24).toImage(),
                     pinIcon(senderPin->isChecked()).pixmap(24, 24).toImage());
            for (const auto& name : {"windowPin", "logTabPin"})
                for (auto* pin : window.findChildren<QToolButton*>(name))
                    QCOMPARE(pin->icon().pixmap(24, 24).toImage(),
                             pinIcon(pin->isChecked()).pixmap(24, 24).toImage());
            QVERIFY(options->isVisible());
            QVERIFY(history->isVisible());
            auto* toolbar = window.findChild<QToolBar*>();
            QCOMPARE(toolbar->parentWidget()->objectName(), QString("shellHeader"));
            QCOMPARE(toolbar->toolButtonStyle(), Qt::ToolButtonIconOnly);
            QTRY_VERIFY_WITH_TIMEOUT(!pane->terminal()->property("recoloring").toBool(), 5000);
            for (auto block = pane->terminal()->document()->begin(); block.isValid(); block = block.next())
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const auto format = it.fragment().charFormat();
                    const auto raw = format.property(QTextFormat::UserProperty + 1);
                    if (raw.isValid()) {
                        const auto expected =
                            displayAnsiColor(QColor::fromRgb(raw.toInt()),
                                             pane->terminal()->palette().color(QPalette::Base), theme.dark());
                        QVERIFY2(
                            format.foreground().color() == expected,
                            qPrintable(QString("ANSI %1 raw=%2 actual=%3 expected=%4 bg=%5")
                                           .arg(id)
                                           .arg(raw.toInt())
                                           .arg(format.foreground().color().name(), expected.name(),
                                                pane->terminal()->palette().color(QPalette::Base).name())));
                    }
                }
            const auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
            if (!destination.isEmpty()) {
                window.grab().save(destination + "/unified-" + id + ".png");
            }
        }
        interval->setValue(1250);
        unit->setCurrentIndex(1);
        QCOMPARE(interval->value(), 1.25);
        unit->setCurrentIndex(0);
        QCOMPARE(interval->value(), 1250.0);
        popup->popup(options->mapToGlobal(QPoint(0, options->height())));
        QCoreApplication::processEvents();
        QVERIFY(interval->isVisible());
        const auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        if (!destination.isEmpty())
            popup->grab().save(destination + "/send-options.png");
        popup->close();
        auto* draft = controls->findChild<QPlainTextEdit*>("sendEdit");
        draft->setPlainText("retained draft");
        QTRY_COMPARE(pane->session()->config().sendDraft, QString("retained draft"));
        power->trigger();
        QVERIFY(pane->session()->connected());
        QVERIFY(power->isEnabled());
        QCOMPARE(power->property("connectionState").toString(), QString("paused"));
        QVERIFY(pane->receivePaused());
        power->trigger();
        QVERIFY(!pane->receivePaused());
        QCOMPARE(power->property("connectionState").toString(), QString("connected"));
        QCOMPARE(draft->toPlainText(), QString("retained draft"));
    }
    void oneDarkDefaultsPreserveOverrides() {
        ThemeManager theme;
        theme.apply(true);
        QCOMPARE(appearanceColor("surface", true, Qt::red), QColor("#282c34"));
        QCOMPARE(appearanceColor("text", true, Qt::red), QColor("#abb2bf"));
        QCOMPARE(appearanceColor("ansi.1", true, Qt::red), QColor("#e05561"));
        TerminalView terminal;
        terminal.setTheme(true);
        terminal.consume("\x1b[31mred\x1b[0m");
        QTextCursor cursor(terminal.document());
        cursor.setPosition(1);
        QCOMPARE(cursor.charFormat().foreground().color(),
                 readableAnsiColor(QColor("#e05561"), QColor("#282c34")));

        QCOMPARE(displayAnsiColor(QColor("#cd0000"), QColor("#282c34"), true),
                 readableAnsiColor(appearanceColor("ansi.1", true, Qt::red), QColor("#282c34")));
        QSettings().setValue("appearance/dark/background", "#123456");
        QSettings().setValue("appearance/dark/active", "#abcdef");
        theme.apply(false);
        theme.apply(true);
        QCOMPARE(QSettings().value("appearance/dark/background").toString(), QString("#123456"));
        QCOMPARE(qApp->palette().color(QPalette::Accent), QColor("#abcdef"));
    }
    void appearanceSettingsAreStagedAndLayoutIsExplicit() {
        SettingsRepository repository;
        ThemeManager theme;
        theme.apply(true);
        {
            SettingsDialog dialog(repository, theme, {}, nullptr);
            dialog.findChild<QComboBox*>("appearanceControls")->setCurrentIndex(1);
            dialog.reject();
        }
        QVERIFY(!QSettings().contains("appearance/dark/controls"));
        QVERIFY(!QSettings().contains("appearance/dark/motion"));
        SettingsDialog dialog(repository, theme, {}, nullptr);
        QSignalSpy actions(&dialog, &SettingsDialog::actionRequested);
        auto* controls = dialog.findChild<QComboBox*>("appearanceControls");
        auto* layout = dialog.findChild<QComboBox*>("appearanceLayout");
        controls->setCurrentIndex(controls->findData("compact"));
        QVERIFY(dialog.apply());
        QCOMPARE(QSettings().value("appearance/dark/controls").toString(), QString("compact"));
        QVERIFY(actions.isEmpty());
        const auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        if (!destination.isEmpty()) {
            dialog.resize(1100, 780);
            dialog.show();
            for (auto* button : dialog.findChildren<QPushButton*>())
                if (button->text() == "颜色与字体")
                    button->click();
            QCoreApplication::processEvents();
            dialog.grab().save(destination + "/appearance-settings.png");
            dialog.hide();
        }
        layout->setCurrentIndex(layout->findData("columns"));
        QVERIFY(dialog.apply());
        QCOMPARE(actions.size(), 1);
        QCOMPARE(actions.first().first().toString(), QString("appearance_layout:columns"));
        QVERIFY(dialog.apply());
        QCOMPARE(actions.size(), 1); // never reapply layout on color/style Apply
        MainWindow window(repository, theme);
        const auto cleanup = qScopeGuard([&] {
            window.close();
            QTest::qWait(200);
        });
        window.resize(1200, 800);
        window.show();
        window.addTestConnection();
        window.addTestConnection();
        auto* snap = window.findChild<SnapLayout*>();
        auto* area = window.findChild<QMdiArea*>();
        auto* active = area->activeSubWindow();
        auto* pane = qobject_cast<TerminalPane*>(active->widget());
        auto* draft = pane->controlsWidget()->findChild<QPlainTextEdit*>("sendEdit");
        draft->setPlainText("preserved draft");
        auto* document = pane->terminal()->document();
        QTRY_COMPARE(pane->session()->config().sendDraft, QString("preserved draft"));
        const auto config = pane->session()->config().toJson();
        QVERIFY(snap->applyPreset("columns"));
        // Hiding the tab strip posts a viewport resize, then a queued reflow.
        // Establish the final layout before testing that color changes retain it.
        QTRY_COMPARE(active->height(), area->viewport()->height() - 8);
        const auto geometry = active->geometry();
        theme.apply(false);
        QCoreApplication::processEvents();
        QCOMPARE(active->geometry(), geometry);
        QCOMPARE(area->activeSubWindow(), active);
        QCOMPARE(pane->terminal()->document(), document);
        QCOMPARE(draft->toPlainText(), QString("preserved draft"));
        QCOMPARE(pane->session()->config().toJson(), config);
        QVERIFY(snap->applyPreset("single"));
        QCOMPARE(area->activeSubWindow(), active);
        QVERIFY(active->isMaximized());
        window.close();
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
    }
    void imeToggleShortcutIsReservedForLogWindow() {
        const QString logKey = "shortcuts/log_search", customKey = "shortcuts/clear";
        const auto previousLog = QSettings().value(logKey), previousCustom = QSettings().value(customKey);
        const auto restore = qScopeGuard([&] {
            for (const auto& entry : {qMakePair(logKey, previousLog), qMakePair(customKey, previousCustom)}) {
                if (entry.second.isValid())
                    QSettings().setValue(entry.first, entry.second);
                else
                    QSettings().remove(entry.first);
            }
        });
        QSettings().setValue(logKey, "Ctrl+Shift+F");
        QSettings().setValue(customKey, "Ctrl+Shift+F");
        SettingsRepository repository;
        ThemeManager theme;
        MainWindow window(repository, theme);
        const auto cleanup = qScopeGuard([&] {
            window.close();
            QTest::qWait(200);
        });
        window.show();
        auto* logs = window.findChild<LogWorkspace*>();
        QVERIFY(logs);
        int logShortcuts = 0;
        for (auto* shortcut : window.findChildren<QShortcut*>()) {
            if (shortcut->key() == QKeySequence("Ctrl+Shift+F")) {
                ++logShortcuts;
                QMetaObject::invokeMethod(shortcut, "activated", Qt::DirectConnection);
            } else {
                // The conflicting binding belongs to another operation, so it is not installed.
                QVERIFY(!conflictsWithImeToggle(shortcut->key()));
            }
        }
        QCOMPARE(logShortcuts, 1);
        QVERIFY(logs->isVisible());
        logs->close();
        // Drop the deliberately conflicting custom binding so only the log search
        // default still matches Ctrl+Shift+F in the editable shortcut list.
        QCOMPARE(QSettings().value(customKey).toString(), QString("Ctrl+Shift+F"));
        QSettings().remove(customKey);
        SettingsDialog dialog(repository, theme, {}, nullptr);
        int logFields = 0, disabledFields = 0;
        for (auto* key : dialog.findChildren<QKeySequenceEdit*>()) {
            // Requirement: every shortcut is editable; the log search default stays as agreed.
            disabledFields += key->isEnabled() ? 0 : 1;
            logFields += key->keySequence() == logSearchShortcut() ? 1 : 0;
        }
        QCOMPARE(disabledFields, 0);
        QCOMPARE(logFields, 1);
        dialog.reject();
        QCOMPARE(QSettings().value(logKey).toString(), QString("Ctrl+Shift+F"));
        QVERIFY(conflictsWithImeToggle(QKeySequence("Ctrl+Shift+F")));
        QVERIFY(conflictsWithImeToggle(QKeySequence("Ctrl+Shift+F, Ctrl+L")));
        QVERIFY(conflictsWithImeToggle(logSearchShortcut()));
    }
    // Space / Return reconnect a session that the user disconnected, and never emit TX.
    void disconnectedTerminalReconnectsOnResumeKey() {
        TerminalView view;
        view.setDisconnected(true);
        QSignalSpy reconnect(&view, &TerminalView::reconnectRequested);
        QSignalSpy input(&view, &TerminalView::input);
        QTest::keyClick(&view, Qt::Key_Space);
        QTest::keyClick(&view, Qt::Key_Return);
        QCOMPARE(reconnect.count(), 2);
        QCOMPARE(input.count(), 0);
        view.setDisconnected(false);
        QTest::keyClick(&view, Qt::Key_Space);
        QCOMPARE(input.count(), 1);
        QCOMPARE(reconnect.count(), 2);
    }
    // A disconnected session reconnects from the receive area, without sending bytes.
    void paneReconnectsAfterDisconnectOnResumeKey() {
        ConnectionConfig config;
        config.id = "resume-sim";
        config.name = "模拟";
        config.kind = "test";
        Session session(config);
        TerminalPane pane(&session);
        QSignalSpy tx(pane.terminal(), &TerminalView::input);
        const auto cleanup = qScopeGuard([&] {
            pane.shutdown();
            session.stop();
            QTest::qWait(200);
        });
        session.start();
        QTRY_VERIFY_WITH_TIMEOUT(session.connected(), 5000);
        session.stop();
        QTRY_VERIFY_WITH_TIMEOUT(session.isStopped(), 5000);
        QVERIFY(pane.terminal()->disconnected());
        QTest::keyClick(pane.terminal(), Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(session.connected(), 5000);
        QCOMPARE(tx.count(), 0);
        QVERIFY(!pane.terminal()->disconnected());
    }
    // A repeated live/keep search must not list the same target twice.
    void cacheSearchKeepsOneRowPerTarget() {
        ThemeManager theme;
        theme.apply(true);
        SearchDocument document;
        document.id = "doc-1";
        document.path = "demo.log";
        document.text = "alpha one\nalpha two\nalpha three\n";
        FindDialog dialog([document](bool) { return QVector<SearchDocument>{document}; },
                          [](const SearchHit&) { return true; }, true, "search/cache/test-dedupe");
        dialog.show();
        auto* query = dialog.findChild<QComboBox*>("findQuery");
        auto* results = dialog.findChild<QTreeWidget*>();
        QVERIFY(query && results);
        for (auto* box : dialog.findChildren<QCheckBox*>())
            if (box->text().contains("保留历史"))
                box->setChecked(true);
        query->setEditText("alpha");
        dialog.searchAll(false);
        QTRY_COMPARE_WITH_TIMEOUT(results->topLevelItemCount(), 3, 5000);
        dialog.searchAll(false);
        QTest::qWait(400);
        QCOMPARE(results->topLevelItemCount(), 3);
    }
    // The query drops down the workspace's own history, and its last entry clears it.
    void searchHistoryIsPerPreferenceKeyAndClearable() {
        ThemeManager theme;
        theme.apply(true);
        const QString key = "search/cache/test-history";
        const auto previous = QSettings().value(key);
        const auto restore = qScopeGuard([&] {
            if (previous.isValid())
                QSettings().setValue(key, previous);
            else
                QSettings().remove(key);
        });
        {
            QSettings settings;
            settings.beginGroup(key);
            settings.remove("");
            settings.endGroup();
        }
        SearchDocument document;
        document.id = "doc-history";
        document.path = "history.log";
        document.text = "alpha\nbeta\n";
        FindDialog dialog([document](bool) { return QVector<SearchDocument>{document}; },
                          [](const SearchHit&) { return true; }, true, key);
        dialog.show();
        auto* query = dialog.findChild<QComboBox*>("findQuery");
        QVERIFY(query);
        query->setEditText("alpha");
        dialog.searchAll(false);
        QTRY_VERIFY_WITH_TIMEOUT(query->count() >= 3, 5000);
        QCOMPARE(query->itemText(0), QString("alpha"));
        QCOMPARE(query->itemText(query->count() - 1), QString("清除所有搜索历史"));
        QMetaObject::invokeMethod(query, "activated", Qt::DirectConnection, Q_ARG(int, query->count() - 1));
        QCOMPARE(query->count(), 0);
        QSettings settings;
        settings.beginGroup(key);
        QVERIFY(!settings.contains("history"));
        settings.endGroup();
    }
    // A new log search opens its own group and folds the older result groups.
    void newestSearchGroupFoldsOlderOnes() {
        QTemporaryDir dir;
        QFile file(dir.filePath("fold.log"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("alpha one\nalpha two\nbeta one\n");
        file.close();
        LogDocumentView view;
        view.show();
        view.openPath(file.fileName());
        auto* tabs = view.findChild<QTabWidget*>("logDocumentTabs");
        auto* tree = view.findChild<QTreeWidget*>();
        QVERIFY(tabs && tree);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 10000);
        view.search();
        auto* finder = view.findChild<FindDialog*>();
        auto* query = finder ? finder->findChild<QComboBox*>("findQuery") : nullptr;
        QVERIFY(finder && query);
        query->setEditText("alpha");
        finder->searchAll(false);
        QTRY_VERIFY_WITH_TIMEOUT(tree->topLevelItemCount() >= 1, 10000);
        query->setEditText("beta");
        finder->searchAll(false);
        QTRY_VERIFY_WITH_TIMEOUT(tree->topLevelItemCount() >= 2, 10000);
        for (int index = 0; index < tree->topLevelItemCount() - 1; ++index)
            QVERIFY(!tree->topLevelItem(index)->isExpanded());
        QVERIFY(tree->topLevelItem(tree->topLevelItemCount() - 1)->isExpanded());
    }
    // Opening settings starts on the connection of the active pane.
    void settingsOpenOnActiveConnection() {
        SettingsRepository repository;
        ThemeManager theme;
        QString error;
        QVector<ConnectionConfig> configs(2);
        configs[0].id = "settings-a";
        configs[0].name = "A";
        configs[0].kind = "test";
        configs[1].id = "settings-b";
        configs[1].name = "B";
        configs[1].kind = "test";
        QVERIFY(repository.saveConnections(configs, error));
        SettingsDialog dialog(repository, theme, {}, nullptr, nullptr, {}, "settings-b");
        auto* connections = dialog.findChild<QComboBox*>("settingsConnections");
        QVERIFY(connections);
        QCOMPARE(connections->currentData().toString(), QString("settings-b"));
        dialog.reject();
    }
    void resultSelectionKeepsTextPosition() {
        ThemeManager theme;
        theme.apply(true);
        QTreeWidget tree;
        tree.setColumnCount(3);
        tree.setRootIsDecorated(false);
        tree.setItemDelegate(new StableResultDelegate(&tree));
        auto* item = new QTreeWidgetItem(&tree, {"9982", "[NOTICE] configuration reloaded 中文", "有效"});
        QStyleOptionViewItem option;
        option.initFrom(&tree);
        option.rect = QRect(0, 0, 360, 40);
        option.widget = &tree;
        option.font = tree.font();
        option.palette.setColor(QPalette::Text, Qt::white);
        option.palette.setColor(QPalette::HighlightedText, Qt::white);
        auto record = [&](bool selected) {
            TextProbe device;
            QPainter painter(&device);
            auto state = option;
            state.state.setFlag(QStyle::State_Selected, selected);
            state.state.setFlag(QStyle::State_HasFocus, selected);
            tree.itemDelegate()->paint(&painter, state, tree.indexFromItem(item, 1));
            painter.end();
            return device.engine.draws;
        };
        const auto normal = record(false), selected = record(true);
        QVERIFY(!normal.isEmpty());
        QCOMPARE(normal.size(), selected.size());
        for (int i = 0; i < normal.size(); ++i) {
            QCOMPARE(normal[i].origin, selected[i].origin);
            QCOMPARE(normal[i].font, selected[i].font);
            QCOMPARE(normal[i].text, selected[i].text);
        }
    }

    void primaryButtonsKeepContrastWithCustomAccent() {
        ThemeManager theme;
        auto luminance = [](QColor color) {
            auto linear = [](double value) {
                return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
            };
            return .2126 * linear(color.redF()) + .7152 * linear(color.greenF()) +
                   .0722 * linear(color.blueF());
        };
        for (bool dark : {true, false}) {
            const QString key = dark ? "appearance/dark/active" : "appearance/light/active";
            const auto previous = QSettings().value(key);
            const auto cleanup = qScopeGuard([&] {
                if (previous.isValid())
                    QSettings().setValue(key, previous);
                else
                    QSettings().remove(key);
            });
            for (const auto* accent : {"#344054", "#70bcf4", "#0067c0", "#f3c821"}) {
                QSettings().setValue(key, accent);
                theme.apply(dark);
                QPushButton button("查找 / 保存 / 发送");
                button.setProperty("primary", true);
                button.ensurePolished();
                const auto foreground = luminance(button.palette().color(QPalette::ButtonText));
                const auto background = luminance(button.palette().color(QPalette::Button));
                const auto contrast =
                    (qMax(foreground, background) + .05) / (qMin(foreground, background) + .05);
                QVERIFY2(contrast >= 4.5,
                         qPrintable(QString("Accent %1 contrast %2").arg(accent).arg(contrast)));
            }
        }
    }
    void cacheResultSelectionViews() {
        ThemeManager theme;
        theme.apply(true);
        QWidget host;
        host.show();
        const auto provider = [](bool) {
            SearchDocument source;
            source.id = "selection-preview";
            source.path = "t3";
            for (int i = 0; i < 100; ++i)
                source.text += QString("[%1] [NOTICE] configuration reloaded 中文\n").arg(i);
            return QVector<SearchDocument>{source};
        };
        FindDialog dialog(
            provider, [](const SearchHit&) { return true; }, true, "tests/cacheSelection", &host);
        dialog.resize(760, 530);
        dialog.openFind();
        auto options = dialog.options();
        options.query = "configuration";
        dialog.setOptions(options);
        QSignalSpy finished(&dialog, &FindDialog::allSearchFinished);
        dialog.searchAll();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        auto* results = dialog.findChild<QTreeWidget*>("cacheSearchResults");
        QVERIFY(results);
        QVERIFY(results->topLevelItemCount() >= 100);
        results->clearSelection();
        results->setCurrentItem(nullptr);
        auto* item = results->topLevelItem(3);
        const auto rect = results->visualItemRect(item);
        const auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        QTest::qWait(50);
        if (!destination.isEmpty())
            dialog.grab().save(destination + "/cache-results-normal.png");
        results->setCurrentItem(item);
        results->setFocus();
        QCoreApplication::processEvents();
        QCOMPARE(results->visualItemRect(item), rect);
        if (!destination.isEmpty())
            dialog.grab().save(destination + "/cache-results-selected.png");
        // A selected row remains a stable snapshot despite live receive ticks.
        for (auto* check : dialog.findChildren<QCheckBox*>())
            if (check->text() == "实时更新")
                check->setChecked(true);
        for (int i = 0; i < 5; ++i)
            dialog.tickLive();
        QTest::qWait(350);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(results->currentItem(), item);
        QCOMPARE(results->visualItemRect(item), rect);
        dialog.close();
        QSettings().remove("tests/cacheSelection");
    }
    void dragDisplayPauseKeepsBackgroundLogging() {
        QTemporaryDir directory;
        ConnectionConfig config;
        config.kind = "test";
        config.name = "drag-test";
        config.autoLog = true;
        config.rotate = false;
        config.logDirectory = directory.path();
        Session session(config);
        TerminalPane pane(&session);
        pane.resize(800, 500);
        pane.show();
        QSignalSpy stopped(&session, &Session::stopped);
        session.start();
        QTRY_VERIFY_WITH_TIMEOUT(!pane.terminal()->toPlainText().isEmpty(), 5000);
        pane.setInteractionPaused(true);
        const auto frozen = pane.terminal()->toPlainText();
        const auto bytesOnDisk = [&] {
            qint64 bytes = 0;
            for (const auto& file : QDir(directory.path()).entryInfoList(QDir::Files))
                bytes += file.size();
            return bytes;
        };
        const auto before = bytesOnDisk();
        QTest::qWait(1300);
        const bool displayFrozen = pane.terminal()->toPlainText() == frozen;
        const bool loggingContinued = bytesOnDisk() > before;
        pane.setInteractionPaused(false);
        QTest::qWait(100);
        const bool displayResumed = pane.terminal()->toPlainText() != frozen;
        pane.pauseDisplay(true);
        pane.setInteractionPaused(true);
        const auto explicitlyPaused = pane.terminal()->toPlainText();
        QTest::qWait(80);
        pane.setInteractionPaused(false);
        QTest::qWait(80);
        const bool userPausePreserved = pane.terminal()->toPlainText() == explicitlyPaused;
        pane.shutdown();
        session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.size(), 1, 5000);
        QVERIFY(displayFrozen);
        QVERIFY(loggingContinued);
        QVERIFY(displayResumed);
        QVERIFY(userPausePreserved);
    }
    void floatingSearchRemembersGeometry() {
        QWidget host;
        host.resize(900, 700);
        host.show();
        for (const bool cache : {false, true}) {
            const QString key = cache ? "search/windows/cache/geometry" : "search/windows/log/geometry";
            const QString preferences = "tests/floatingSearch/" + QString::number(cache);
            const auto previous = QSettings().value(key);
            const auto cleanup = qScopeGuard([&] {
                if (previous.isValid())
                    QSettings().setValue(key, previous);
                else
                    QSettings().remove(key);
                QSettings().remove(preferences);
            });
            QSettings().remove(key);
            const auto provider = [](bool) { return QVector<SearchDocument>{}; };
            const auto activate = [](const SearchHit&) { return false; };
            QRect saved;
            {
                FindDialog dialog(provider, activate, cache, preferences, &host);
                dialog.openFind();
                QVERIFY(dialog.isWindow());
                QVERIFY(!dialog.isModal());
                dialog.setGeometry(30, 40, 720, 540);
                QCoreApplication::processEvents();
                saved = dialog.geometry();
                // Ctrl+F on an already open window must not reset its geometry.
                dialog.openFind();
                QCOMPARE(dialog.geometry(), saved);
                QTest::keyClick(&dialog, Qt::Key_Escape);
                QVERIFY(!dialog.isVisible());
                dialog.openFind();
                QCoreApplication::processEvents();
                QCOMPARE(dialog.geometry(), saved);
                dialog.close();
            }
            // A recreated window (including a different connection/workspace)
            // reuses the last geometry for this search kind.
            FindDialog restored(provider, activate, cache, preferences, &host);
            restored.openFind();
            QCoreApplication::processEvents();
            QCOMPARE(restored.geometry(), saved);
            if (!cache) {
                auto* tabs = restored.findChild<QTabBar*>("findModeTabs");
                QVERIFY(tabs);
                tabs->setCurrentIndex(1);
                QCoreApplication::processEvents();
                QCOMPARE(restored.geometry(), saved);
                restored.openFind();
                QCOMPARE(restored.geometry(), saved);
            }
            restored.close();
        }
    }
    void ansiSelectionPreservesMetrics() {
        // Captured from the reference escook_theme.readable implementation.
        QCOMPARE(readableAnsiColor(QColor("#cd0000"), QColor("#282c34")).name(), QString("#ff7b7b"));
        QCOMPARE(readableAnsiColor(QColor("#0000ee"), QColor("#282c34")).name(), QString("#a4a4ff"));
        QCOMPARE(readableAnsiColor(QColor("#00cd00"), QColor("#fdf6e3")).name(), QString("#008200"));
        ThemeManager theme;
        theme.apply(true);
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        auto editor = makeEditor(&host);
        layout->addWidget(editor->widget());
        editor->setAppearance(QFont("Consolas", 11), QColor("#282c34"), QColor("#abb2bf"));
        QString text;
        QVector<EditorAnsiSpan> spans;
        QVector<int> offsets;
        for (int i = 0; i < 48; ++i) {
            offsets.push_back(text.toUtf8().size());
            const auto line =
                QString("[%1] ANSI 日志 selection/font alignment 中文\n").arg(i, 4, 10, QChar('0'));
            spans.push_back({text.size(), line.size(),
                             static_cast<int>(QColor::fromHsv(i * 7, 220, 230).rgb() & 0xffffff), -1, false});
            text += line;
        }
        editor->setText(text);
        editor->setAnsiStyles(spans, true);
        auto* native = qobject_cast<QsciScintilla*>(editor->widget());
        QVERIFY(native);
        QVector<int> styles;
        for (int offset : offsets) {
            const int style = native->SendScintilla(QsciScintillaBase::SCI_GETSTYLEAT, offset);
            QVERIFY(style < 32 || style >= 40);
            QCOMPARE(native->SendScintilla(QsciScintillaBase::SCI_STYLEGETSIZE, style), 11L);
            styles.push_back(style);
        }
        QCOMPARE(native->SendScintilla(QsciScintillaBase::SCI_STYLEGETSIZE, QsciScintillaBase::STYLE_DEFAULT),
                 11L);
        QCOMPARE(
            native->SendScintilla(QsciScintillaBase::SCI_STYLEGETSIZE, QsciScintillaBase::STYLE_LINENUMBER),
            11L);
        host.resize(1060, 690);
        host.show();
        editor->selectRange(text.indexOf("[0018]"), 40);
        native->setFocus();
        QTest::keyClick(native, Qt::Key_Right);
        for (int i = 0; i < offsets.size(); ++i)
            QCOMPARE(native->SendScintilla(QsciScintillaBase::SCI_GETSTYLEAT, offsets[i]), long(styles[i]));
        QCOMPARE(editor->text(), text);
        const auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        if (!destination.isEmpty())
            host.grab().save(destination + "/cpp-log-selection-metrics.png");
    }
    void logWorkspaceLayoutRefreshAndRestore() {
        QTemporaryDir directory;
        const QStringList names{"imx6u(2026-10-23 10_40_53).txt", "imx6u(2026-10-23 14_43_26).txt",
                                "test1(2026-10-23 22_00_09).txt", "test1(2026-10-23 22_04_45).txt"};
        QStringList paths;
        QByteArray content;
        const QStringList colors{"32", "33", "31", "36", "35", "38;5;39", "38;2;0;200;140"};
        const QStringList messages{"[INFO] test connection ready; request completed",
                                   "[WARN] simulated temperature warning; 72 C",
                                   "[ERROR] simulated timeout; retry scheduled",
                                   "[DEBUG] rx=AA 55 01 02; checksum OK",
                                   "[NOTICE] configuration reloaded",
                                   "[TRACE] 256-color output; processing next sample",
                                   "[INFO] RGB color output; log saved"};
        for (int i = 0; i < 180; ++i)
            content +=
                QString("[22:00:09.361] Oct 23 22:00:09 test1 service[%1]: \x1b[%2m%3 serial receive stress "
                        "sample; UTF-8 中文; ANSI colors; scroll/search responsiveness.\x1b[0m\r\n")
                    .arg(53267 + i, 8, 10, QChar('0'))
                    .arg(colors[i % 7], messages[i % 7])
                    .toUtf8();
        for (const auto& name : names) {
            QFile file(directory.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(content);
            paths.push_back(file.fileName());
        }
        ThemeManager theme;
        theme.apply(true);
        LogWorkspace workspace;
        workspace.resize(1352, 862);
        workspace.registerConnection("one", "test1");
        workspace.registerConnection("two", "test2");
        workspace.registerConnection("three", "test3");
        workspace.activateConnection("board", "IMX6U");
        workspace.show();
        auto* workspaceTabs = workspace.findChild<QTabBar*>("logWorkspaceTabs");
        const auto boardId = workspaceTabs->tabData(workspaceTabs->currentIndex()).toString();
        auto* original = workspace.activeView();
        for (const auto& path : paths)
            workspace.openPath(path);
        auto* fileTabs = original->findChild<QTabWidget*>("logDocumentTabs");
        QTRY_COMPARE_WITH_TIMEOUT(fileTabs->count(), 4, 5000);
        fileTabs->setCurrentIndex(2);
        auto* native = qobject_cast<QsciScintilla*>(fileTabs->currentWidget());
        QVERIFY(native);
        original->search();
        auto* finder = original->findChild<FindDialog*>();
        QVERIFY(finder);
        auto options = finder->options();
        options.query = "warning";
        options.mode = 0;
        finder->setOptions(options);
        finder->close();
        for (int i = 0; i < 2; ++i) {
            auto* buttons = fileTabs->tabBar()->tabButton(i, QTabBar::RightSide);
            buttons->findChild<QToolButton*>("logTabPin")->setChecked(true);
        }
        native->setCursorPosition(25, 62);
        original->applyMarks({{10, 8, 2, false}}, true);
        native->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE, 18UL);
        const auto secondId = workspace.createWorkspace("test1 (2)", true);
        QVERIFY(!secondId.isEmpty());
        auto* copy = workspace.activeView();
        QVERIFY(copy != original);
        QCOMPARE(copy->searchSources(true).size(), 4);
        QCOMPARE(copy->searchSources(false).first().text, original->searchSources(false).first().text);
        auto* copyNative =
            qobject_cast<QsciScintilla*>(copy->findChild<QTabWidget*>("logDocumentTabs")->currentWidget());
        QVERIFY(copyNative->SendScintilla(QsciScintillaBase::SCI_INDICATORVALUEAT, 10UL, 10L) != 0);
        const auto thirdId = workspace.createWorkspace("test1 (3)", true);
        QVERIFY(!thirdId.isEmpty());
        QCOMPARE(workspaceTabs->count(), 6);
        workspace.activateWorkspace(boardId);
        QCOMPARE(workspace.activeView(), original);
        QCOMPARE(fileTabs->currentWidget(), native);
        auto* location = original->findChild<QLabel*>("logReaderLocation");
        QTRY_VERIFY(location->text().contains("行 26, 列 63"));
        QVERIFY(location->text().contains("Windows (CR LF)"));
        QVERIFY(!workspace.findChild<QToolBar*>());
        const auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        if (!destination.isEmpty()) {
            QDir().mkpath(destination);
            workspace.grab().save(destination + "/cpp-log-layout.png");
        }
        auto* menu = workspace.findChild<QToolButton*>("workspaceMenuButton")->menu();
        menu->aboutToShow();
        QStringList actions;
        for (auto* action : menu->actions())
            if (!action->isSeparator())
                actions.push_back(action->text());
        QCOMPARE(actions,
                 QStringList({"打开日志…\tCtrl+O", "新建工作区…", "另存为新工作区…",
                              "保存 / 更新工作区与布局", "重命名当前工作区…", "恢复单窗口", "整个工作区"}));
        QSignalSpy operations(&workspace, &LogWorkspace::workspaceOperationRequested);
        menu->actions().last()->menu()->actions().at(2)->trigger();
        QCOMPARE(operations.count(), 1);
        QCOMPARE(operations.first().first().toString(), QString("backup_workspace"));
        if (!destination.isEmpty() && QGuiApplication::platformName() == "windows") {
            auto* button = workspace.findChild<QToolButton*>("workspaceMenuButton");
            menu->popup(
                button->mapToGlobal(QPoint(button->width() - menu->sizeHint().width(), button->height())));
            QTest::qWait(70);
            menu->grab().save(destination + "/cpp-log-workspace-menu.png");
            menu->hide();
        }
        workspace.detachWorkspace(boardId);
        QVERIFY(original->window() != &workspace);
        original->window()->setGeometry(30, 40, 620, 460);
        QTest::qWait(30);
        const auto floatingGeometry = original->window()->geometry();
        original->window()->close();
        workspace.detachWorkspace(boardId);
        QTest::qWait(30);
        QCOMPARE(original->window()->geometry(), floatingGeometry);
        QCOMPARE(fileTabs->currentWidget(), native);
        workspace.restoreSingleWindow();
        QCOMPARE(original->window(), &workspace);
        QCOMPARE(fileTabs->currentWidget(), native);
        auto append = [&](const QByteArray& bytes) {
            QFile file(paths[2]);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Append))
                return false;
            return file.write(bytes) == bytes.size();
        };
        QVERIFY(append("manual refresh appended\r\n"));
        original->findChild<QPushButton*>("logManualRefresh")->click();
        QTRY_VERIFY_WITH_TIMEOUT(
            original->searchSources(false).first().text.contains("manual refresh appended"), 5000);
        int line, column;
        native->getCursorPosition(&line, &column);
        QCOMPARE(line, 25);
        QCOMPARE(column, 62);
        QVERIFY(native->SendScintilla(QsciScintillaBase::SCI_INDICATORVALUEAT, 10UL, 10L) != 0);
        auto* automatic = original->findChild<QCheckBox*>("logAutoRefresh");
        automatic->setChecked(true);
        QVERIFY(append("automatic refresh appended\r\n"));
        QTRY_VERIFY_WITH_TIMEOUT(
            original->searchSources(false).first().text.contains("automatic refresh appended"), 3500);
        native->getCursorPosition(&line, &column);
        QCOMPARE(line, 25);
        QCOMPARE(column, 62);
        QCOMPARE(fileTabs->currentWidget(), native);
        auto* windowPin = workspace.findChild<QToolButton*>("windowPin");
        windowPin->setChecked(true);
        QVERIFY(!workspace.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
        workspace.saveWorkspaces();
        workspace.close();
        LogWorkspace restored;
        restored.show();
        QTRY_COMPARE_WITH_TIMEOUT(restored.searchSources(true).size(), 4, 5000);
        QCOMPARE(restored.windowTitle(), QString("IMX6U"));
        QVERIFY(restored.findChild<QToolButton*>("windowPin")->isChecked());
        QVERIFY(!restored.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
        QCOMPARE(restored.findChild<QTabBar*>("logWorkspaceTabs")->count(), 6);
        QVERIFY(!restored.findChild<FindDialog*>());
        QVERIFY(!restored.activeView()->findChild<QCheckBox*>("logAutoRefresh")->isChecked());
        auto* restoredTabs = restored.activeView()->findChild<QTabWidget*>("logDocumentTabs");
        QCOMPARE(restoredTabs->currentIndex(), 2);
        QCOMPARE(restoredTabs->tabBar()
                     ->tabButton(0, QTabBar::RightSide)
                     ->findChild<QToolButton*>("logTabPin")
                     ->isChecked(),
                 true);
        restored.search();
        QCOMPARE(restored.activeView()->findChild<FindDialog*>()->options().query, QString("warning"));
        restored.close();
    }
    void logFindVisibilityAndTabOrder() {
        QTemporaryDir directory;
        for (const auto& name : {"one.log", "two.log"}) {
            QFile file(directory.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(name == QString("one.log") ? "needle one\nneedle two\n" : "second document\n");
        }
        ThemeManager theme;
        theme.apply(true);
        LogWorkspace workspace;
        workspace.resize(1100, 760);
        workspace.openPath(directory.filePath("one.log"));
        workspace.openPath(directory.filePath("two.log"));
        auto* tabs = workspace.findChild<QTabWidget*>("logDocumentTabs");
        auto* panel = workspace.findChild<QWidget*>("logResultsPanel");
        QVERIFY(tabs && panel);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 5000);
        QVERIFY(panel->isHidden());
        QVERIFY(!workspace.findChild<FindDialog*>());
        const auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        if (!destination.isEmpty()) {
            QDir().mkpath(destination);
            workspace.grab().save(destination + "/cpp-log-open-no-search.png");
        }
        tabs->tabBar()->moveTab(0, 1);
        QCOMPARE(workspace.searchSources(true).first().text, QString("second document\n"));
        QCOMPARE(workspace.searchSources(true).last().text, QString("needle one\nneedle two\n"));
        tabs->setCurrentIndex(1);
        QTest::keyClick(&workspace, Qt::Key_F, Qt::ControlModifier);
        auto* finder = workspace.findChild<FindDialog*>();
        QTRY_VERIFY(finder && finder->isVisible());
        QVERIFY(finder->isWindow());
        QVERIFY(panel->isHidden());
        auto options = finder->options();
        options.query = "needle";
        finder->setOptions(options);
        QSignalSpy finished(finder, &FindDialog::allSearchFinished);
        QPushButton* next = nullptr;
        for (auto* button : finder->findChildren<QPushButton*>())
            if (button->text() == "查找下一处")
                next = button;
        QVERIFY(next);
        next->click();
        QTRY_COMPARE(workspace.searchSources(false).first().selectionEnd, qsizetype(6));
        QVERIFY(panel->isHidden());
        QCOMPARE(finished.count(), 0);
        if (!destination.isEmpty())
            finder->grab().save(destination + "/cpp-log-ctrl-f.png");
        finder->searchAll();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QVERIFY(panel->isVisible());
        auto* splitter = workspace.findChild<QSplitter*>();
        QVERIFY(splitter);
        QTRY_VERIFY(splitter->sizes().at(1) <= 220);
        const auto firstFinish = finished.takeFirst();
        QCOMPARE(firstFinish.at(0).toInt(), 2);
        QCOMPARE(firstFinish.at(1).toInt(), 1);
        auto* resultTree = workspace.findChild<QTreeWidget*>();
        QCOMPARE(resultTree->columnCount(), 1);
        QCOMPARE(resultTree->topLevelItemCount(), 1);
        QCOMPARE(resultTree->topLevelItem(0)->childCount(), 1);
        QCOMPARE(resultTree->topLevelItem(0)->child(0)->childCount(), 2);
        auto* firstHit = resultTree->topLevelItem(0)->child(0)->child(0);
        resultTree->setCurrentItem(firstHit);
        firstHit->setSelected(true);
        resultTree->setFocus();
        QTest::keyClick(resultTree, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(), QString("行 1: needle one"));
        options.query = "not-present";
        finder->setOptions(options);
        finder->searchAll();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        const auto secondFinish = finished.takeFirst();
        QCOMPARE(secondFinish.at(0).toInt(), 0);
        QCOMPARE(secondFinish.at(1).toInt(), 1);
        QCOMPARE(resultTree->topLevelItemCount(), 2);
        QCOMPARE(resultTree->topLevelItem(1)->childCount(), 1);
        QTRY_VERIFY(resultTree->visualItemRect(resultTree->topLevelItem(1)).top() >= 0);
        QVERIFY(resultTree->visualItemRect(resultTree->topLevelItem(1)).bottom() <=
                resultTree->viewport()->height());
        if (!destination.isEmpty())
            workspace.grab().save(destination + "/cpp-log-results.png");
        finder->close();
        QVERIFY(panel->isVisible());
        workspace.findChild<QToolButton*>("closeLogResults")->click();
        QVERIFY(panel->isHidden());
        workspace.search();
        QVERIFY(panel->isHidden());
        workspace.close();
        QVERIFY(!finder->isVisible());
        workspace.show();
        QVERIFY(!finder->isVisible());
        workspace.close();
    }
    void connectionTabSnapAndCancel() {
        SettingsRepository repository;
        ThemeManager theme;
        theme.apply(true);
        MainWindow window(repository, theme);
        window.resize(1280, 820);
        window.show();
        window.addTestConnection();
        window.addTestConnection();
        auto* logWindow = window.findChild<LogWorkspace*>();
        QVERIFY(logWindow);
        for (auto* action : window.findChildren<QAction*>())
            if (action->objectName() == "toolbar_log")
                action->trigger();
        QVERIFY(logWindow->isVisible());
        QVERIFY(!logWindow->findChild<FindDialog*>());
        logWindow->close();
        auto* tabs = window.findChild<QTabBar*>("connectionTabs");
        auto* area = window.findChild<QMdiArea*>();
        QVERIFY(tabs && area);
        QTest::qWait(100);
        const auto windows = area->subWindowList();
        QCOMPARE(windows.size(), 2);
        auto dragMove = [&](const QPoint& global) {
            QMouseEvent move(QEvent::MouseMove, QPointF(tabs->mapFromGlobal(global)), QPointF(global),
                             Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(tabs, &move);
        };
        auto begin = [&](int tab) {
            if (!tabs->isTabVisible(tab)) {
                auto* header = windows[tab]->findChild<QWidget*>("terminalPaneHeader");
                QTest::mousePress(header, Qt::LeftButton, Qt::NoModifier, QPoint(30, 10));
                const auto global = area->viewport()->mapToGlobal(QPoint(area->viewport()->width() / 2, 12));
                QMouseEvent move(QEvent::MouseMove, QPointF(header->mapFromGlobal(global)), QPointF(global),
                                 Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(header, &move);
                return;
            }
            QTest::mousePress(tabs, Qt::LeftButton, Qt::NoModifier, tabs->tabRect(tab).center());
            dragMove(area->viewport()->mapToGlobal(QPoint(area->viewport()->width() / 2, 12)));
        };
        auto releaseAt = [&](const QPoint& global) {
            QTest::mouseRelease(tabs, Qt::LeftButton, Qt::NoModifier, tabs->mapFromGlobal(global));
        };
        auto* chooser = area->findChild<QWidget*>("snapChooser");
        auto* preview = area->findChild<QWidget*>("snapPreview");
        QVERIFY(chooser && preview);
        begin(0);
        for (auto* pane : window.findChildren<TerminalPane*>())
            QVERIFY(pane->interactionPaused());
        QVERIFY(chooser->isVisible());
        QVERIFY(chooser->property("expanded").toBool());
        // Seven thumbnails, each with the same first-layout two halves as the reference.
        const double cellWidth = (chooser->width() - 24.0 - 14 * 6) / 7;
        const auto left = chooser->mapToGlobal(QPoint(qRound(12 + cellWidth / 4), 50));
        const auto right = chooser->mapToGlobal(QPoint(qRound(12 + cellWidth * 3 / 4), 50));
        dragMove(left);
        QVERIFY(preview->isVisible());
        const auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        if (!destination.isEmpty()) {
            QDir().mkpath(destination);
            window.grab().save(destination + "/cpp-terminal-snap-chooser.png");
        }
        releaseAt(left);
        for (auto* pane : window.findChildren<TerminalPane*>())
            QVERIFY(!pane->interactionPaused());
        QVERIFY(chooser->isHidden());
        QVERIFY(preview->isHidden());
        QVERIFY(!windows[0]->isMaximized());
        QCOMPARE(windows[0]->geometry().left(), 4);
        QVERIFY(windows[0]->width() <= area->viewport()->width() / 2);
        begin(1);
        dragMove(right);
        releaseAt(right);
        QVERIFY(tabs->isHidden());
        QVERIFY(windows[1]->geometry().left() > windows[0]->geometry().right());
        const auto before = windows[0]->geometry();
        begin(0);
        QTest::keyClick(tabs, Qt::Key_Escape);
        releaseAt(right);
        QCOMPARE(windows[0]->geometry(), before);
        for (auto* pane : window.findChildren<TerminalPane*>())
            QVERIFY(!pane->interactionPaused());
        auto* firstPane = qobject_cast<TerminalPane*>(windows[0]->widget());
        auto* secondPane = qobject_cast<TerminalPane*>(windows[1]->widget());
        area->setActiveSubWindow(windows[1]);
        QVERIFY(secondPane->property("activeConnection").toBool());
        QVERIFY(!firstPane->property("activeConnection").toBool());
        area->setActiveSubWindow(windows[0]);
        QVERIFY(firstPane->property("activeConnection").toBool());
        QVERIFY(!secondPane->property("activeConnection").toBool());
        auto* outline = firstPane->findChild<QWidget*>("activeConnectionOutline");
        QVERIFY(outline && outline->isVisible());
        QCOMPARE(outline->geometry(), firstPane->rect());
        QVERIFY(outline->testAttribute(Qt::WA_TransparentForMouseEvents));
        QVERIFY(chooser->isHidden());
        QTRY_COMPARE(window.findChild<QSplitter*>("workspaceSplitter")->y(), 0);
        QVERIFY(tabs->isHidden());
        if (!destination.isEmpty())
            window.grab().save(destination + "/cpp-terminal-snap-two-panes.png");
        window.close();
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
    }
    void sharedControlsAndStatusAfterDetach() {
        SettingsRepository repository;
        ThemeManager theme;
        theme.apply(true);
        MainWindow window(repository, theme);
        window.resize(1280, 820);
        window.show();
        window.addTestConnection();
        window.addTestConnection();
        auto* area = window.findChild<QMdiArea*>();
        auto* tabs = window.findChild<QTabBar*>("connectionTabs");
        auto* stack = window.findChild<QStackedWidget*>("sharedConnectionControls");
        auto* bottomStatus = window.findChild<QLabel*>("mainConnectionStatus");
        QVERIFY(bottomStatus);
        const auto panes = window.findChildren<TerminalPane*>();
        QCOMPARE(panes.size(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(panes[0]->session()->connected() && panes[1]->session()->connected(), 5000);
        const auto windows = area->subWindowList();
        auto* draftA = panes[0]->controlsWidget()->findChild<QPlainTextEdit*>("sendEdit");
        auto* draftB = panes[1]->controlsWidget()->findChild<QPlainTextEdit*>("sendEdit");
        draftA->setPlainText("draft A");
        draftB->setPlainText("draft B");
        QTest::mouseClick(panes[0]->terminal()->viewport(), Qt::LeftButton);
        QCOMPARE(stack->currentWidget(), panes[0]->controlsWidget());
        QVERIFY(draftA->isVisible());
        QVERIFY(!draftB->isVisible());
        area->setActiveSubWindow(windows[1]);
        QCOMPARE(stack->currentWidget(), panes[1]->controlsWidget());
        QVERIFY(bottomStatus->text().contains("test2"));
        QVERIFY(!bottomStatus->text().contains("RX "));
        QVERIFY(!bottomStatus->text().contains("PortWave 0.1.0"));
        QCOMPARE(draftA->toPlainText(), QString("draft A"));
        QCOMPARE(draftB->toPlainText(), QString("draft B"));
        auto* snap = window.findChild<SnapLayout*>();
        QVERIFY(snap);
        for (int i = 0; i < windows.size(); ++i) {
            windows[i]->showNormal();
            snap->paneDetached(windows[i]);
            QCOMPARE(tabs->isTabVisible(i), false);
        }
        QVERIFY(tabs->isHidden());
        QTest::qWait(30);
        const auto rect = area->viewport()->rect();
        windows[0]->setGeometry(4, 4, rect.width() / 2 - 8, rect.height() - 8);
        windows[1]->setGeometry(rect.width() / 2 + 4, 4, rect.width() / 2 - 8, rect.height() - 8);
        QTest::mouseClick(panes[0]->terminal()->viewport(), Qt::LeftButton);
        QCOMPARE(stack->currentWidget(), panes[0]->controlsWidget());
        auto* target = panes[0]->controlsWidget()->findChild<QComboBox*>("sendTargetCombo");
        const int index = target->findData(panes[1]->session()->config().id);
        QVERIFY(index >= 0);
        target->setCurrentIndex(index);
        target->activated(index);
        QCOMPARE(target->currentData().toString(), panes[0]->session()->config().id);
        QCOMPARE(area->activeSubWindow(), windows[1]);
        QCOMPARE(stack->currentWidget(), panes[1]->controlsWidget());
        QCOMPARE(draftA->toPlainText(), QString("draft A"));
        QCOMPARE(draftB->toPlainText(), QString("draft B"));
        panes[0]->session()->logPathChanged("C:/long-folder/very-long-subdirectory/连接一.log");
        auto* fileLabel = panes[0]->fileStatusWidget();
        QCOMPARE(fileLabel->window(), &window);
        QCOMPARE(fileLabel->text(), QString("存盘文件：连接一.log"));
        QVERIFY(fileLabel->toolTip().startsWith("C:/long-folder/very-long-subdirectory/连接一.log"));
        QVERIFY(panes[0]->connectionStatusText().contains("接收模式：常规串口"));
        auto* dockButton = panes[0]->findChild<QToolButton*>("dockTerminal");
        QVERIFY(dockButton);
        QVERIFY(dockButton->text() != "吸回");
        auto* sendPin = panes[0]->controlsWidget()->findChild<QToolButton*>("senderPin");
        QVERIFY(sendPin);
        sendPin->setChecked(false);
        QVERIFY(!panes[1]->controlsWidget()->findChild<QToolButton*>("senderPin")->isChecked());
        sendPin->setChecked(true);
        QVERIFY(panes[1]->controlsWidget()->findChild<QToolButton*>("senderPin")->isChecked());
        panes[0]->setAutomaticSending(true);
        theme.apply(false);
        theme.apply(true);
        QVERIFY(panes[0]->automaticSending());
        QVERIFY(!panes[1]->automaticSending());
        QCOMPARE(draftA->toPlainText(), QString("draft A"));
        QCOMPARE(draftB->toPlainText(), QString("draft B"));
        int visiblePages = 0;
        for (auto* pane : panes)
            visiblePages += pane->controlsWidget()->isVisible() ? 1 : 0;
        QCOMPARE(visiblePages, 1);
        const auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        if (!destination.isEmpty()) {
            QDir().mkpath(destination);
            window.grab().save(destination + "/cpp-shared-sender-all-detached.png");
        }
        panes[0]->findChild<QToolButton*>("dockTerminal")->click();
        QVERIFY(tabs->isVisible());
        QVERIFY(tabs->isTabVisible(0));
        QVERIFY(!tabs->isTabVisible(1));
        QCOMPARE(stack->currentWidget(), panes[0]->controlsWidget());
        if (!destination.isEmpty()) {
            QDir().mkpath(destination);
            window.grab().save(destination + "/cpp-shared-sender-mixed.png");
        }
        windows[1]->close();
        QTRY_COMPARE_WITH_TIMEOUT(area->subWindowList().size(), 1, 5000);
        QCOMPARE(stack->count(), 1);
        QCOMPARE(stack->currentWidget(), panes[0]->controlsWidget());
        QCOMPARE(draftA->toPlainText(), QString("draft A"));
        window.close();
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
    }
    void allSevenSnapLayoutsAndOccupiedSwap() {
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        auto* tabs = new QTabBar;
        tabs->addTab("A");
        tabs->addTab("B");
        auto* area = new QMdiArea;
        layout->addWidget(tabs);
        layout->addWidget(area);
        QVector<QMdiSubWindow*> windows;
        for (const auto& label : {"A", "B"}) {
            auto* window = area->addSubWindow(new QLabel(label));
            window->setWindowTitle(label);
            window->show();
            windows.push_back(window);
        }
        new SnapLayout(
            area, tabs, [windows](int i) { return i >= 0 && i < windows.size() ? windows[i] : nullptr; },
            &host);
        host.resize(1320, 850);
        host.show();
        QTest::qWait(50);
        auto* chooser = area->findChild<QWidget*>("snapChooser");
        const QVector<QRectF> zones{{0, 0, .5, 1},  {0, 0, 2.0 / 3, 1}, {0, 0, 1.0 / 3, 1}, {0, 0, .5, 1},
                                    {0, 0, .5, .5}, {0, 0, 1.0 / 3, 1}, {0, 0, .25, 1}};
        auto move = [&](QPoint global) {
            QMouseEvent event(QEvent::MouseMove, QPointF(tabs->mapFromGlobal(global)), QPointF(global),
                              Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(tabs, &event);
        };
        auto drop = [&](int tab, int grid, double horizontalFraction) {
            QTest::mousePress(tabs, Qt::LeftButton, Qt::NoModifier, tabs->tabRect(tab).center());
            move(area->viewport()->mapToGlobal(QPoint(area->viewport()->width() / 2, 12)));
            const double width = (chooser->width() - 24.0 - 84) / 7;
            const auto global = chooser->mapToGlobal(
                QPoint(qRound(12 + grid * (width + 14) + width * horizontalFraction), 30));
            move(global);
            QTest::mouseRelease(tabs, Qt::LeftButton, Qt::NoModifier, tabs->mapFromGlobal(global));
        };
        for (int grid = 0; grid < zones.size(); ++grid) {
            const double fraction = grid == 1                ? 1.0 / 3
                                    : grid == 2 || grid == 5 ? 1.0 / 6
                                    : grid == 6              ? .125
                                                             : .25;
            drop(0, grid, fraction);
            QCOMPARE(windows[0]->x(), 4);
            QCOMPARE(windows[0]->y(), 4);
            QCOMPARE(windows[0]->width(), qRound(area->viewport()->width() * zones[grid].width()) - 8);
            QCOMPARE(windows[0]->height(), qRound(area->viewport()->height() * zones[grid].height()) - 8);
        }
        drop(0, 0, .25);
        drop(1, 0, .75);
        const auto left = windows[0]->geometry();
        const auto right = windows[1]->geometry();
        drop(0, 0, .75);
        QCOMPARE(windows[0]->geometry(), right);
        QCOMPARE(windows[1]->geometry(), left);
        host.resize(1440, 900);
        QTRY_COMPARE(windows[1]->width(), qRound(area->viewport()->width() * .5) - 8);
        host.close();
    }
    void editorAndTheme() {
        QWidget host;
        auto editor = makeEditor(&host, true);
        editor->setText("int main() { return 20; }\n");
        QVERIFY(editor->find("return", false, true, true));
        auto before = editor->text();
        editor->setTheme(false);
        editor->setTheme(true);
        QCOMPARE(editor->text(), before);
    }
    void settingsStageApplyAndCancel() {
        SettingsRepository repository;
        ConnectionConfig config;
        config.id = "settings-stage-test";
        config.kind = "test";
        config.name = "Before";
        config.sendIntervalMs = 500;
        config.sendIntervalUnit = 1;
        QString error;
        QVERIFY(repository.saveConnections({config}, error));

        ThemeManager theme;
        {
            SettingsDialog dialog(repository, theme, {}, nullptr);
            auto* name = dialog.findChild<QLineEdit*>("setting_name");
            QVERIFY(name);
            name->setText("Canceled");
            dialog.reject();
        }
        QCOMPARE(repository.loadConnections().first().name, QString("Before"));

        SettingsDialog dialog(repository, theme, {}, nullptr);
        auto* name = dialog.findChild<QLineEdit*>("setting_name");
        QVERIFY(name);
        name->setText("Applied");
        auto* interval = dialog.findChild<QDoubleSpinBox*>("setting_sendIntervalMs");
        QVERIFY(interval);
        QCOMPARE(interval->value(), 0.5);
        interval->setValue(0.25);
        QVERIFY(dialog.apply());
        const auto saved = repository.loadConnections().first();
        QCOMPARE(saved.name, QString("Applied"));
        QCOMPARE(saved.sendIntervalMs, 250);
        QCOMPARE(saved.sendIntervalUnit, 1);
    }
    void workspaceArchiveRoundTrip() {
        const auto dir = testData_.path();
        QSettings source(dir + "/workspace-source.ini", QSettings::IniFormat);
        source.setValue("appearance/dark", false);
        source.setValue("window/geometry", QByteArray("geometry", 8));
        source.setValue("logs/workspaces_v1", "should-not-export");
        source.setValue("logs/workspaces_v2", "should-not-export");
        source.sync();
        ConnectionConfig config;
        config.id = "workspace-profile";
        config.name = "Workspace serial";
        config.port = "COM7";
        QString error;
        const auto path = dir + "/roundtrip.swws";
        QVERIFY2(WorkspaceArchive::write(path, source, {config}, error), qPrintable(error));
        QJsonObject values;
        QJsonArray connections;
        QVERIFY2(WorkspaceArchive::read(path, values, connections, error), qPrintable(error));
        QVERIFY(values.contains("appearance/dark"));
        QVERIFY(values.contains("window/geometry"));
        QVERIFY(!values.contains("logs/workspaces_v1"));
        QVERIFY(!values.contains("logs/workspaces_v2"));
        QJsonParseError legacyError;
        const auto legacyConnections =
            QJsonDocument::fromJson(values.value("connections/json").toString().toUtf8(), &legacyError);
        QCOMPARE(legacyError.error, QJsonParseError::NoError);
        QCOMPARE(legacyConnections.array().first().toObject().value("type").toString(), QString("COM"));
        const auto legacyLayout =
            QJsonDocument::fromJson(values.value("workspace/layout_v1").toString().toUtf8(), &legacyError);
        QCOMPARE(legacyError.error, QJsonParseError::NoError);
        QCOMPARE(legacyLayout.object().value("version").toInt(), 1);
        QCOMPARE(connections.size(), 1);
        QCOMPARE(ConnectionConfig::fromJson(connections.first().toObject()).port, QString("COM7"));
        QSettings restored(dir + "/workspace-restored.ini", QSettings::IniFormat);
        QVERIFY2(WorkspaceArchive::replaceSettings(restored, values, error), qPrintable(error));
        QCOMPARE(restored.value("appearance/dark").toBool(), false);
        QCOMPARE(restored.value("window/geometry").toByteArray(), QByteArray("geometry", 8));

        const QJsonObject pythonConnection{
            {"id", "python-old-id"}, {"name", "Python COM"},   {"type", "COM"},
            {"port", "COM8"},        {"host", "127.0.0.1"},    {"tcp_port", 23},
            {"baudrate", 230400},    {"data_bits", "8"},       {"stop_bits", "1.5"},
            {"parity", "偶"},        {"flow", "RTS/CTS"},      {"buffer_lines", 800},
            {"auto_save", true},     {"save_by_size", true},   {"save_size_kb", 2048},
            {"save_dir", "logs"},    {"input_echo", true},     {"upgrade_mode", false},
            {"newline_mode", true},  {"timestamp_interval", 5}};
        const QJsonObject pythonManifest{
            {"format", "SerialWave.Workspace"},
            {"version", 1},
            {"settings",
             QJsonObject{
                 {"connections/json",
                  QString::fromUtf8(
                      QJsonDocument(QJsonArray{pythonConnection}).toJson(QJsonDocument::Compact))},
                 {"workspace/layout_v1", "{\"version\":1,\"current\":{\"panes\":[],\"area\":[1,1]}}"}}},
            {"include_logs", false},
            {"files", QJsonArray{}},
            {"missing", QJsonArray{}}};
        const auto legacyRaw = QJsonDocument(pythonManifest).toJson(QJsonDocument::Compact);
        mz_zip_archive zip{};
        QVERIFY(mz_zip_writer_init_heap(&zip, 0, 0));
        QVERIFY(mz_zip_writer_add_mem(&zip, "workspace.json", legacyRaw.constData(),
                                      static_cast<size_t>(legacyRaw.size()),
                                      static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION)));
        void* archiveData = nullptr;
        size_t archiveSize = 0;
        QVERIFY(mz_zip_writer_finalize_heap_archive(&zip, &archiveData, &archiveSize));
        mz_zip_writer_end(&zip);
        const auto pythonPath = dir + "/python-style.swws";
        QFile pythonFile(pythonPath);
        QVERIFY(pythonFile.open(QIODevice::WriteOnly));
        QCOMPARE(pythonFile.write(static_cast<const char*>(archiveData), static_cast<qint64>(archiveSize)),
                 static_cast<qint64>(archiveSize));
        pythonFile.close();
        mz_free(archiveData);
        QVERIFY2(WorkspaceArchive::read(pythonPath, values, connections, error), qPrintable(error));
        QCOMPARE(connections.size(), 1);
        const auto imported = ConnectionConfig::fromJson(connections.first().toObject());
        QCOMPARE(imported.kind, QString("serial"));
        QCOMPARE(imported.port, QString("COM8"));
        QCOMPARE(imported.stopBits, 3);
        QCOMPARE(imported.parity, 2);
        QCOMPARE(imported.flow, 1);
        QVERIFY(imported.autoLog);
    }
    void namedLayoutStoreRoundTrip() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings settings(dir.path() + "/layouts.ini", QSettings::IniFormat);
        NamedLayoutSnapshot input;
        input.mainGeometry = QByteArray("main-geometry");
        input.activeConnectionId = "connection-b";
        input.windows = {{"connection-a", QByteArray("window-a"), false},
                         {"connection-b", QByteArray("window-b"), true}};
        QString error;
        QVERIFY2(NamedLayoutStore::save(settings, "two-window", input, error), qPrintable(error));
        const auto loaded = NamedLayoutStore::load(settings, "two-window");
        QVERIFY(loaded.has_value());
        QCOMPARE(loaded->mainGeometry, input.mainGeometry);
        QCOMPARE(loaded->activeConnectionId, input.activeConnectionId);
        QCOMPARE(loaded->windows.size(), 2);
        QCOMPARE(loaded->windows.last().geometry, QByteArray("window-b"));
        QVERIFY(loaded->windows.last().maximized);
        settings.setValue("layouts/saved/legacy", QByteArray("legacy-window-geometry"));
        const auto legacy = NamedLayoutStore::load(settings, "legacy");
        QVERIFY(legacy.has_value());
        QVERIFY(legacy->legacyGeometryOnly);
        QCOMPARE(legacy->mainGeometry, QByteArray("legacy-window-geometry"));
    }
    void terminalKeyboard() {
        TerminalView view;
        view.show();
        QSignalSpy input(&view, &TerminalView::input);
        QTest::keyClick(&view, Qt::Key_Return);
        QCOMPARE(input.takeFirst()[0].toByteArray(), QByteArray("\r"));
        QTest::keyClick(&view, Qt::Key_Up);
        QCOMPARE(input.takeFirst()[0].toByteArray(), QByteArray("\x1b[A"));
        QTest::keyClick(&view, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(input.size(), 0);
    }
    void terminalRendering() {
        TerminalView view;
        view.consume("abc\rX\n\x1b[32mOK\x1b[0m");
        QCOMPARE(view.toPlainText(), QString("Xbc\nOK"));
        auto before = view.document();
        auto text = view.toPlainText();
        view.setTheme(false);
        view.setTheme(true);
        QCOMPARE(view.document(), before);
        QCOMPARE(view.toPlainText(), text);
    }
    void newConnectionUsesTerminalAppearance() {
        QSettings().setValue("appearance/dark/font", "Consolas");
        QSettings().setValue("appearance/dark/size", 14);
        SettingsRepository repository;
        ThemeManager theme;
        theme.apply(true);
        MainWindow window(repository, theme);
        window.show();
        window.addTestConnection();
        auto* pane = window.findChild<TerminalPane*>();
        QVERIFY(pane);
        QTRY_COMPARE(pane->terminal()->font().family(), QString("Consolas"));
        QCOMPARE(pane->terminal()->font().pointSize(), 14);
        QCOMPARE(pane->terminal()->document()->documentMargin(), 16.0);
        window.close();
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
    }
    void terminalTimestampDisplay() {
        TerminalView view;
        view.setTimestampOptions(true, 0);
        view.consume("\x1b[31mone\ntwo\n");
        const auto text = view.toPlainText();
        QVERIFY(text.contains("one"));
        QVERIFY(text.contains("two"));
        const auto first = text.indexOf('[');
        QVERIFY(first >= 0);
        QVERIFY(text.indexOf('[', first + 1) > first);
        QVERIFY(QRegularExpression(R"(\[\d{2}:\d{2}:\d{2}\.\d{3}\] )").match(text).hasMatch());
        for (auto block = view.document()->begin(); block.isValid(); block = block.next()) {
            if (!block.text().startsWith('['))
                continue;
            const auto format = block.begin().fragment().charFormat();
            QVERIFY(!format.hasProperty(QTextFormat::UserProperty + 1));
            QCOMPARE(format.foreground().style(), Qt::NoBrush);
        }
    }
    void plainTextLogSettingsAreStaged() {
        SettingsRepository repository;
        const auto previous = repository.loadConnections();
        QString error;
        const auto cleanup = qScopeGuard([&] { repository.saveConnections(previous, error); });
        ConnectionConfig config;
        config.id = "plain-text-setting";
        config.kind = "tcp";
        QVERIFY(repository.saveConnections({config}, error));
        ThemeManager theme;
        theme.applyTheme("chromatic");
        {
            SettingsDialog settings(repository, theme, {}, nullptr, nullptr, "日志文件");
            auto* plain = settings.findChild<QCheckBox*>("setting_plainTextLog");
            QVERIFY(plain && plain->isChecked());
            plain->setChecked(false);
            settings.reject();
        }
        QVERIFY(repository.loadConnections().first().plainTextLog);
        SettingsDialog settings(repository, theme, {}, nullptr, nullptr, "日志文件");
        auto* plain = settings.findChild<QCheckBox*>("setting_plainTextLog");
        plain->setChecked(false);
        QVERIFY(settings.apply());
        QVERIFY(!repository.loadConnections().first().plainTextLog);
        for (auto* button : settings.findChildren<QPushButton*>())
            if (button->text() == "恢复本页默认")
                button->click();
        QVERIFY(plain->isChecked());
        QVERIFY(!repository.loadConnections().first().plainTextLog);
        QVERIFY(settings.apply());
        QVERIFY(repository.loadConnections().first().plainTextLog);
        const auto preview = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        if (!preview.isEmpty()) {
            QDir().mkpath(preview);
            settings.show();
            QTest::qWait(100);
            settings.grab().save(preview + "/plain-text-log-settings.png");
        }
    }
    void plainTextLogPolicyAndStreaming() {
        QVERIFY(ConnectionConfig{}.plainTextLog);
        QVERIFY(ConnectionConfig::fromJson(QJsonObject{}).plainTextLog);
        QTemporaryDir dir;
        const QByteArray raw = QString("\x1b[31m红色\x1b[0m\r\n\tline\x1b]0;title\a!\n").toUtf8();
        const QByteArray clean = QString("红色\r\n\tline!\n").toUtf8();
        for (const bool plain : {true, false}) {
            ConnectionConfig config;
            config.autoLog = true;
            config.timestampLog = false;
            config.logDirectory = dir.path();
            config.name = plain ? "plain" : "raw";
            config.plainTextLog = plain;
            QCOMPARE(ConnectionConfig::fromJson(config.toJson()).plainTextLog, plain);
            LogWriter writer(config);
            QSignalSpy paths(&writer, &LogWriter::pathChanged);
            QSignalSpy finished(&writer, &LogWriter::finished);
            writer.start();
            for (const char c : raw)
                writer.submit(QByteArray(1, c), 0);
            writer.finish();
            QTRY_COMPARE(finished.size(), 1);
            QCOMPARE(paths.size(), 1);
            QFile file(paths.first().first().toString());
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), plain ? clean : raw);
        }
        ConnectionConfig config;
        config.autoLog = true;
        config.timestampLog = false;
        config.logDirectory = dir.path();
        config.name = "switch-policy";
        LogWriter writer(config);
        QSignalSpy paths(&writer, &LogWriter::pathChanged);
        QSignalSpy finished(&writer, &LogWriter::finished);
        writer.start();
        writer.submit("old\x1b[3", 0);
        config.plainTextLog = false;
        writer.configure(config);
        writer.submit(raw, 0);
        writer.finish();
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(paths.size(), 2);
        QFile first(paths.first().first().toString()), second(paths.last().first().toString());
        QVERIFY(first.open(QIODevice::ReadOnly));
        QVERIFY(second.open(QIODevice::ReadOnly));
        QCOMPARE(first.readAll(), QByteArray("old"));
        QCOMPARE(second.readAll(), raw);
    }
    void testConnectionLifecycle() {
        QTemporaryDir dir;
        ConnectionConfig c;
        c.kind = "test";
        c.name = "test";
        c.autoLog = true;
        c.logDirectory = dir.path();
        Session session(c);
        QSignalSpy connected(&session, &Session::stateChanged);
        QSignalSpy stopped(&session, &Session::stopped);
        session.start();
        QTRY_VERIFY_WITH_TIMEOUT(session.connected(), 5000);
        std::string received;
        QTRY_VERIFY_WITH_TIMEOUT(!(received += session.take()).empty(), 5000);
        QTest::qWait(50);
        received += session.take(16384);
        session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.size(), 1, 5000);
        QVERIFY(session.isStopped());
        QVERIFY(!connected.isEmpty());
        QVERIFY(received.find("256-color output") != std::string::npos);
        QVERIFY(received.find("RGB color output") != std::string::npos);
        QVERIFY(received.find("LOCAL SIMULATION") != std::string::npos);
        QVERIFY(received.find('\x1b') != std::string::npos); // RX remains colored/raw.
    }
    void rotateOnlyLogging() {
        QTemporaryDir directory;
        ConnectionConfig config;
        config.kind = "test";
        config.name = "日志测试";
        config.rotate = true;
        config.autoLog = false;
        config.rotateKiB = 1;
        config.timestampLog = false;
        config.logDirectory = directory.path();
        Session session(config);
        QSignalSpy stopped(&session, &Session::stopped);
        session.start();
        QTest::qWait(200);
        session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.size(), 1, 5000);
        const auto files = QDir(directory.path()).entryList({"*.txt"}, QDir::Files);
        QVERIFY(files.size() > 1);
        for (const auto& name : files) {
            QVERIFY(name.startsWith("日志测试("));
            QFile file(directory.filePath(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            auto bytes = file.readAll();
            QVERIFY(!bytes.isEmpty());
            QVERIFY(!bytes.contains('\x1b')); // Default plain-text policy also applies to rotated logs.
            const auto text = QString::fromUtf8(bytes);
            QVERIFY(text.contains(config.name));
            QVERIFY(!text.contains(QChar::ReplacementCharacter));
        }
    }
    void windowAsyncShutdown() {
        SettingsRepository repository;
        ThemeManager theme;
        theme.apply(true);
        MainWindow window(repository, theme);
        window.show();
        window.addTestConnection();
        QTest::qWait(150);
        window.close();
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
    }
    void tcpLoopback() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QByteArray written;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            auto* peer = server.nextPendingConnection();
            connect(peer, &QTcpSocket::readyRead, peer, [&, peer] {
                auto data = peer->readAll();
                written += data;
                peer->write(data);
            });
        });
        ConnectionConfig c;
        c.kind = "tcp";
        c.tcpPort = server.serverPort();
        Session session(c);
        QSignalSpy stopped(&session, &Session::stopped);
        session.start();
        QTRY_VERIFY_WITH_TIMEOUT(session.connected(), 5000);
        session.send("AT\r");
        std::string received;
        QTRY_VERIFY_WITH_TIMEOUT(!(received += session.take()).empty(), 5000);
        session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.size(), 1, 5000);
        QCOMPARE(written, QByteArray("AT\r"));
        QCOMPARE(received, std::string("AT\r"));
    }
    void upgradeWithSimulatedBoot() {
        QTemporaryDir dir;
        QFile firmware(dir.filePath("firmware.bin"));
        QVERIFY(firmware.open(QIODevice::WriteOnly));
        firmware.write(QByteArray(2400, 'A'));
        firmware.close();
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        FrameParser parser;
        QVector<int> opcodes;
        int progress = 0;
        bool valid = true;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            auto* peer = server.nextPendingConnection();
            connect(peer, &QTcpSocket::readyRead, peer, [&, peer] {
                auto data = peer->readAll();
                auto frames = parser.feed({reinterpret_cast<const std::uint8_t*>(data.constData()),
                                           static_cast<std::size_t>(data.size())});
                for (const auto& f : frames) {
                    opcodes.push_back(f.opcode);
                    if (f.opcode == 1) {
                        valid = valid && f.total == 3 && f.payload.size() == 14;
                        progress = 0;
                    } else if (f.opcode == 2) {
                        valid = valid && f.total == 0 && f.sequence == progress;
                        ++progress;
                    } else if (f.opcode == 3) {
                        valid = valid && f.total == 0 && f.payload.size() == 4;
                    }
                    Frame reply{0,
                                f.sequence,
                                0x81,
                                {static_cast<std::uint8_t>(progress & 255),
                                 static_cast<std::uint8_t>(progress >> 8),
                                 static_cast<std::uint8_t>(f.opcode == 3 ? 1 : 0)}};
                    auto bytes = packFrame(reply);
                    peer->write(reinterpret_cast<const char*>(bytes.data()),
                                static_cast<qint64>(bytes.size()));
                }
            });
        });
        ConnectionConfig c;
        c.kind = "tcp";
        c.upgrade = true;
        c.tcpPort = server.serverPort();
        Session session(c);
        QSignalSpy stopped(&session, &Session::stopped), done(&session, &Session::upgradeFinished);
        session.start();
        QTRY_VERIFY_WITH_TIMEOUT(session.connected(), 5000);
        session.upgrade(firmware.fileName());
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
        session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.size(), 1, 5000);
        QVERIFY(valid);
        QCOMPARE(opcodes, QVector<int>({1, 2, 2, 2, 3}));
        QVERIFY(done.first()[0].toBool());
    }
    void snapshotAndLimit() {
        QTemporaryDir dir;
        QFile file(dir.filePath("utf8.log"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QString("中文日志\nhello\n").toUtf8());
        file.close();
        FileService service;
        QSignalSpy loaded(&service, &FileService::loaded);
        QByteArray received;
        service.open(file.fileName(), [&received](QByteArrayView bytes, QString&) {
            received.append(bytes.data(), bytes.size());
            return true;
        });
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 5000);
        auto first = qvariant_cast<FileResult>(loaded.takeFirst()[0]);
        QCOMPARE(QString::fromUtf8(received), QString("中文日志\nhello\n"));
        QVERIFY(first.error.isEmpty());
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.resize(33 * 1024 * 1024));
        file.close();
        qint64 receivedBytes = 0;
        qsizetype largestChunk = 0;
        service.open(file.fileName(), [&receivedBytes, &largestChunk](QByteArrayView bytes, QString&) {
            receivedBytes += bytes.size();
            largestChunk = qMax(largestChunk, bytes.size());
            return true;
        });
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 5000);
        QVERIFY(qvariant_cast<FileResult>(loaded.first()[0]).error.isEmpty());
        QCOMPARE(receivedBytes, 33LL * 1024 * 1024);
        QVERIFY(largestChunk <= 128 * 1024);
    }
    void logRefreshControlsStayInPlace() {
        ThemeManager theme;
        theme.apply(true);
        LogDocumentView view;
        view.resize(1100, 700);
        view.show();
        view.addExample();
        auto* automatic = view.findChild<QCheckBox*>("logAutoRefresh");
        auto* manual = view.findChild<QPushButton*>("logManualRefresh");
        auto* message = view.findChild<QLabel*>("logReaderMessage");
        QVERIFY(automatic && manual && message);
        QTRY_VERIFY(automatic->isEnabled());
        QCoreApplication::processEvents();
        const auto autoRect = automatic->geometry();
        const auto manualRect = manual->geometry();
        for (const bool dark : {true, false}) {
            theme.apply(dark);
            QCoreApplication::processEvents();
            const auto folder = qEnvironmentVariable("PORTWAVE_PREVIEWS");
            const auto prefix = folder + (dark ? "/footer-dark-" : "/footer-light-");
            if (!folder.isEmpty())
                view.statusBar()->grab().save(prefix + "normal.png");
            QCOMPARE(automatic->geometry(), autoRect);
            QCOMPARE(manual->geometry(), manualRect);
            QTest::mouseClick(automatic, Qt::LeftButton);
            QVERIFY(automatic->isChecked());
            QCOMPARE(automatic->geometry(), autoRect);
            QCOMPARE(manual->geometry(), manualRect);
            QTest::mouseClick(automatic, Qt::LeftButton);
            QVERIFY(!automatic->isChecked());
            QTest::mousePress(manual, Qt::LeftButton);
            if (!folder.isEmpty())
                view.statusBar()->grab().save(prefix + "pressed.png");
            QCOMPARE(manual->geometry(), manualRect);
            QTest::mouseRelease(manual, Qt::LeftButton);
            view.statusBar()->showMessage("后台分块读取日志…");
            QCoreApplication::processEvents();
            if (!folder.isEmpty())
                view.statusBar()->grab().save(prefix + "message.png");
            QVERIFY(automatic->isVisible());
            QVERIFY(manual->isVisible());
            QCOMPARE(automatic->geometry(), autoRect);
            QCOMPARE(manual->geometry(), manualRect);
            QCOMPARE(message->text(), view.statusBar()->currentMessage());
            view.statusBar()->showMessage(QString(1000, QChar('错')));
            QCoreApplication::processEvents();
            QCOMPARE(automatic->geometry(), autoRect);
            QCOMPARE(manual->geometry(), manualRect);
            view.statusBar()->clearMessage();
            QCoreApplication::processEvents();
            QVERIFY(message->text().isEmpty());
            QCOMPARE(automatic->geometry(), autoRect);
            QCOMPARE(manual->geometry(), manualRect);
        }
    }
    void logIncrementalRefreshPreservesExistingDocument() {
        QTemporaryDir dir;
        QFile file(dir.filePath("tail.log"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QString original = QString("old中😀\n") + QString("existing line\n").repeated(150);
        file.write(QByteArray("\x1b[31m") + original.toUtf8());
        file.close();
        LogDocumentView view;
        view.openPath(file.fileName());
        auto* tabs = view.findChild<QTabWidget*>("logDocumentTabs");
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 5000);
        auto* native = qobject_cast<QsciScintilla*>(tabs->currentWidget());
        QVERIFY(native);
        QTest::qWait(80);
        const auto document = native->SendScintillaPtrResult(QsciScintillaBase::SCI_GETDOCPOINTER);
        view.applyMarks({{0, 3, 2, false}}, true);
        native->SendScintilla(QsciScintillaBase::SCI_SETSEL, 6UL, 1L);
        native->verticalScrollBar()->setValue(25);
        const auto scroll = native->verticalScrollBar()->value();
        // An independent sentinel style proves refresh never restyles old bytes.
        native->SendScintilla(QsciScintillaBase::SCI_STARTSTYLING, 0UL);
        native->SendScintilla(QsciScintillaBase::SCI_SETSTYLING, 3UL, 100L);
        auto append = [&](const QByteArray& bytes) {
            if (!file.open(QIODevice::WriteOnly | QIODevice::Append))
                return false;
            const bool ok = file.write(bytes) == bytes.size();
            file.close();
            return ok;
        };
        auto refresh = [&] { view.refreshCurrent(); };
        QVERIFY(append("\x1b[3"));
        refresh();
        QTRY_VERIFY_WITH_TIMEOUT(view.statusBar()->currentMessage().isEmpty(), 5000);
        QCOMPARE(native->text(), original);
        QVERIFY(append(QByteArray("2mnew\xe4\xb8", 7)));
        refresh();
        QTRY_VERIFY_WITH_TIMEOUT(view.statusBar()->currentMessage().isEmpty(), 5000);
        QCOMPARE(native->text(), original + "new");
        QVERIFY(append(QByteArray("\xad\xf0\x9f", 3)));
        refresh();
        QTRY_VERIFY_WITH_TIMEOUT(view.statusBar()->currentMessage().isEmpty(), 5000);
        QCOMPARE(native->text(), original + QString("new中"));
        QVERIFY(append(QByteArray("\x98\x80\r\n", 4)));
        refresh();
        QTRY_VERIFY_WITH_TIMEOUT(view.statusBar()->currentMessage().isEmpty(), 5000);
        const auto expected = original + QString("new中😀\n");
        QCOMPARE(native->text(), expected);
        QCOMPARE(native->SendScintillaPtrResult(QsciScintillaBase::SCI_GETDOCPOINTER), document);
        QCOMPARE(native->SendScintilla(QsciScintillaBase::SCI_GETSTYLEAT, 0UL), 100L);
        QCOMPARE(native->SendScintilla(QsciScintillaBase::SCI_GETANCHOR), 6L);
        QCOMPARE(native->SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS), 1L);
        QCOMPARE(native->verticalScrollBar()->value(), scroll);
        QVERIFY(native->SendScintilla(QsciScintillaBase::SCI_INDICATORVALUEAT, 10UL, 0L));
        QVERIFY(native->SendScintilla(QsciScintillaBase::SCI_GETSTYLEAT,
                                      static_cast<unsigned long>(original.toUtf8().size())) != 0);
        refresh();
        QTRY_VERIFY_WITH_TIMEOUT(view.statusBar()->currentMessage().isEmpty(), 5000);
        QCOMPARE(native->text(), expected); // No duplicate tail on unchanged F5.
        // Changes to old source bytes do not replace the displayed old content.
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.seek(5));
        QCOMPARE(file.write("NEW"), 3);
        file.close();
        QVERIFY(append("last\n"));
        refresh();
        QTRY_VERIFY_WITH_TIMEOUT(view.statusBar()->currentMessage().isEmpty(), 5000);
        QCOMPARE(native->text(), expected + "last\n");
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("replacement\n");
        file.close();
        refresh();
        QTRY_VERIFY_WITH_TIMEOUT(view.statusBar()->currentMessage().contains("截断"), 5000);
        QCOMPARE(native->text(), expected + "last\n");
        const auto preview = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        if (!preview.isEmpty()) {
            QDir().mkpath(preview);
            view.grab().save(preview + "/incremental-log-refresh.png");
        }
        view.close();
    }
    void logTailReadCheckpointsAndBounds() {
        QTemporaryDir dir;
        QFile file(dir.filePath("range.log"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("\xef", 1); // Even a split leading BOM must remain invisible.
        file.close();
        FileService service;
        QSignalSpy loaded(&service, &FileService::loaded);
        QByteArray received;
        auto consume = [&received](QByteArrayView bytes, QString&) {
            received.append(bytes.data(), bytes.size());
            return true;
        };
        service.open(file.fileName(), consume, {}, true);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 5000);
        auto result = qvariant_cast<FileResult>(loaded.takeFirst()[0]);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(received.isEmpty());
#ifdef Q_OS_WIN
        QVERIFY(result.cursor.identity.contains(':')); // Native Windows file ID.
#endif
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Append));
        file.write(QByteArray("\xbb\xbf", 2) + QByteArray(700 * 1024, 'x'));
        file.close();
        while (result.cursor.offset < file.size()) {
            const auto before = result.cursor.offset;
            service.open(file.fileName(), consume, result.cursor, true);
            QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 5000);
            result = qvariant_cast<FileResult>(loaded.takeFirst()[0]);
            QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
            QVERIFY(result.bytesRead <= 256 * 1024);
            QCOMPARE(result.cursor.offset, before + result.bytesRead);
        }
        QCOMPARE(received, QByteArray(700 * 1024, 'x'));
        const auto checkpoint = result.cursor;
        service.open(file.fileName(), consume, checkpoint, true);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 5000);
        QCOMPARE(qvariant_cast<FileResult>(loaded.takeFirst()[0]).bytesRead, 0);
        // Same-sized replacement must be rejected using file identity, not size.
        QVERIFY(file.rename(dir.filePath("old.log")));
        QFile replacement(dir.filePath("range.log"));
        QVERIFY(replacement.open(QIODevice::WriteOnly));
        replacement.write(QByteArray("\xef\xbb\xbf", 3) + QByteArray(700 * 1024, 'x'));
        replacement.close();
        service.open(replacement.fileName(), consume, checkpoint, true);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 5000);
        result = qvariant_cast<FileResult>(loaded.takeFirst()[0]);
        QVERIFY(result.sourceChanged);
        QCOMPARE(result.bytesRead, 0);
        QCOMPARE(received.size(), 700 * 1024);
    }
    void logTailCancellationAndClone() {
        QTemporaryDir dir;
        QFile file(dir.filePath("retry.log"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("old\n");
        file.close();
        LogDocumentView view;
        view.openPath(file.fileName());
        auto* tabs = view.findChild<QTabWidget*>("logDocumentTabs");
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 5000);
        auto* native = qobject_cast<QsciScintilla*>(tabs->currentWidget());
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Append));
        const QByteArray tail = QByteArray("\x1b[32m") + QByteArray(700 * 1024, 'a') + "\n";
        file.write(tail);
        file.close();
        view.refreshCurrent();
        view.cancelLoading();
        QTest::qWait(80);
        QCOMPARE(native->text(), QString("old\n"));
        view.refreshCurrent();
        QTRY_COMPARE_WITH_TIMEOUT(native->text().size(), 4 + 700 * 1024 + 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(view.statusBar()->currentMessage().isEmpty(), 5000);
        LogDocumentView cloned;
        cloned.cloneFrom(view);
        auto* cloneTabs = cloned.findChild<QTabWidget*>("logDocumentTabs");
        auto* copy = qobject_cast<QsciScintilla*>(cloneTabs->currentWidget());
        QCOMPARE(copy->text(), native->text());
        const auto validSize = file.size();
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Append));
        file.write("invalid\xff", 8);
        file.close();
        cloned.refreshCurrent();
        QTRY_VERIFY_WITH_TIMEOUT(cloned.statusBar()->currentMessage().contains("UTF-8"), 5000);
        QCOMPARE(copy->text(), native->text());
        // Repair only the rejected tail; retry starts at the last committed byte.
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.resize(validSize));
        QVERIFY(file.seek(validSize));
        file.write("fixed\n");
        file.close();
        cloned.refreshCurrent();
        QTRY_VERIFY_WITH_TIMEOUT(cloned.statusBar()->currentMessage().isEmpty(), 5000);
        QCOMPARE(copy->text(), native->text() + "fixed\n");
        view.refreshCurrent();
        QTRY_VERIFY_WITH_TIMEOUT(view.statusBar()->currentMessage().isEmpty(), 5000);
        QCOMPARE(copy->text(), native->text()); // Independent cloned checkpoints.
        view.close();
        cloned.close();
    }
    void logReaderAnsiAcrossChunksAndFailedRefresh() {
        QTemporaryDir dir;
        QFile file(dir.filePath("ansi-boundary.log"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray prefix(128 * 1024 - 1, 'x');
        file.write(prefix + QByteArray("\x1b[31m") + QString("中\r\nend").toUtf8());
        file.close();
        LogDocumentView view;
        view.openPath(file.fileName());
        auto* tabs = view.findChild<QTabWidget*>("logDocumentTabs");
        QVERIFY(tabs);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 5000);
        auto* native = qobject_cast<QsciScintilla*>(tabs->currentWidget());
        QVERIFY(native);
        const auto expected = QString::fromLatin1(prefix) + QString("中\nend");
        QCOMPARE(native->text(), expected);
        QVERIFY(native->SendScintilla(QsciScintillaBase::SCI_GETSTYLEAT,
                                      static_cast<unsigned long>(prefix.size())) != 0);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("bad\xf0\x9f", 5);
        file.close();
        view.refreshCurrent();
        QTRY_VERIFY_WITH_TIMEOUT(view.statusBar()->currentMessage().contains("截断"), 5000);
        QCOMPARE(native->text(), expected);
        view.refreshCurrent();
        view.cancelLoading();
        QTest::qWait(100);
        QCOMPARE(native->text(), expected);
    }
    void streamingDocumentAndLargeMode() {
        QWidget parent;
        auto editor = makeEditor(&parent);
        QString error;
        auto buffer = editor->createLoadBuffer(true, error);
        QVERIFY2(buffer, qPrintable(error));
        // The detached loader can receive UTF-8 pieces on the worker, including
        // a multibyte character split between appends. No QWidget on that thread.
        QFutureWatcher<bool> watcher;
        QSignalSpy finished(&watcher, &QFutureWatcher<bool>::finished);
        watcher.setFuture(QtConcurrent::run([buffer] {
            QString failure;
            return buffer->appendUtf8(QByteArrayView("abc\xe4", 4), failure) &&
                   buffer->appendUtf8(QByteArrayView("\xb8\xad\nend", 6), failure);
        }));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QVERIFY(watcher.result());
        QVERIFY2(editor->adoptLoadBuffer(*buffer, error), qPrintable(error));
        QCOMPARE(editor->text(), QString("abc中\nend"));
        auto* native = qobject_cast<QsciScintilla*>(editor->widget());
        QVERIFY(native);
        const auto options = native->SendScintilla(QsciScintillaBase::SCI_GETDOCUMENTOPTIONS);
        QVERIFY(options & QsciScintillaBase::SC_DOCUMENTOPTION_TEXT_LARGE);
        QVERIFY(options & QsciScintillaBase::SC_DOCUMENTOPTION_STYLES_NONE);
        editor->setWrap(true);
        QCOMPARE(native->wrapMode(), QsciScintilla::WrapNone);
        QCOMPARE(native->SendScintilla(QsciScintillaBase::SCI_GETUNDOCOLLECTION), 0L);
        editor->selectRange(3, 1);
        QCOMPARE(editor->selection(), (std::pair<qsizetype, qsizetype>{3, 4}));
        // Reusing a view releases the previous document and restores small-file
        // wrapping; a consumed buffer cannot be adopted twice.
        QVERIFY(!editor->adoptLoadBuffer(*buffer, error));
        auto small = editor->createLoadBuffer(false, error);
        QVERIFY(small);
        QVERIFY(small->appendUtf8(QByteArrayView("new", 3), error));
        QVERIFY(editor->adoptLoadBuffer(*small, error));
        QCOMPARE(editor->text(), QString("new"));
        QCOMPARE(native->wrapMode(), QsciScintilla::WrapWord);
    }
    void streamingFileBoundaryErrorsAndCancel() {
        QTemporaryDir dir;
        QFile file(dir.filePath("boundary.log"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const auto expected = QByteArray(128 * 1024 - 1, 'x') + QString("中\r\n😀").toUtf8();
        QCOMPARE(file.write(expected), expected.size());
        file.close();
        FileService service;
        QSignalSpy loaded(&service, &FileService::loaded);
        QByteArray received;
        auto collect = [&received](QByteArrayView bytes, QString&) {
            received.append(bytes.data(), bytes.size());
            return true;
        };
        service.open(file.fileName(), collect);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 5000);
        auto result = qvariant_cast<FileResult>(loaded.takeFirst()[0]);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(received, expected);
        QCOMPARE(result.lineEnding, QString("Windows (CR LF)"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("\xf0\x9f", 2); // Incomplete final code point must fail.
        file.close();
        service.open(file.fileName(), collect);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 5000);
        QVERIFY(!qvariant_cast<FileResult>(loaded.takeFirst()[0]).error.isEmpty());
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("valid");
        file.close();
        service.open(file.fileName(), [](QByteArrayView, QString& error) {
            error = "simulated loader failure";
            return false;
        });
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 5000);
        QCOMPARE(qvariant_cast<FileResult>(loaded.takeFirst()[0]).error, QString("simulated loader failure"));
        service.open(file.fileName(), [](QByteArrayView, QString&) { return true; });
        service.cancel();
        QTest::qWait(100);
        QVERIFY(loaded.isEmpty());
    }
    void multipleDocuments() {
        QTemporaryDir dir;
        QStringList paths;
        for (int i = 0; i < 2; ++i) {
            auto path = dir.filePath(QString::number(i) + ".log");
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("log\n");
            paths.push_back(path);
        }
        LogWorkspace workspace;
        for (const auto& path : paths)
            workspace.openPath(path);
        auto* tabs = workspace.findChild<QTabWidget*>();
        QVERIFY(tabs);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 5000);
        workspace.openPath(paths.first());
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(tabs->currentIndex(), 0);
        workspace.close();
    }
    void logAnsiColorToggle() {
        QTemporaryDir directory;
        const auto path = directory.filePath("ansi.log");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray::fromHex("1b5b33316d7265641b5b306d20706c61696e0d0a"));
        file.close();

        LogWorkspace workspace;
        workspace.setReadingOptions(true, false, true);
        workspace.openPath(path);
        auto* tabs = workspace.findChild<QTabWidget*>();
        QVERIFY(tabs);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 5000);
        QCOMPARE(workspace.searchSources(true).first().text, QString("red plain\n"));
        workspace.setReadingOptions(true, false, false);
        workspace.setReadingOptions(true, false, true);
        workspace.close();
    }
    void searchServiceBatchesAndErrors() {
        QString error;
        SearchOptions options;
        options.query = "alpha\\nbeta";
        options.mode = 1;
        auto expression = searchExpression(options, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(expression.match("alpha\nbeta").hasMatch());

        options = {};
        options.query = "hit";
        QVector<SearchDocument> sources;
        SearchDocument missing;
        missing.id = "missing";
        missing.path = "Z:/PortWave-test-missing.log";
        missing.fromFile = true;
        sources.push_back(missing);
        for (int i = 0; i < 202; ++i) {
            SearchDocument source;
            source.id = QString::number(i);
            source.path = source.id + ".log";
            source.text = "hit\n";
            sources.push_back(source);
        }
        SearchService service;
        QSignalSpy batches(&service, &SearchService::batchReady);
        service.start(std::move(sources), options);
        QTRY_VERIFY_WITH_TIMEOUT(!batches.isEmpty() && batches.last().first().value<SearchBatch>().finished,
                                 5000);
        const auto result = batches.last().first().value<SearchBatch>();
        QCOMPARE(result.total, 202);
        QVERIFY(!result.errors.isEmpty());
        QVERIFY(result.errors.first().contains("missing"));
    }
    void fluentSettingsTypeAndCancel() {
        SettingsRepository repository;
        const auto previous = repository.loadConnections();
        ConnectionConfig config;
        config.id = "fluent-type";
        config.name = "IMX6U";
        config.port = "COM3";
        QString error;
        QVERIFY(repository.saveConnections({config}, error));
        ThemeManager theme;
        theme.apply(true);
        {
            SettingsDialog dialog(repository, theme, {}, nullptr);
            dialog.show();
            auto* kind = dialog.findChild<QComboBox*>("setting_kind");
            auto* host = dialog.findChild<QLineEdit*>("setting_host");
            auto* baud = dialog.findChild<QComboBox*>("setting_baud");
            QVERIFY(kind && host && baud);
            QVERIFY(!host->isVisible());
            QVERIFY(baud->isVisible());
            auto* choices = dialog.findChild<QWidget*>("connectionTypeChoices");
            auto buttons = choices->findChildren<QPushButton*>();
            QCOMPARE(buttons.size(), 3);
            QTest::mouseClick(buttons[0], Qt::LeftButton);
            QVERIFY(buttons[0]->isChecked());
            QTest::mouseClick(buttons[1], Qt::LeftButton);
            QCOMPARE(kind->currentData().toString(), QString("tcp"));
            QVERIFY(host->isVisible());
            QVERIFY(!baud->isVisible());
            QTest::mouseClick(buttons[2], Qt::LeftButton);
            QVERIFY(!host->isVisible());
            QVERIFY(!baud->isVisible());
            dialog.reject();
        }
        QCOMPARE(repository.loadConnections().first().kind, QString("serial"));
        QVERIFY(repository.saveConnections(previous, error));
    }
    void searchOverlayKeepsAnsiAndMarks() {
        QWidget parent;
        auto editor = makeEditor(&parent);
        editor->setText("中 ERROR and ERROR\n");
        editor->setAnsiStyles({{2, 5, 0xcd0000, -1, false}}, true);
        editor->mark({{0, 1, 2, false}}, true);
        auto* native = qobject_cast<QsciScintilla*>(editor->widget());
        QVERIFY(native);
        const auto style = native->SendScintilla(QsciScintillaBase::SCI_GETSTYLEAT, 4UL);
        editor->highlightMatches({{2, 5, 0, false}});
        editor->highlightMatches({{12, 5, 0, false}}, true);
        QVERIFY(native->SendScintilla(QsciScintillaBase::SCI_INDICATORVALUEAT, 16UL, 4L));
        QVERIFY(native->SendScintilla(QsciScintillaBase::SCI_INDICATORVALUEAT, 16UL, 14L));
        QCOMPARE(native->SendScintilla(QsciScintillaBase::SCI_GETSTYLEAT, 4UL), style);
        QCOMPARE(editor->marks().size(), 1);
        editor->highlightMatches({});
        QCOMPARE(native->SendScintilla(QsciScintillaBase::SCI_INDICATORVALUEAT, 16UL, 4L), 0L);
        QCOMPARE(editor->marks().size(), 1);
    }
    void captureApprovedFluentViews() {
        const auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        if (destination.isEmpty())
            return;
        QDir().mkpath(destination);
        SettingsRepository repository;
        const auto previous = repository.loadConnections();
        QSettings().setValue("sender/sharedHeight", 128);
        ConnectionConfig config;
        config.id = "fluent-preview";
        config.name = "IMX6U";
        config.port = "COM3";
        QString error;
        QVERIFY(repository.saveConnections({config}, error));
        ThemeManager theme;
        theme.apply(true);
        {
            SettingsDialog settings(repository, theme, {}, nullptr, nullptr, "连接配置");
            settings.resize(1060, 690);
            settings.show();
            QTest::qWait(100);
            qInfo() << "Actual UI font"
                    << QFontInfo(settings.findChild<QLabel*>("settingsHeading")->font()).family();
            settings.grab().save(destination + "/settings-dark.png");
            theme.apply(false);
            QTest::qWait(50);
            settings.grab().save(destination + "/settings-light.png");
        }
        theme.apply(true);
        {
            MainWindow main(repository, theme);
            main.resize(1060, 662);
            main.show();
            main.addTestConnection();
            main.addTestConnection();
            QTest::qWait(250);
            auto* terminal = main.findChild<TerminalPane*>()->terminal();
            qInfo() << "Actual terminal fonts" << QFontInfo(terminal->font()).family()
                    << QFontInfo(terminal->document()->defaultFont()).family()
                    << terminal->document()->begin().begin().fragment().charFormat().font().families();
            main.grab().save(destination + "/serial-dark.png");
            main.close();
            QTRY_VERIFY_WITH_TIMEOUT(!main.isVisible(), 5000);
        }
        QTemporaryDir directory;
        QStringList paths;
        for (int fileIndex = 0; fileIndex < 3; ++fileIndex) {
            const auto path = directory.filePath(QString("test1(2026-10-30 22_0%1_09).txt").arg(fileIndex));
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            for (int i = 0; i < 100; ++i)
                file.write(QString("[22:00:09.361] Oct 23 22:00:09 test1 service[%1]: \x1b[%2m[%3] serial "
                                   "receive stress sample; UTF-8 中文; ANSI colors.\x1b[0m\r\n")
                               .arg(53280 + i, 8, 10, QChar('0'))
                               .arg(i % 4 == 0   ? "31"
                                    : i % 4 == 1 ? "32"
                                    : i % 4 == 2 ? "33"
                                                 : "36")
                               .arg(i % 4 == 0   ? "ERROR"
                                    : i % 4 == 1 ? "INFO"
                                    : i % 4 == 2 ? "WARN"
                                                 : "DEBUG")
                               .toUtf8());
            paths.push_back(path);
        }
        {
            QSettings().remove("logs/workspaces_v2");
            LogWorkspace logs;
            logs.registerConnection("preview-1", "test1");
            logs.resize(1060, 662);
            for (const auto& name : {"test2", "test3", "IMX6U"})
                logs.createWorkspace(name);
            for (const auto& path : paths)
                logs.openPath(path);
            logs.show();
            auto* view = logs.activeView();
            auto* tabs = view->findChild<QTabWidget*>("logDocumentTabs");
            QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 3, 5000);
            tabs->setCurrentIndex(1);
            view->search();
            auto* finder = view->findChild<FindDialog*>();
            QVERIFY(finder && finder->isWindow());
            auto options = finder->options();
            options.query = "ERROR";
            finder->setOptions(options);
            QSignalSpy finished(finder, &FindDialog::allSearchFinished);
            finder->searchAll(true);
            QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
            auto* native = qobject_cast<QsciScintilla*>(tabs->currentWidget());
            native->setCursorPosition(25, 62);
            native->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE, 18UL);
            QTest::qWait(200);
            logs.grab().save(destination + "/log-search-dark.png");
            finder->grab().save(destination + "/log-search-floating-dark.png");
            QVERIFY(!finder->findChild<QToolButton*>("findPanelPin"));
            theme.apply(false);
            logs.setTheme(false);
            QTest::qWait(80);
            logs.grab().save(destination + "/log-search-light.png");
            finder->grab().save(destination + "/log-search-floating-light.png");
            logs.close();
        }
        QVERIFY(repository.saveConnections(previous, error));
    }
    void captureReferenceViews() {
        auto destination = qEnvironmentVariable("PORTWAVE_PREVIEWS");
        if (destination.isEmpty())
            return;
        QDir().mkpath(destination);
        SettingsRepository repository;
        ThemeManager theme;
        theme.apply(true);
        MainWindow window(repository, theme);
        window.resize(1280, 820);
        window.show();
        window.addTestConnection();
        window.addTestConnection();
        QTest::qWait(200);
        window.grab().save(destination + "/cpp-dark.png");
        window.showMaximized();
        QTest::qWait(100);
        window.grab().save(destination + "/cpp-main-maximized.png");
        window.showNormal();
        window.resize(1280, 820);
        SettingsDialog connectionSettings(repository, theme, {}, nullptr, nullptr, "连接配置");
        connectionSettings.resize(1000, 880);
        connectionSettings.show();
        QTest::qWait(100);
        connectionSettings.grab().save(destination + "/cpp-settings-connection-dark.png");
        connectionSettings.close();
        SettingsDialog findSettings(repository, theme, {}, nullptr, nullptr, "查找与标记");
        findSettings.resize(1000, 880);
        findSettings.show();
        QTest::qWait(100);
        findSettings.grab().save(destination + "/cpp-settings-find-dark.png");
        findSettings.close();
        theme.apply(false);
        QTest::qWait(100);
        window.grab().save(destination + "/cpp-light.png");
        SettingsDialog appearanceSettings(repository, theme, {}, nullptr, nullptr, "颜色与字体");
        appearanceSettings.resize(1000, 880);
        appearanceSettings.show();
        QTest::qWait(100);
        appearanceSettings.grab().save(destination + "/cpp-settings-appearance-light.png");
        appearanceSettings.close();
        window.close();
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
    }
};
QTEST_MAIN(Integration)
#include "qt_tests.moc"
