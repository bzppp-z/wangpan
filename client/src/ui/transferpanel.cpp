#include "ui/transferpanel.h"

#include <QAction>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "transfer/transfermanager.h"
#include "transfer/transferutil.h"
#include "ui/theme.h"

namespace {

// 状态文案与颜色：与《阶段 5 传输面板》界面契约一致。
QString stateText(TransferState state) {
    switch (state) {
        case TransferState::Waiting:
            return QStringLiteral("等待中");
        case TransferState::Running:
            return QStringLiteral("进行中");
        case TransferState::Paused:
            return QStringLiteral("已暂停");
        case TransferState::Completed:
            return QStringLiteral("已完成");
        case TransferState::Failed:
            return QStringLiteral("失败");
        case TransferState::Canceled:
            return QStringLiteral("已取消");
    }
    return QString();
}

QColor stateColor(TransferState state) {
    switch (state) {
        case TransferState::Running:
            return theme::kPrimary;
        case TransferState::Completed:
            return theme::kSuccess;
        case TransferState::Failed:
            return theme::kDanger;
        case TransferState::Paused:
        case TransferState::Canceled:
            return theme::kTextMuted;
        case TransferState::Waiting:
            return theme::kText;
    }
    return theme::kText;
}

QColor barColor(TransferState state) {
    switch (state) {
        case TransferState::Completed:
            return theme::kSuccess;
        case TransferState::Failed:
            return theme::kDanger;
        case TransferState::Paused:
        case TransferState::Canceled:
            return theme::kTextMuted;
        default:
            return theme::kPrimary;
    }
}

int percentOf(const TransferSnapshot& snapshot) {
    if (snapshot.size <= 0) {
        return snapshot.state == TransferState::Completed ? 100 : 0;
    }
    const qint64 done = qBound<qint64>(0, snapshot.done, snapshot.size);
    return static_cast<int>(done * 100 / snapshot.size);
}

bool isTerminal(TransferState state) {
    return state == TransferState::Completed || state == TransferState::Failed ||
           state == TransferState::Canceled;
}

}  // namespace

TransferPanel::TransferPanel(TransferManager* manager, QWidget* parent)
    : QWidget(parent), manager_(manager) {
    setMinimumHeight(140);
    setMaximumHeight(320);
    setStyleSheet(QStringLiteral("TransferPanel { background:#FFFFFF; }"));

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    QFrame* header = new QFrame(this);
    header->setFixedHeight(32);
    header->setStyleSheet(QStringLiteral("QFrame { background:#FFFFFF; border-bottom:1px solid #F2F3F5; }"));
    QHBoxLayout* headLayout = new QHBoxLayout(header);
    headLayout->setContentsMargins(16, 0, 16, 0);
    headLayout->setSpacing(8);

    QLabel* title = new QLabel(QStringLiteral("传输列表"), header);
    title->setStyleSheet(QStringLiteral("color:%1; font-size:13px; font-weight:600;")
                             .arg(theme::kText.name()));
    clearButton_ = new QPushButton(QStringLiteral("清空已完成"), header);
    clearButton_->setFlat(true);
    clearButton_->setCursor(Qt::PointingHandCursor);
    clearButton_->setStyleSheet(
        QStringLiteral("QPushButton { border:none; background:transparent; color:%1; font-size:12px; }"
                       "QPushButton:hover { color:%2; }"
                       "QPushButton:disabled { color:#C9CDD4; }")
            .arg(theme::kPrimary.name())
            .arg(theme::kPrimaryHover.name()));
    connect(clearButton_, &QPushButton::clicked, this, [this]() { manager_->clearCompleted(); });

    headLayout->addWidget(title);
    headLayout->addStretch();
    headLayout->addWidget(clearButton_);
    layout->addWidget(header);

    table_ = new QTableWidget(0, 7, this);
    table_->setHorizontalHeaderLabels(QStringList()
                                      << QStringLiteral("名称") << QStringLiteral("方向")
                                      << QStringLiteral("大小") << QStringLiteral("进度")
                                      << QStringLiteral("速度") << QStringLiteral("剩余")
                                      << QStringLiteral("状态"));
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setTextElideMode(Qt::ElideRight);
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(29);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 7; ++column) {
        table_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Fixed);
    }
    table_->setColumnWidth(1, 64);
    table_->setColumnWidth(2, 90);
    table_->setColumnWidth(3, 120);
    table_->setColumnWidth(4, 90);
    table_->setColumnWidth(5, 80);
    table_->setColumnWidth(6, 80);
    table_->setShowGrid(false);
    table_->setStyleSheet(
        QStringLiteral("QTableWidget { background:#FFFFFF; border:none; font-size:12px; color:%1; }"
                       "QTableWidget::item { padding-left:12px; }"
                       "QTableWidget::item:selected { background:#E8F3FF; color:%1; }"
                       "QHeaderView::section { background:#FAFAFA; color:%2; font-size:11px;"
                       " padding:6px 12px; border:none; border-bottom:1px solid #E5E6EB; }")
            .arg(theme::kText.name())
            .arg(theme::kTextMuted.name()));
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QWidget::customContextMenuRequested, this, &TransferPanel::showContextMenu);

    emptyLabel_ = new QLabel(QStringLiteral("没有传输任务"), this);
    emptyLabel_->setAlignment(Qt::AlignCenter);
    emptyLabel_->setStyleSheet(
        QStringLiteral("color:%1; font-size:13px; background:#FFFFFF;").arg(theme::kTextMuted.name()));

    stack_ = new QStackedWidget(this);
    stack_->addWidget(table_);
    stack_->addWidget(emptyLabel_);
    layout->addWidget(stack_, 1);

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(500);
    connect(refreshTimer_, &QTimer::timeout, this, &TransferPanel::refreshRows);
    refreshTimer_->start();

    connect(manager_, &TransferManager::taskUpdated, this, [this](const QString&) { refreshRows(); });

    refreshRows();
}

