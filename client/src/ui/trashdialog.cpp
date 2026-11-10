#include "ui/trashdialog.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "apiclient.h"
#include "transfer/transferutil.h"
#include "ui/theme.h"

namespace {

QString formatTime(qint64 seconds) {
    return QDateTime::fromSecsSinceEpoch(seconds).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

// 剩余天数向上取整：不足一天也显示 "1 天后彻底删除"。
int remainingDays(qint64 expiresAt) {
    const qint64 secs = expiresAt - QDateTime::currentSecsSinceEpoch();
    if (secs <= 0) {
        return 0;
    }
    return static_cast<int>((secs + 86399) / 86400);
}

// 彻底删除 / 清空均不可恢复，统一走这里确认；默认按钮为"取消"。
bool confirmPurge(QWidget* parent, const QString& title) {
    QMessageBox box(parent);
    box.setWindowTitle(title);
    box.setIcon(QMessageBox::Warning);
    box.setText(QStringLiteral("彻底删除后不可恢复，确定继续？"));
    QPushButton* okButton = box.addButton(QStringLiteral("确定"), QMessageBox::AcceptRole);
    QPushButton* cancelButton = box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box.setDefaultButton(cancelButton);
    box.exec();
    return box.clickedButton() == okButton;
}

}  // namespace

TrashDialog::TrashDialog(ApiClient* api, QWidget* parent)
    : QDialog(parent),
      api_(api),
      table_(nullptr),
      stack_(nullptr),
      emptyLabel_(nullptr),
      footerHint_(nullptr),
      clearButton_(nullptr),
      restoreButton_(nullptr),
      purgeButton_(nullptr) {
    setWindowTitle(QStringLiteral("回收站"));
    setModal(true);
    resize(720, 480);
    buildUi();

    connect(api_, &ApiClient::trashLoaded, this, &TrashDialog::populate);
    connect(api_, &ApiClient::trashRestored, this, [this](qint64, const QString&, qint64) {
        showFooterMessage(QStringLiteral("已恢复到原位置"), false);
        emit changed();
        reload();
    });
    connect(api_, &ApiClient::trashPurged, this, [this](qint64) {
        showFooterMessage(QStringLiteral("已彻底删除"), false);
        emit changed();
        reload();
    });
    connect(api_, &ApiClient::trashCleared, this, [this]() {
        showFooterMessage(QStringLiteral("回收站已清空"), false);
        emit changed();
        reload();
    });
    connect(api_, &ApiClient::failed, this, [this](const QString& message) {
        showFooterMessage(message, true);
    });

    reload();
}

void TrashDialog::buildUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    QFrame* header = new QFrame(this);
    header->setFixedHeight(48);
    header->setStyleSheet(
        QStringLiteral("QFrame { background:#FFFFFF; border-bottom:1px solid #F2F3F5; }"));
    QHBoxLayout* headLayout = new QHBoxLayout(header);
    headLayout->setContentsMargins(20, 0, 20, 0);
    headLayout->setSpacing(8);

    QLabel* title = new QLabel(QStringLiteral("回收站"), header);
    title->setStyleSheet(
        QStringLiteral("color:%1; font-size:14px; font-weight:600;").arg(theme::kText.name()));
    clearButton_ = new QPushButton(QStringLiteral("清空回收站"), header);
    clearButton_->setFlat(true);
    clearButton_->setCursor(Qt::PointingHandCursor);
    clearButton_->setStyleSheet(
        QStringLiteral("QPushButton { border:none; background:transparent; color:%1; font-size:13px;"
                       " padding:4px 2px; }"
                       "QPushButton:hover { color:%2; }"
                       "QPushButton:disabled { color:#C9CDD4; }")
            .arg(theme::kDanger.name())
            .arg(theme::kErrorText.name()));
    connect(clearButton_, &QPushButton::clicked, this, &TrashDialog::clearAll);

    headLayout->addWidget(title);
    headLayout->addStretch();
    headLayout->addWidget(clearButton_);
    layout->addWidget(header);

