#include "features/find_dialog.h"
#include "features/stable_result_delegate.h"
#include "features/window_caption.h"
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSizeGrip>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>
#include <QtConcurrent>
#include <algorithm>

namespace portwave {
namespace {
class Caption : public QWidget {
  public:
    Caption(QDialog* dialog, QWidget* title) : QWidget(dialog), dialog_(dialog) {
        setObjectName("appChrome");
        setAttribute(Qt::WA_StyledBackground);
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(12, 0, 0, 0);
        row->addWidget(title);
        row->addStretch();
        auto* close = new QToolButton;
        close->setObjectName("chromeControl");
        close->setText("×");
        close->setFixedSize(38, 34);
        row->addWidget(close);
        connect(close, &QToolButton::clicked, dialog, &QDialog::close);
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && dialog_->isWindow() && dialog_->windowHandle())
            dialog_->windowHandle()->startSystemMove();
    }

  private:
    QDialog* dialog_;
};
QStringList chooseSearchPaths(QWidget* parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle("选择日志文件");
    dialog.resize(850, 560);
    auto* root = new QVBoxLayout(&dialog);
    auto* filter = new QLineEdit;
    filter->setPlaceholderText("筛选文件名或路径");
    root->addWidget(filter);
    auto* tree = new QTreeWidget;
    tree->setHeaderLabels({"文件", "路径", "大小 / 状态"});
    tree->setColumnWidth(0, 220);
    tree->setColumnWidth(1, 430);
    root->addWidget(tree, 1);
    auto add = [tree](const QStringList& paths) {
        for (const auto& path : paths) {
            auto canonical = QFileInfo(path).absoluteFilePath();
            bool exists = false;
            for (int i = 0; i < tree->topLevelItemCount(); ++i)
                if (tree->topLevelItem(i)
                        ->data(0, Qt::UserRole)
                        .toString()
                        .compare(canonical, Qt::CaseInsensitive) == 0) {
                    exists = true;
                    break;
                }
            if (exists)
                continue;
            QFileInfo info(canonical);
            auto* item = new QTreeWidgetItem(
                tree, {info.fileName(), info.absolutePath(),
                       info.exists() ? QString::number(info.size() / 1024) + " KiB" : "文件不存在"});
            item->setData(0, Qt::UserRole, canonical);
            item->setCheckState(0, info.exists() ? Qt::Checked : Qt::Unchecked);
        }
    };
    add(QSettings().value("logs/recent").toStringList());
    auto* row = new QHBoxLayout;
    root->addLayout(row);
    auto* summary = new QLabel("选择日志文件或添加文件夹");
    root->addWidget(summary);
    auto button = [&](const QString& label, auto callback) {
        auto* b = new QPushButton(label);
        row->addWidget(b);
        QObject::connect(b, &QPushButton::clicked, &dialog, callback);
    };
    button("添加文件…", [&] {
        add(QFileDialog::getOpenFileNames(&dialog, "添加日志", {}, "日志 (*.log *.txt);;所有文件 (*)"));
    });
    button("添加文件夹…", [&] {
        auto path = QFileDialog::getExistingDirectory(&dialog, "添加文件夹");
        if (path.isEmpty())
            return;
        summary->setText("正在后台扫描文件夹…");
        auto* watcher = new QFutureWatcher<QStringList>(&dialog);
        QObject::connect(watcher, &QFutureWatcher<QStringList>::finished, &dialog, [watcher, add, summary] {
            const auto paths = watcher->result();
            add(paths);
            summary->setText(QString("已发现 %1 个候选文件%2")
                                 .arg(paths.size())
                                 .arg(paths.size() == 10000 ? "（已达 10000 个扫描上限）" : ""));
            watcher->deleteLater();
        });
        watcher->setFuture(QtConcurrent::run([path] {
            QStringList files;
            QDirIterator iterator(path, {"*.log", "*.txt"}, QDir::Files, QDirIterator::Subdirectories);
            while (iterator.hasNext() && files.size() < 10000)
                files.push_back(iterator.next());
            return files;
        }));
    });
    button("全选", [tree] {
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            tree->topLevelItem(i)->setCheckState(0, Qt::Checked);
    });
    button("全不选", [tree] {
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            tree->topLevelItem(i)->setCheckState(0, Qt::Unchecked);
    });
    button("重新定位…", [&] {
        auto* item = tree->currentItem();
        if (!item)
            return;
        auto path = QFileDialog::getOpenFileName(&dialog, "重新定位日志");
        if (path.isEmpty())
            return;
        item->setData(0, Qt::UserRole, QFileInfo(path).absoluteFilePath());
        item->setText(0, QFileInfo(path).fileName());
        item->setText(1, QFileInfo(path).absolutePath());
        item->setText(2, QString::number(QFileInfo(path).size() / 1024) + " KiB");
        item->setCheckState(0, Qt::Checked);
    });
    auto update = [tree, summary] {
        int selected = 0;
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (tree->topLevelItem(i)->checkState(0) == Qt::Checked)
                ++selected;
        summary->setText(QString("已选择 %1 / %2 个文件").arg(selected).arg(tree->topLevelItemCount()));
    };
    QObject::connect(tree, &QTreeWidget::itemChanged, &dialog, [update](QTreeWidgetItem*, int) { update(); });
    update();
    QObject::connect(filter, &QLineEdit::textChanged, &dialog, [tree](const QString& text) {
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            auto* item = tree->topLevelItem(i);
            item->setHidden(!item->data(0, Qt::UserRole).toString().contains(text, Qt::CaseInsensitive));
        }
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    root->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QStringList selected;
    if (dialog.exec() == QDialog::Accepted)
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            auto* item = tree->topLevelItem(i);
            if (item->checkState(0) == Qt::Checked)
                selected.push_back(item->data(0, Qt::UserRole).toString());
        }
    return selected;
}
} // namespace
FindDialog::FindDialog(Provider provider, Activator activate, bool cacheMode, QString preferenceKey,
                       QWidget* parent)
    : QDialog(parent), provider_(std::move(provider)), activate_(std::move(activate)),
      preferenceKey_(std::move(preferenceKey)), cacheMode_(cacheMode) {
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
    setModal(false);
    decorateWindow(this);
    setObjectName(cacheMode ? "cacheSearchDialog" : "logFindDialog");
    setWindowTitle(cacheMode ? "查找" : "查找 - 日志浏览");
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(1, 1, 1, 1);
    root->setSpacing(0);
    tabs_ = new QTabBar;
    tabs_->setObjectName("findModeTabs");
    tabs_->addTab("查找");
    tabs_->addTab("标记");
    tabs_->setExpanding(false);
    tabs_->setDrawBase(false);
    auto* caption = new Caption(this, cacheMode ? static_cast<QWidget*>(new QLabel("查找")) : tabs_);
    root->addWidget(caption);
    if (cacheMode)
        tabs_->hide();
    auto* panel = new QWidget;
    auto* body = new QVBoxLayout(panel);
    body->setContentsMargins(14, 12, 14, 14);
    body->setSpacing(9);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(panel);
    root->addWidget(scroll, 1);
    target_ = new QLabel;
    body->addWidget(target_);
    target_->setVisible(cacheMode);
    auto* queryRow = new QHBoxLayout;
    if (!cacheMode) {
        auto* label = new QLabel("查找目标");
        queryRow->addWidget(label);
    }
    query_ = new QComboBox;
    query_->setEditable(true);
    query_->setObjectName("findQuery");
    query_->setInsertPolicy(QComboBox::NoInsert);
    queryRow->addWidget(query_, 1);
    auto* findButton = new QPushButton("查找");
    findButton->setProperty("primary", true);
    if (cacheMode)
        queryRow->addWidget(findButton);
    else
        findButton->hide();
    body->addLayout(queryRow);
    markOptions_ = new QWidget;
    auto* marks = new QGridLayout(markOptions_);
    marks->setContentsMargins(0, 0, 0, 0);
    bookmark_ = new QCheckBox("标记所在行");
    clearPrevious_ = new QCheckBox("清除上次标记");
    selectionOnly_ = new QCheckBox("选取范围内");
    markColor_ = new QComboBox;
    markColor_->setObjectName("markColor");
    markColor_->setIconSize(QSize(30, 20));
    markColor_->addItems({"橙", "蓝", "青", "绿", "红", "紫", "黄", "灰"});
    setMarkBackground({});
    marks->addWidget(bookmark_, 0, 0);
    marks->addWidget(selectionOnly_, 0, 1);
    marks->addWidget(clearPrevious_, 1, 0);
    marks->addWidget(new QLabel("标记颜色"), 1, 1);
    marks->addWidget(markColor_, 2, 1);
    body->addWidget(markOptions_);
    case_ = new QCheckBox("匹配大小写");
    whole_ = new QCheckBox("全词匹配");
    wrap_ = new QCheckBox("循环查找");
    backward_ = new QCheckBox("反向查找");
    dotall_ = new QCheckBox("“.”匹配换行符");
    keep_ = new QCheckBox("保留历史检索结果");
    live_ = new QCheckBox("实时更新");
    wrap_->setChecked(true);
    normal_ = new QRadioButton("普通");
    extended_ = new QRadioButton("扩展");
    regex_ = new QRadioButton("正则表达式");
    normal_->setChecked(true);
    if (cacheMode) {
        auto* options = new QHBoxLayout;
        auto* checks = new QGridLayout;
        const QVector<QCheckBox*> controls{backward_, whole_, case_, wrap_, keep_, live_};
        for (int i = 0; i < controls.size(); ++i)
            checks->addWidget(controls[i], i / 2, i % 2);
        options->addLayout(checks, 3);
        auto* box = new QGroupBox("查找模式");
        auto* grid = new QGridLayout(box);
        grid->addWidget(normal_, 0, 0);
        grid->addWidget(extended_, 0, 1);
        grid->addWidget(regex_, 0, 2);
        grid->addWidget(dotall_, 1, 0, 1, 3);
        options->addWidget(box, 2);
        body->addLayout(options);
    } else {
        auto* options = new QHBoxLayout;
        options->addWidget(case_);
        options->addWidget(whole_);
        options->addWidget(wrap_);
        options->addStretch();
        body->addLayout(options);
        auto* modes = new QHBoxLayout;
        for (auto* radio : {normal_, extended_, regex_})
            modes->addWidget(radio);
        modes->addStretch();
        body->addLayout(modes);
    }
    findActions_ = new QWidget;
    findActions_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* findGrid = new QGridLayout(findActions_);
    findGrid->setContentsMargins(0, 0, 0, 0);
    auto button = [this](QGridLayout* grid, const QString& label, int row, int column, auto callback) {
        auto* control = new QPushButton(label);
        control->setAutoDefault(false);
        grid->addWidget(control, row, column);
        connect(control, &QPushButton::clicked, this, callback);
        return control;
    };
    button(findGrid, "查找下一处", 0, 1, [this] { run(Operation::Next); })->setProperty("primary", true);
    button(findGrid, "查找上一处", 0, 0, [this] { run(Operation::Next, false, true); });
    button(findGrid, "查找当前文件全部", 1, 0, [this] { searchAll(false); });
    button(findGrid, "查找所有已打开文件", 1, 1, [this] { searchAll(true); });
    body->addWidget(findActions_);
    markActions_ = new QWidget;
    markActions_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* markGrid = new QGridLayout(markActions_);
    markGrid->setContentsMargins(0, 0, 0, 0);
    button(markGrid, "全部标记", 0, 0, [this] { run(Operation::Mark); })->setProperty("primary", true);
    button(markGrid, "清除", 0, 1, [this] { emit clearMarksRequested(); });
    button(markGrid, "复制标记的文本", 1, 0, [this] { emit copyMarksRequested(); });
    button(markGrid, "停止标记", 1, 1, [this] {
        service_.cancel();
        status_->setText("已停止标记");
    });
    body->addWidget(markActions_);
    extra_ = new QWidget;
    auto* extraGrid = new QGridLayout(extra_);
    extraGrid->setContentsMargins(0, 0, 0, 0);
    if (!cacheMode) {
        extraGrid->addWidget(backward_, 0, 0);
        extraGrid->addWidget(dotall_, 0, 1);
    }
    button(extraGrid, "计数", 1, 0, [this] { run(Operation::Count); });
    button(extraGrid, "检索指定日志文件…", 1, 1, [this] { chooseFiles(); });
    button(extraGrid, "停止检索", 2, 0, [this] {
        service_.cancel();
        status_->setText("已停止检索");
    });
    transparency_ = new QGroupBox("透明度");
    auto* transparencyGrid = new QGridLayout(transparency_);
    transparent_ = new QCheckBox("启用透明度");
    inactive_ = new QRadioButton("失去焦点后");
    always_ = new QRadioButton("始终");
    inactive_->setChecked(true);
    opacity_ = new QSlider(Qt::Horizontal);
    opacity_->setRange(30, 100);
    opacity_->setValue(75);
    transparencyGrid->addWidget(transparent_, 0, 0, 1, 2);
    transparencyGrid->addWidget(inactive_, 1, 0);
    transparencyGrid->addWidget(always_, 1, 1);
    transparencyGrid->addWidget(opacity_, 2, 0, 1, 2);
    extraGrid->addWidget(transparency_, 3, 0, 1, 2);
    body->addWidget(extra_);
    extra_->hide();
    if (cacheMode) {
        results_ = new QTreeWidget;
        results_->setObjectName("cacheSearchResults");
        results_->setRootIsDecorated(false);
        results_->setUniformRowHeights(true);
        results_->setItemDelegate(new StableResultDelegate(results_));
        results_->setHeaderLabels({"行号", "匹配内容", "状态"});
        results_->setColumnWidth(0, 85);
        results_->setColumnWidth(1, 470);
        results_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        connect(results_, &QTreeWidget::itemSelectionChanged, this, [this] {
            if (!results_->selectedItems().isEmpty()) {
                liveTimer_->stop();
                if (operation_ == Operation::All)
                    service_.cancel();
            }
        });
        body->addWidget(results_, 1);
        connect(results_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int) {
            auto hit = qvariant_cast<SearchHit>(item->data(0, Qt::UserRole));
            if (!activate_(hit)) {
                item->setText(2, "已失效");
                item->setDisabled(true);
            }
        });
        results_->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(results_, &QWidget::customContextMenuRequested, this, [this](const QPoint& point) {
            QMenu menu;
            menu.addAction("复制匹配内容", this, [this] {
                QStringList lines;
                for (auto* item : results_->selectedItems())
                    lines.push_back(item->text(1));
                QApplication::clipboard()->setText(lines.join('\n'));
            });
            auto* markAll = menu.addMenu("标记全部");
            for (int i = 0; i < 8; ++i) {
                const bool dark = qApp->property("portwave.theme.dark").toBool();
                auto* action = markAll->addAction(markSwatch(i, dark, markBackground_),
                                                  markAppearance(i, dark, markBackground_).name);
                connect(action, &QAction::triggered, this, [this, i] {
                    markColor_->setCurrentIndex(i);
                    run(Operation::Mark);
                });
            }
            menu.exec(results_->mapToGlobal(point));
        });
        findActions_->hide();
    }
    auto* footer = new QHBoxLayout;
    status_ = new QLabel("Enter 查找下一处；Shift+Enter 反向查找；Esc 关闭。");
    status_->setProperty("muted", true);
    status_->setWordWrap(true);
    footer->addWidget(status_, 1);
    auto* more = new QToolButton;
    more->setText("更多选项");
    more->setCheckable(true);
    connect(more, &QToolButton::toggled, extra_, &QWidget::setVisible);
    footer->addWidget(more);
    if (!cacheMode)
        body->addStretch(1);
    body->addLayout(footer);
    if (cacheMode) {
        auto* row = new QHBoxLayout;
        auto add = [&](const QString& label, auto callback) {
            auto* control = new QPushButton(label);
            row->addWidget(control);
            connect(control, &QPushButton::clicked, this, callback);
        };
        add("上一处", [this] { run(Operation::Next, false, true); });
        add("下一处", [this] { run(Operation::Next); });
        row->addStretch();
        add("清空结果", [this] {
            hits_.clear();
            results_->clear();
        });
        add("关闭", [this] { close(); });
        body->addLayout(row);
    }
    connect(findButton, &QPushButton::clicked, this, [this] { searchAll(false); });
    connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
        markOptions_->setVisible(index == 1);
        markActions_->setVisible(index == 1);
        findActions_->setVisible(!cacheMode_ && index == 0);
        updateOpacity();
    });
    markOptions_->hide();
    markActions_->hide();
    for (auto* control : {transparent_, static_cast<QCheckBox*>(nullptr)})
        if (control)
            connect(control, &QCheckBox::toggled, this, [this] { updateOpacity(); });
    connect(inactive_, &QRadioButton::toggled, this, [this] { updateOpacity(); });
    connect(always_, &QRadioButton::toggled, this, [this] { updateOpacity(); });
    connect(opacity_, &QSlider::valueChanged, this, [this] { updateOpacity(); });
    connect(&service_, &SearchService::batchReady, this, [this](const SearchBatch& batch) {
        // A repeated live/keep search re-reports targets it already listed. Keep one
        // row per document+line in the visible result list.
        QVector<SearchHit> fresh;
        for (const auto& hit : batch.hits) {
            const auto key = hit.documentId + QLatin1Char('#') + QString::number(hit.line);
            if (shownHits_.contains(key))
                continue;
            shownHits_.insert(key);
            fresh.push_back(hit);
        }
        hits_ += fresh;
        if (operation_ == Operation::All) {
            if (!fresh.isEmpty()) {
                renderHits(fresh, true);
                emit resultsChanged(fresh, query_->currentText(), true);
            }
        }
        if (batch.finished)
            finish(batch);
    });
    connect(&service_, &SearchService::progress, this, [this](int value, int max) {
        status_->setText(QString("正在检索… %1 / %2").arg(value).arg(max));
    });
    liveTimer_ = new QTimer(this);
    liveTimer_->setSingleShot(true);
    liveTimer_->setInterval(300);
    connect(liveTimer_, &QTimer::timeout, this, [this] {
        if (live_->isChecked() && isVisible() && !interactingWithResults())
            searchAll(false);
    });
    connect(query_, &QComboBox::editTextChanged, this, [this] {
        if (live_->isChecked())
            liveTimer_->start();
    });
    connect(query_, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        if (history_.isEmpty())
            return;
        // The last drop-down entry clears every remembered query for this workspace.
        if (index == query_->count() - 1) {
            clearSearchHistory();
            return;
        }
        if (index < 0 || index >= history_.size())
            return;
        query_->setEditText(history_.at(index));
        if (live_->isChecked())
            liveTimer_->start();
    });
    query_->lineEdit()->installEventFilter(this);
    nextShortcut_ = new QShortcut(QKeySequence(Qt::Key_F3), this);
    previousShortcut_ = new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3), this);
    connect(nextShortcut_, &QShortcut::activated, this, [this] { run(Operation::Next, false, false); });
    connect(previousShortcut_, &QShortcut::activated, this, [this] { run(Operation::Next, false, true); });
    auto* gripRow = new QHBoxLayout;
    gripRow->setContentsMargins(0, 0, 0, 0);
    gripRow->addStretch();
    gripRow->addWidget(new QSizeGrip(this));
    root->addLayout(gripRow);
    setMinimumSize(cacheMode ? 690 : 495, 300);
    resize(cacheMode ? 760 : 540, cacheMode ? 530 : 355);
    loadPreferences();
}
SearchOptions FindDialog::options() const {
    return {query_->currentText(), regex_->isChecked() ? 2 : extended_->isChecked() ? 1 : 0,
            case_->isChecked(),    whole_->isChecked(),
            dotall_->isChecked(),  backward_->isChecked(),
            wrap_->isChecked()};
}
void FindDialog::setOptions(const SearchOptions& o) {
    query_->setEditText(o.query);
    normal_->setChecked(o.mode == 0);
    extended_->setChecked(o.mode == 1);
    regex_->setChecked(o.mode == 2);
    case_->setChecked(o.caseSensitive);
    whole_->setChecked(o.wholeWord);
    dotall_->setChecked(o.dotAll);
    backward_->setChecked(o.backward);
    wrap_->setChecked(o.wrap);
}
void FindDialog::setPreferenceKey(const QString& key) {
    if (key == preferenceKey_)
        return;
    savePreferences();
    preferenceKey_ = key;
    loadPreferences();
}
void FindDialog::openFind(const QString& selected) {
    if (!isVisible())
        restoreWindowGeometry();
    tabs_->setCurrentIndex(0);
    auto sources = provider_(false);
    target_->setText(sources.isEmpty() ? "未打开连接" : "查找范围：" + sources.first().path);
    if (!selected.isEmpty() && !selected.contains('\n'))
        query_->setEditText(selected.left(4096));
    show();
    raise();
    if (isWindow())
        activateWindow();
    query_->setFocus();
    query_->lineEdit()->selectAll();
}
void FindDialog::searchAll(bool allOpened) {
    run(Operation::All, allOpened);
}
void FindDialog::run(Operation operation, bool allOpened, bool reverseOverride,
                     QVector<SearchDocument> external) {
    QString error;
    auto opts = options();
    searchExpression(opts, error);
    if (!error.isEmpty()) {
        status_->setText(error);
        return;
    }
    auto sources = external.isEmpty() ? provider_(allOpened) : std::move(external);
    if (sources.isEmpty()) {
        status_->setText("没有可检索的连接或日志文件");
        return;
    }
    source_ = sources.first();
    if (operation == Operation::Mark && selectionOnly_->isChecked() &&
        source_.selectionEnd <= source_.selectionStart) {
        status_->setText("请先在日志正文中选取需要标记的范围。");
        return;
    }
    operation_ = operation;
    searchedDocuments_ = sources.size();
    reverse_ = opts.backward ^ reverseOverride;
    append_ = cacheMode_ ? keep_->isChecked() : logHistoryEnabled_;
    hits_.clear();
    // Only the cache result list survives repeated searches; log searches start a
    // new group, so their duplicate filter must not leak across groups.
    if (!append_ || !cacheMode_)
        shownHits_.clear();
    if (operation == Operation::All) {
        if (results_ && !append_)
            results_->clear();
        emit resultsChanged({}, opts.query, append_);
    }
    savePreferences();
    status_->setText("正在后台检索…");
    service_.start(std::move(sources), opts);
}
void FindDialog::finish(const SearchBatch& batch) {
    if (operation_ == Operation::All && !batch.cancelled)
        emit allSearchFinished(batch.total, searchedDocuments_);
    if (operation_ == Operation::Next && !hits_.isEmpty()) {
        qsizetype anchor = source_.anchor;
        if (reverse_ && source_.selectionEnd > source_.selectionStart)
            anchor = source_.selectionStart;
        int index = -1;
        if (reverse_) {
            for (int i = hits_.size() - 1; i >= 0; --i)
                if (hits_[i].position < anchor) {
                    index = i;
                    break;
                }
            if (index < 0 && wrap_->isChecked())
                index = hits_.size() - 1;
        } else {
            for (int i = 0; i < hits_.size(); ++i)
                if (hits_[i].position >= anchor) {
                    index = i;
                    break;
                }
            if (index < 0 && wrap_->isChecked())
                index = 0;
        }
        if (index >= 0)
            activate_(hits_[index]);
        else {
            status_->setText("已到达查找范围边界");
            return;
        }
    } else if (operation_ == Operation::Mark) {
        QVector<TextMark> marks;
        for (const auto& hit : hits_)
            if (!selectionOnly_->isChecked() ||
                (hit.position >= source_.selectionStart && hit.position + hit.length <= source_.selectionEnd))
                marks.push_back(
                    {hit.position, hit.length, markColor_->currentIndex(), bookmark_->isChecked()});
        emit marksRequested(marks, clearPrevious_->isChecked());
    }
    auto text = QString("找到 %1 处匹配").arg(batch.total);
    if (batch.truncated)
        text += "（已达到 50000 处上限）";
    if (!batch.errors.isEmpty())
        text += "\n" + batch.errors.join('\n');
    status_->setText(text);
}
void FindDialog::renderHits(const QVector<SearchHit>& hits, bool append) {
    if (!results_)
        return;
    if (!append)
        results_->clear();
    for (const auto& hit : hits) {
        auto* item = new QTreeWidgetItem(results_, {QString::number(hit.line + 1), hit.preview, "有效"});
        item->setData(0, Qt::UserRole, QVariant::fromValue(hit));
    }
}
void FindDialog::chooseFiles() {
    auto paths = chooseSearchPaths(this);
    QVector<SearchDocument> sources;
    for (const auto& path : paths) {
        SearchDocument source;
        source.id = path;
        source.path = path;
        source.fromFile = true;
        sources.push_back(source);
    }
    if (!sources.isEmpty())
        run(Operation::All, false, false, std::move(sources));
}
void FindDialog::savePreferences() {
    auto o = options();
    QSettings s;
    s.beginGroup(preferenceKey_);
    s.setValue("query", o.query);
    s.setValue("mode", o.mode);
    s.setValue("case", o.caseSensitive);
    s.setValue("whole", o.wholeWord);
    s.setValue("dotall", o.dotAll);
    s.setValue("backward", o.backward);
    s.setValue("wrap", o.wrap);
    s.setValue("keep", keep_->isChecked());
    s.setValue("live", live_->isChecked());
    saveWindowGeometry();
    const auto previous = history_;
    history_.removeAll(o.query);
    if (!o.query.isEmpty())
        history_.prepend(o.query);
    while (history_.size() > 30)
        history_.removeLast();
    s.setValue("history", history_);
    s.endGroup();
    if (previous != history_)
        rebuildHistoryItems();
}
void FindDialog::loadPreferences() {
    QSettings s;
    s.beginGroup(preferenceKey_);
    SearchOptions o;
    o.query = s.value("query").toString();
    o.mode = s.value("mode", 0).toInt();
    o.caseSensitive = s.value("case", false).toBool();
    o.wholeWord = s.value("whole", false).toBool();
    o.dotAll = s.value("dotall", false).toBool();
    o.backward = s.value("backward", false).toBool();
    o.wrap = s.value("wrap", true).toBool();
    history_ = s.value("history").toStringList();
    setOptions(o);
    rebuildHistoryItems();
    keep_->setChecked(s.value("keep", false).toBool());
    live_->setChecked(s.value("live", false).toBool());
    restoreWindowGeometry();
}
void FindDialog::rebuildHistoryItems() {
    const QString current = query_->currentText();
    const QSignalBlocker blocker(query_);
    query_->clear();
    for (const auto& entry : history_)
        query_->addItem(entry);
    if (!history_.isEmpty()) {
        query_->insertSeparator(query_->count());
        query_->addItem("清除所有搜索历史");
    }
    query_->setCurrentIndex(-1);
    query_->setEditText(current);
}
void FindDialog::clearSearchHistory() {
    history_.clear();
    QSettings s;
    s.beginGroup(preferenceKey_);
    s.remove("history");
    s.endGroup();
    rebuildHistoryItems();
    if (status_)
        status_->setText("已清除搜索历史");
}
void FindDialog::setShortcutOverrides(const QHash<QString, QKeySequence>& overrides) {
    if (nextShortcut_) {
        nextShortcut_->setKey(overrides.value("find_next", QKeySequence(Qt::Key_F3)));
        nextShortcut_->setEnabled(!nextShortcut_->key().isEmpty());
    }
    if (previousShortcut_) {
        previousShortcut_->setKey(overrides.value("find_previous", QKeySequence(Qt::SHIFT | Qt::Key_F3)));
        previousShortcut_->setEnabled(!previousShortcut_->key().isEmpty());
    }
}
bool FindDialog::eventFilter(QObject* object, QEvent* event) {
    if (object == query_->lineEdit() && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            run(tabs_->currentIndex() == 1 ? Operation::Mark : Operation::Next, false,
                key->modifiers().testFlag(Qt::ShiftModifier));
            return true;
        }
    }
    return QDialog::eventFilter(object, event);
}
void FindDialog::closeEvent(QCloseEvent* event) {
    service_.cancel();
    savePreferences();
    event->accept();
}
void FindDialog::reject() {
    service_.cancel();
    savePreferences();
    QDialog::reject();
}
QString FindDialog::geometryKey() const {
    return cacheMode_ ? "search/windows/cache/geometry" : "search/windows/log/geometry";
}
void FindDialog::saveWindowGeometry() {
    if (!restoringGeometry_)
        QSettings().setValue(geometryKey(), saveGeometry());
}
void FindDialog::restoreWindowGeometry() {
    restoringGeometry_ = true;
    const auto geometry = QSettings().value(geometryKey()).toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
    restoringGeometry_ = false;
}
void FindDialog::moveEvent(QMoveEvent* event) {
    QDialog::moveEvent(event);
    if (isVisible())
        saveWindowGeometry();
}
void FindDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    if (isVisible())
        saveWindowGeometry();
}
void FindDialog::setMarkBackground(const QColor& background) {
    markBackground_ = background;
    if (!markColor_)
        return;
    const bool dark = qApp->property("portwave.theme.dark").toBool();
    for (int i = 0; i < markColor_->count(); ++i)
        markColor_->setItemIcon(i, markSwatch(i, dark, background));
}
void FindDialog::changeEvent(QEvent* event) {
    QDialog::changeEvent(event);
    if (event->type() == QEvent::PaletteChange)
        setMarkBackground(markBackground_);
    if (event->type() == QEvent::ActivationChange && transparent_)
        updateOpacity();
}
void FindDialog::updateOpacity() {
    if (!isWindow())
        return;
    setWindowOpacity(tabs_->currentIndex() == 1 && transparent_->isChecked() &&
                             (always_->isChecked() || !isActiveWindow())
                         ? opacity_->value() / 100.0
                         : 1.0);
}
} // namespace portwave