void TransferPanel::rebuild() {
    const QVector<TransferSnapshot> snapshots = manager_->snapshots();
    rows_.clear();
    table_->setRowCount(0);  // 连同 cellWidget 一起清掉，避免残留旧进度条
    table_->setRowCount(snapshots.size());
    for (int row = 0; row < snapshots.size(); ++row) {
        const TransferSnapshot& snapshot = snapshots.at(row);
        rows_.insert(snapshot.id, row);
        for (int column = 0; column < 7; ++column) {
            QTableWidgetItem* item = new QTableWidgetItem();
            item->setData(Qt::UserRole, snapshot.id);
            table_->setItem(row, column, item);
        }
        // 进度列用 120 宽的进度条，条内显示百分比。
        QWidget* holder = new QWidget(table_);
        QHBoxLayout* holderLayout = new QHBoxLayout(holder);
        holderLayout->setContentsMargins(12, 8, 12, 7);
        QProgressBar* bar = new QProgressBar(holder);
        bar->setRange(0, 100);
        bar->setTextVisible(true);
        bar->setAlignment(Qt::AlignCenter);
        bar->setFixedHeight(12);
        holderLayout->addWidget(bar);
        table_->setCellWidget(row, 3, holder);
        updateRow(row, snapshot);
    }
    stack_->setCurrentIndex(snapshots.isEmpty() ? 1 : 0);
}

void TransferPanel::refreshRows() {
    const QVector<TransferSnapshot> snapshots = manager_->snapshots();
    bool sameRows = rows_.size() == snapshots.size();
    if (sameRows) {
        for (const TransferSnapshot& snapshot : snapshots) {
            if (!rows_.contains(snapshot.id)) {
                sameRows = false;
                break;
            }
        }
    }
    if (!sameRows) {
        rebuild();
    } else {
        stack_->setCurrentIndex(snapshots.isEmpty() ? 1 : 0);
        for (const TransferSnapshot& snapshot : snapshots) {
            updateRow(rows_.value(snapshot.id), snapshot);
        }
    }

    bool hasTerminal = false;
    for (const TransferSnapshot& snapshot : snapshots) {
        const TransferState state = snapshot.state;
        if (state == TransferState::Completed || state == TransferState::Canceled) {
            hasTerminal = true;
            break;
        }
    }
    clearButton_->setEnabled(hasTerminal);
}