    table_ = new QTableWidget(0, 4, this);
    table_->setObjectName(QStringLiteral("trashTable"));
    table_->setHorizontalHeaderLabels(QStringList()
                                      << QStringLiteral("名称") << QStringLiteral("大小")
                                      << QStringLiteral("删除时间") << QStringLiteral("剩余天数"));
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setTextElideMode(Qt::ElideRight);
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(36);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 4; ++column) {
        table_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Fixed);
    }
    table_->setColumnWidth(1, 100);
    table_->setColumnWidth(2, 160);
    // 90 放不下 "30 天后彻底删除"，加宽到 120 避免省略号。
    table_->setColumnWidth(3, 120);
    table_->setShowGrid(false);
    table_->setStyleSheet(
        QStringLiteral("QTableWidget { background:#FFFFFF; border:none; font-size:13px; color:%1; }"
                       "QTableWidget::item { padding-left:12px; }"
                       "QTableWidget::item:selected { background:#E8F3FF; color:%1; }"
                       "QHeaderView::section { background:#FAFAFA; color:%2; font-size:12px;"
                       " padding:8px 12px; border:none; border-bottom:1px solid #E5E6EB; }")
            .arg(theme::kText.name())
            .arg(theme::kTextMuted.name()));
    connect(table_, &QTableWidget::itemSelectionChanged, this, &TrashDialog::updateButtons);
    connect(table_, &QTableWidget::doubleClicked, this, [this](const QModelIndex&) {
        if (!selectedIds().isEmpty()) {
            restoreSelected();
        }
    });

    emptyLabel_ = new QLabel(QStringLiteral("回收站是空的"), this);
    emptyLabel_->setAlignment(Qt::AlignCenter);
    emptyLabel_->setStyleSheet(
        QStringLiteral("color:%1; font-size:13px; background:#FFFFFF;").arg(theme::kTextMuted.name()));

    stack_ = new QStackedWidget(this);
    stack_->addWidget(table_);
    stack_->addWidget(emptyLabel_);
    layout->addWidget(stack_, 1);

    QFrame* footer = new QFrame(this);
    footer->setFixedHeight(56);
    footer->setStyleSheet(
        QStringLiteral("QFrame { background:#FFFFFF; border-top:1px solid #F2F3F5; }"));
    QHBoxLayout* footLayout = new QHBoxLayout(footer);
    footLayout->setContentsMargins(20, 0, 20, 0);
    footLayout->setSpacing(8);

    restoreButton_ = new QPushButton(QStringLiteral("恢复"), footer);
    restoreButton_->setFixedHeight(32);
    restoreButton_->setCursor(Qt::PointingHandCursor);
    restoreButton_->setStyleSheet(
        QStringLiteral("QPushButton { background:%1; border:none; border-radius:4px; color:#FFFFFF;"
                       " font-size:13px; padding:0 18px; }"
                       "QPushButton:hover { background:%2; }"
                       "QPushButton:disabled { background:%3; color:#FFFFFF; }")
            .arg(theme::kPrimary.name())
            .arg(theme::kPrimaryHover.name())
            .arg(theme::kPrimaryDisabled.name()));
    connect(restoreButton_, &QPushButton::clicked, this, &TrashDialog::restoreSelected);

    purgeButton_ = new QPushButton(QStringLiteral("彻底删除"), footer);
    purgeButton_->setFixedHeight(32);
    purgeButton_->setCursor(Qt::PointingHandCursor);
    purgeButton_->setStyleSheet(
        QStringLiteral("QPushButton { background:#FFFFFF; border:1px solid %1; border-radius:4px;"
                       " color:%1; font-size:13px; padding:0 18px; }"
                       "QPushButton:hover { color:%2; border-color:%2; }"
                       "QPushButton:disabled { color:#C9CDD4; border-color:#E5E6EB; }")
            .arg(theme::kDanger.name())
            .arg(theme::kErrorText.name()));
    connect(purgeButton_, &QPushButton::clicked, this, &TrashDialog::purgeSelected);

    footerHint_ = new QLabel(QStringLiteral("双击一行可直接恢复"), footer);
    footerHint_->setStyleSheet(
        QStringLiteral("color:%1; font-size:12px; background:transparent;").arg(theme::kTextMuted.name()));

    footLayout->addWidget(restoreButton_);
    footLayout->addWidget(purgeButton_);
    footLayout->addStretch();
    footLayout->addWidget(footerHint_);
    layout->addWidget(footer);

    updateButtons();
}

