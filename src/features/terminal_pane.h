#pragma once
#include "core/commands.h"
#include "features/find_dialog.h"
#include "features/terminal_view.h"
#include "services/session.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QJsonArray>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QShortcut>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>

namespace portwave {
class TerminalPane final : public QWidget {
    Q_OBJECT
  public:
    explicit TerminalPane(Session* session, QWidget* parent = nullptr);
    ~TerminalPane() override;
    // GUI thread only. Host may reparent this page into a shared stack and must
    // keep it alive until pane/host teardown. The pane destroys its page on
    // close; QPointer tolerates host teardown first. No configuration is reapplied.
    QWidget* controlsWidget() const { return controls_; }
    // GUI-thread status widget. Host may reparent it; pane owns its lifetime.
    QLabel* fileStatusWidget() const { return path_; }
    void setSenderPinned(bool pinned);
    // GUI-thread layout preferences; invalid/missing sizes fall back to 200 px.
    // Stored per connection or shared while pinned, without touching the session.
    int storedSenderHeight(bool shared) const;
    void rememberSenderHeight(int height, bool shared);
    QWidget* dragHeader() const { return header_; }
    // View-only, GUI-thread change; does not connect/disconnect the session.
    void setDetached(bool value);
    // GUI-thread snapshot of the current connection's bottom status text.
    QString connectionStatusText() const;
    TerminalView* terminal() const { return terminal_; }
    Session* session() const { return session_; }
    // GUI thread, transient view state only. Freeze display while dragging;
    // resume from bounded newest bytes, respecting explicit pauseDisplay.
    void setInteractionPaused(bool paused);
    bool interactionPaused() const { return interactionPaused_; }
    // GUI-thread decoration, no session/draft mutation; parent owns outline.
    void setActiveConnection(bool active);
    // GUI-only, current session remains open; logger/file identity is preserved.
    void setReceivePaused(bool paused);
    bool receivePaused() const { return pauseReceiveCheck_->isChecked(); }
    void toggleSender();
    void setTheme(bool dark);
    void find();
    void reloadFindPreferences() {
        if (finder_)
            finder_->reloadPreferences();
    }
    void pauseDisplay(bool value) { pausedDisplay_ = value; }
    // GUI-only shortcut rebinding; empty disables it. Provided by the UI layer so
    // features never read the shortcut profile directly.
    void setSendShortcut(const QKeySequence& sequence);
    // GUI-only: find shortcuts for this pane's search window, supplied by the UI layer.
    void setFindShortcuts(const QHash<QString, QKeySequence>& overrides);
    void setWrap(bool value) {
        terminal_->setLineWrapMode(value ? QTextEdit::WidgetWidth : QTextEdit::NoWrap);
    }
    void shutdown();
    void setTargets(const QVector<QPair<QString, QString>>& targets);
    void showDisplayOptions(const QPoint& position);
    ConnectionConfig currentConfiguration() const;
    void applyConfiguration(const ConnectionConfig& config);
    bool automaticSending() const { return automatic_->isChecked(); }
    void setAutomaticSending(bool value) { automatic_->setChecked(value); }
    bool senderVisible() const { return !sender_->isHidden(); }
    void setTerminalAppearance(const QFont& font, const QColor& background, const QColor& foreground,
                               bool smooth);
    QJsonArray commands() const { return commands_; }
    QString commandGroupName() const { return activeCommandGroup_; }
    void setCommandGroup(const QString& name);
    void setCommands(const QJsonArray& commands);
    void reloadCommandGroup(const QString& name);
    void manageCommand(int index) { editShortcut(index); }
    void addCommand() { editShortcut(-1); }
    void importCommandGroup() { importCommands(); }
    void exportCommandGroup() { exportCommands(); }
    void manageCommandGroupFromSettings(const QString& operation) { manageCommandGroup(operation); }
  signals:
    void status(const QString& message);
    void connectionStatusChanged(const QString& message);
    void openLog(const QString& path);
    void targetRequested(const QString& id);
    void commandsChanged(const QString& group);
    void dockRequested();
    void senderPinChanged(bool pinned);
    void senderVisibilityChanged(bool visible);
    void receivePauseChanged(bool paused);

  protected:
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* object, QEvent* event) override;

  private:
    void openLogFile(LogOpenAction action);
    void send();
    // GUI thread only. Inline hint anchored to the send row; it never touches the
    // draft, the session or the send history, and hides itself after two seconds.
    void showSendHint(const QString& text);
    void positionSendHint();
    void executeShortcut(const QString& command, bool hex, bool appendCR = false, bool appendLF = false);
    void nextStep();
    void rebuildShortcuts();
    void editShortcut(int index);
    void importCommands();
    void exportCommands();
    void saveCommandGroup();
    void manageCommandGroup(const QString& operation);
    void refreshStatus();
    Session* session_;
    TerminalView* terminal_;
    QWidget *sender_, *shortcuts_;
    QPointer<QWidget> controls_;
    QWidget* header_;
    QLabel *title_, *stateDot_;
    QString connectionStatusText_;
    QPlainTextEdit* draft_;
    QComboBox *mode_, *group_, *target_, *unit_;
    QCheckBox *hex_, *cr_, *lf_, *currentLine_, *advance_, *automatic_;
    QDoubleSpinBox* interval_;
    QWidget* displayOptions_;
    QPointer<QLabel> path_;
    QToolButton* senderPin_{};
    QProgressBar* progress_;
    QTimer refresh_, autoSend_, stepTimer_;
    QJsonArray commands_;
    QString activeCommandGroup_ = "默认";
    QStringList sendHistory_;
    std::vector<CommandStep> steps_;
    std::size_t step_{};
    QString logPath_, firmwarePath_, stateMessage_;
    bool pausedDisplay_{}, interactionPaused_{}, catchUpLatest_{};
    QWidget* activeOutline_{};
    bool applying_{};
    QCheckBox *pauseReceiveCheck_{}, *pauseDisplayCheck_{}, *wrapCheck_{};
    QTimer saveTimer_;
    QLabel* sendHint_{};
    QTimer sendHintTimer_;
    FindDialog* finder_{};
    QShortcut* sendShortcut_{};
    bool everConnected_{};
    QHash<QString, QKeySequence> findShortcutOverrides_;
};
} // namespace portwave