void TransferPanel::updateRow(int row, const TransferSnapshot& snapshot) {
    if (row < 0 || row >= table_->rowCount()) {
        return;
    }
    const int percent = qBound(0, percentOf(snapshot), 100);
    const QString tooltip = !snapshot.error.isEmpty()
                                ? snapshot.error
                                : (snapshot.upload ? snapshot.localPath : snapshot.savePath);

    QTableWidgetItem* nameItem = table_->item(row, 0);
    nameItem->setText(snapshot.name);
    nameItem->setToolTip(tooltip);

    QTableWidgetItem* directionItem = table_->item(row, 1);
    directionItem->setText(snapshot.upload ? QStringLiteral("上传 ↑") : QStringLiteral("下载 ↓"));

    QTableWidgetItem* sizeItem = table_->item(row, 2);
    sizeItem->setText(TransferUtil::formatSize(snapshot.size));

    QTableWidgetItem* speedItem = table_->item(row, 4);
    speedItem->setText(snapshot.state == TransferState::Running && snapshot.speed > 0
                           ? TransferUtil::formatSpeed(snapshot.speed)
                           : QStringLiteral("—"));

    QTableWidgetItem* etaItem = table_->item(row, 5);
    const bool showEta = snapshot.state == TransferState::Running && snapshot.speed > 0 &&
                         snapshot.size > snapshot.done;
    etaItem->setText(showEta
                         ? TransferUtil::formatEta(
                               TransferUtil::etaSeconds(snapshot.size - snapshot.done, snapshot.speed))
                         : QStringLiteral("—"));

    QTableWidgetItem* stateItem = table_->item(row, 6);
    stateItem->setText(stateText(snapshot.state));
    stateItem->setForeground(QBrush(stateColor(snapshot.state)));
    stateItem->setToolTip(tooltip);

    QWidget* holder = table_->cellWidget(row, 3);
    QProgressBar* bar = holder != nullptr ? holder->findChild<QProgressBar*>() : nullptr;
    if (bar != nullptr) {
        bar->setValue(percent);
        bar->setStyleSheet(
            QStringLiteral("QProgressBar { background:%1; border:none; border-radius:6px;"
                           " font-size:10px; color:%2; }"
                           "QProgressBar::chunk { background:%3; border-radius:6px; }")
                .arg(theme::kProgressTrack.name())
                .arg(theme::kText.name())
                .arg(barColor(snapshot.state).name()));
    }
}

void TransferPanel::showContextMenu(const QPoint& pos) {
    const QModelIndex index = table_->indexAt(pos);
    if (!index.isValid()) {
        return;
    }
    QTableWidgetItem* item = table_->item(index.row(), 0);
    if (item == nullptr) {
        return;
    }
    const QString id = item->data(Qt::UserRole).toString();

    TransferSnapshot snapshot;
    bool found = false;
    for (const TransferSnapshot& candidate : manager_->snapshots()) {
        if (candidate.id == id) {
            snapshot = candidate;
            found = true;
            break;
        }
    }
    if (!found) {
        return;
    }

    QMenu menu(this);
    QAction* pauseAction = menu.addAction(QStringLiteral("暂停"));
    QAction* resumeAction = menu.addAction(QStringLiteral("继续"));
    QAction* cancelAction = menu.addAction(QStringLiteral("取消"));
    QAction* retryAction = menu.addAction(QStringLiteral("重试"));
    menu.addSeparator();
    QAction* revealAction = menu.addAction(QStringLiteral("打开所在位置"));

    pauseAction->setEnabled(snapshot.state == TransferState::Running ||
                            (snapshot.state == TransferState::Waiting && !snapshot.restored));
    resumeAction->setEnabled(snapshot.state == TransferState::Paused ||
                             (snapshot.state == TransferState::Waiting && snapshot.restored));
    cancelAction->setEnabled(!isTerminal(snapshot.state));
    retryAction->setEnabled(snapshot.state == TransferState::Failed ||
                            snapshot.state == TransferState::Canceled);
    const QString path = snapshot.upload ? snapshot.localPath : snapshot.savePath;
    revealAction->setEnabled(!path.isEmpty() && QFileInfo(path).exists());

    QAction* chosen = menu.exec(table_->viewport()->mapToGlobal(pos));
    if (chosen == pauseAction) {
        manager_->pause(id);
    } else if (chosen == resumeAction) {
        manager_->resume(id);
    } else if (chosen == cancelAction) {
        manager_->cancel(id);
    } else if (chosen == retryAction) {
        manager_->retry(id);
    } else if (chosen == revealAction) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    }
}