void TrashDialog::reload() {
    api_->listTrash();
}

void TrashDialog::populate(const QJsonArray& items) {
    table_->setRowCount(0);
    table_->setRowCount(items.size());
    for (int row = 0; row < items.size(); ++row) {
        const QJsonObject item = items.at(row).toObject();
        const QString kind = item.value(QStringLiteral("kind")).toString();
        const QString name = item.value(QStringLiteral("name")).toString();
        const qint64 id = static_cast<qint64>(item.value(QStringLiteral("id")).toDouble());
        const qint64 size = static_cast<qint64>(item.value(QStringLiteral("size")).toDouble());
        const qint64 deletedAt = static_cast<qint64>(item.value(QStringLiteral("deleted_at")).toDouble());
        const qint64 expiresAt = static_cast<qint64>(item.value(QStringLiteral("expires_at")).toDouble());

        QTableWidgetItem* nameItem = new QTableWidgetItem(
            kind == QStringLiteral("folder") ? QStringLiteral("📁 %1").arg(name) : name);
        nameItem->setData(Qt::UserRole, id);
        nameItem->setToolTip(name);

        QTableWidgetItem* sizeItem = new QTableWidgetItem(
            kind == QStringLiteral("folder") ? QStringLiteral("—") : TransferUtil::formatSize(size));
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

        QTableWidgetItem* timeItem = new QTableWidgetItem(formatTime(deletedAt));
        QTableWidgetItem* remainItem = new QTableWidgetItem(
            QStringLiteral("%1 天后彻底删除").arg(remainingDays(expiresAt)));

        table_->setItem(row, 0, nameItem);
        table_->setItem(row, 1, sizeItem);
        table_->setItem(row, 2, timeItem);
        table_->setItem(row, 3, remainItem);
    }
    stack_->setCurrentIndex(items.isEmpty() ? 1 : 0);
    updateButtons();
}

QList<qint64> TrashDialog::selectedIds() const {
    QList<qint64> ids;
    if (table_ == nullptr || table_->selectionModel() == nullptr) {
        return ids;
    }
    const QModelIndexList rows = table_->selectionModel()->selectedRows();
    for (const QModelIndex& index : rows) {
        QTableWidgetItem* item = table_->item(index.row(), 0);
        if (item != nullptr) {
            ids << item->data(Qt::UserRole).toLongLong();
        }
    }
    return ids;
}

void TrashDialog::updateButtons() {
    const bool hasRows = table_->rowCount() > 0;
    const bool hasSelection = !selectedIds().isEmpty();
    clearButton_->setEnabled(hasRows);
    restoreButton_->setEnabled(hasSelection);
    purgeButton_->setEnabled(hasSelection);
}

void TrashDialog::restoreSelected() {
    const QList<qint64> ids = selectedIds();
    for (qint64 id : ids) {
        api_->restoreTrash(id);
    }
}

void TrashDialog::purgeSelected() {
    const QList<qint64> ids = selectedIds();
    if (ids.isEmpty() || !confirmPurge(this, QStringLiteral("彻底删除"))) {
        return;
    }
    for (qint64 id : ids) {
        api_->purgeTrash(id);
    }
}

void TrashDialog::clearAll() {
    if (table_->rowCount() == 0 || !confirmPurge(this, QStringLiteral("清空回收站"))) {
        return;
    }
    api_->clearTrash();
}

void TrashDialog::showFooterMessage(const QString& message, bool error) {
    footerHint_->setText(message);
    footerHint_->setStyleSheet(
        QStringLiteral("color:%1; font-size:12px; background:transparent;")
            .arg(error ? theme::kErrorText.name() : theme::kSuccess.name()));
    QTimer::singleShot(3000, this, [this]() {
        footerHint_->setText(QStringLiteral("双击一行可直接恢复"));
        footerHint_->setStyleSheet(QStringLiteral("color:%1; font-size:12px; background:transparent;")
                                       .arg(theme::kTextMuted.name()));
    });
}
