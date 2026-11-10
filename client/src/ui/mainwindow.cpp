#include "ui/mainwindow.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QJsonObject>
#include <QLabel>
#include <QLayoutItem>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTableView>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

#include "apiclient.h"
#include "ui/theme.h"

namespace {

// 以 1024 进制格式化，保留一位小数；不足 1 KB 显示整数字节。
QString formatSize(qint64 bytes) {
    const char* const units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    return QStringLiteral("%1 %2").arg(value, 0, 'f', 1).arg(QLatin1String(units[unit]));
}

QString formatTime(qint64 seconds) {
    return QDateTime::fromSecsSinceEpoch(seconds).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

}  // namespace

MainWindow::MainWindow(ApiClient* api, QWidget* parent)
    : QMainWindow(parent),
      api_(api),
      currentId_(0),
      currentItem_(nullptr),
      pendingSelectId_(0),
      tree_(nullptr),
      treeModel_(nullptr),
      table_(nullptr),
      fileModel_(nullptr),
      breadcrumbLayout_(nullptr),
      statusLabel_(nullptr),
      quotaBar_(nullptr) {
    setWindowTitle(QStringLiteral("网盘"));
    resize(1100, 700);
    setMinimumSize(1024, 640);

    buildUi();

    connect(api_, &ApiClient::filesLoaded, this, &MainWindow::onFilesLoaded);
    connect(api_, &ApiClient::folderCreated, this, &MainWindow::onFolderCreated);
    connect(api_, &ApiClient::failed, this, &MainWindow::onFailed);
    connect(api_, &ApiClient::unauthorized, this, &MainWindow::onUnauthorized);
    connect(api_, &ApiClient::loggedOut, this, &MainWindow::signedOut);

    navigateTo(0);
}

void MainWindow::buildUi() {
    QWidget* central = new QWidget(this);
    QVBoxLayout* rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    rootLayout->addWidget(buildToolBar());

    QWidget* filePanel = buildFilePanel();
    QSplitter* splitter = new QSplitter(Qt::Horizontal, central);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(tree_);
    splitter->addWidget(filePanel);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes(QList<int>() << 240 << 860);
    rootLayout->addWidget(splitter, 1);

    setCentralWidget(central);

    statusLabel_ = new QLabel(this);
    statusLabel_->setStyleSheet(QStringLiteral("color:%1; font-size:12px;").arg(theme::kTextMuted.name()));
    quotaBar_ = new QProgressBar(this);
    quotaBar_->setFixedSize(120, 12);
    quotaBar_->setRange(0, 100);
    quotaBar_->setValue(0);
    quotaBar_->setTextVisible(false);
    quotaBar_->setStyleSheet(
        QStringLiteral("QProgressBar { background:#F2F3F5; border:none; border-radius:6px; }"
                       "QProgressBar::chunk { background:%1; border-radius:6px; }")
            .arg(theme::kPrimary.name()));
    statusBar()->addWidget(statusLabel_, 1);
    statusBar()->addPermanentWidget(quotaBar_);
    statusBar()->setStyleSheet(QStringLiteral("QStatusBar { background:#FFFFFF; border-top:1px solid #E5E6EB; }"
                                              "QStatusBar::item { border:none; }"));
    updateQuota(0, 5LL * 1024 * 1024 * 1024);
}

QWidget* MainWindow::buildToolBar() {
    QFrame* bar = new QFrame(this);
    bar->setObjectName(QStringLiteral("toolBar"));
    bar->setFixedHeight(48);
    bar->setStyleSheet(QStringLiteral("QFrame#toolBar { background:#FFFFFF; border-bottom:1px solid #E5E6EB; }"));

    QHBoxLayout* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(16, 0, 16, 0);
    layout->setSpacing(8);

    const QString secondaryStyle =
        QStringLiteral("QPushButton { background:#FFFFFF; border:1px solid %1; border-radius:4px;"
                       " color:%2; font-size:13px; padding:0 14px; }"
                       "QPushButton:hover { border-color:%3; color:%3; }"
                       "QPushButton:disabled { color:#C9CDD4; border-color:#E5E6EB; }")
            .arg(theme::kBorder.name())
            .arg(theme::kText.name())
            .arg(theme::kPrimary.name());

    QPushButton* newFolder = new QPushButton(QStringLiteral("新建文件夹"), bar);
    newFolder->setFixedHeight(32);
    newFolder->setCursor(Qt::PointingHandCursor);
    newFolder->setStyleSheet(
        QStringLiteral("QPushButton { background:%1; border:none; border-radius:4px; color:#FFFFFF;"
                       " font-size:13px; padding:0 16px; }"
                       "QPushButton:hover { background:%2; }"
                       "QPushButton:pressed { background:%3; }")
            .arg(theme::kPrimary.name())
            .arg(theme::kPrimaryHover.name())
            .arg(theme::kPrimaryPressed.name()));
    connect(newFolder, &QPushButton::clicked, this, &MainWindow::createFolder);

    QPushButton* upload = new QPushButton(QStringLiteral("上传"), bar);
    upload->setFixedHeight(32);
    upload->setEnabled(false);
    upload->setToolTip(QStringLiteral("阶段 3 开放"));
    upload->setStyleSheet(secondaryStyle);

    QPushButton* refreshButton = new QPushButton(QStringLiteral("刷新"), bar);
    refreshButton->setFixedHeight(32);
    refreshButton->setCursor(Qt::PointingHandCursor);
    refreshButton->setStyleSheet(secondaryStyle);
    connect(refreshButton, &QPushButton::clicked, this, &MainWindow::refresh);

    QPushButton* logoutButton = new QPushButton(QStringLiteral("退出登录"), bar);
    logoutButton->setFixedHeight(32);
    logoutButton->setCursor(Qt::PointingHandCursor);
    logoutButton->setStyleSheet(secondaryStyle);
    connect(logoutButton, &QPushButton::clicked, this, &MainWindow::logout);

    layout->addWidget(newFolder);
    layout->addWidget(upload);
    layout->addWidget(refreshButton);
    layout->addStretch();
    layout->addWidget(logoutButton);
    return bar;
}

QWidget* MainWindow::buildFilePanel() {
    QWidget* panel = new QWidget(this);
    QVBoxLayout* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // 面包屑
    QFrame* breadcrumb = new QFrame(panel);
    breadcrumb->setFixedHeight(40);
    breadcrumb->setStyleSheet(
        QStringLiteral("QFrame { background:#FFFFFF; border-bottom:1px solid #F2F3F5; }"));
    breadcrumbLayout_ = new QHBoxLayout(breadcrumb);
    breadcrumbLayout_->setContentsMargins(16, 0, 16, 0);
    breadcrumbLayout_->setSpacing(2);
    layout->addWidget(breadcrumb);

    // 文件列表
    fileModel_ = new QStandardItemModel(0, 3, this);
    fileModel_->setHorizontalHeaderLabels(
        QStringList() << QStringLiteral("名称") << QStringLiteral("大小") << QStringLiteral("修改时间"));

    table_ = new QTableView(panel);
    table_->setModel(fileModel_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(32);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    table_->setColumnWidth(1, 120);
    table_->setColumnWidth(2, 160);
    table_->setShowGrid(false);
    table_->setStyleSheet(
        QStringLiteral("QTableView { background:#FFFFFF; border:none; font-size:13px; color:%1; }"
                       "QTableView::item { padding-left:12px; }"
                       "QTableView::item:selected { background:#E8F3FF; color:%1; }"
                       "QHeaderView::section { background:#FAFAFA; color:%2; font-size:12px;"
                       " padding:6px 12px; border:none; border-bottom:1px solid #E5E6EB; }")
            .arg(theme::kText.name())
            .arg(theme::kTextMuted.name()));
    connect(table_, &QTableView::doubleClicked, this, &MainWindow::onRowActivated);
    layout->addWidget(table_, 1);

    // 左目录树
    treeModel_ = new QStandardItemModel(this);
    QStandardItem* rootItem = new QStandardItem(QStringLiteral("我的网盘"));
    rootItem->setEditable(false);
    rootItem->setData(QVariant::fromValue<qlonglong>(0), Qt::UserRole);
    treeModel_->appendRow(rootItem);
    nodes_.insert(0, rootItem);
    currentItem_ = rootItem;

    tree_ = new QTreeView(this);
    tree_->setModel(treeModel_);
    tree_->setHeaderHidden(true);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->setMinimumWidth(180);
    tree_->setStyleSheet(
        QStringLiteral("QTreeView { background:#FAFBFC; border:none; border-right:1px solid #E5E6EB;"
                       " font-size:13px; color:%1; padding:4px; }"
                       "QTreeView::item { height:28px; }"
                       "QTreeView::item:selected { background:#E8F3FF; color:%2; }")
            .arg(theme::kText.name())
            .arg(theme::kPrimary.name()));
    connect(tree_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &MainWindow::onTreeCurrentChanged);
    tree_->expandAll();
    return panel;
}

void MainWindow::navigateTo(qint64 folderId) {
    QStandardItem* node = nodeFor(folderId);
    if (node == nullptr) {
        return;
    }
    currentId_ = folderId;
    currentItem_ = node;

    if (QItemSelectionModel* selection = tree_->selectionModel()) {
        const QSignalBlocker blocker(selection);
        tree_->setCurrentIndex(node->index());
        tree_->scrollTo(node->index());
    }
    rebuildBreadcrumb();
    api_->listFiles(folderId);
}

void MainWindow::rebuildBreadcrumb() {
    while (QLayoutItem* item = breadcrumbLayout_->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }

    QList<QPair<qint64, QString>> crumbs;
    for (QStandardItem* node = currentItem_; node != nullptr; node = node->parent()) {
        crumbs.prepend(qMakePair(node->data(Qt::UserRole).toLongLong(), node->text()));
    }
    for (int i = 0; i < crumbs.size(); ++i) {
        if (i > 0) {
            QLabel* separator = new QLabel(QStringLiteral("/"), this);
            separator->setStyleSheet(
                QStringLiteral("color:%1; font-size:12px;").arg(theme::kTextMuted.name()));
            breadcrumbLayout_->addWidget(separator);
        }
        QPushButton* crumb = new QPushButton(crumbs[i].second, this);
        crumb->setFlat(true);
        crumb->setCursor(Qt::PointingHandCursor);
        const bool isCurrent = (i == crumbs.size() - 1);
        crumb->setStyleSheet(
            QStringLiteral("QPushButton { border:none; background:transparent; font-size:13px;"
                           " color:%1; padding:0 4px; }"
                           "QPushButton:hover { color:%2; }")
                .arg(isCurrent ? theme::kText.name() : theme::kTextMuted.name())
                .arg(theme::kPrimary.name()));
        const qint64 id = crumbs[i].first;
        connect(crumb, &QPushButton::clicked, this, [this, id]() { navigateTo(id); });
        breadcrumbLayout_->addWidget(crumb);
    }
    breadcrumbLayout_->addStretch();
}

void MainWindow::populateTree(qint64 parentId, const QJsonArray& items) {
    QStandardItem* parent = nodeFor(parentId);
    if (parent == nullptr) {
        return;
    }
    // 清掉旧子节点，避免刷新后残留已删除的目录。
    while (parent->rowCount() > 0) {
        QStandardItem* child = parent->takeRow(0).at(0);
        dropSubtree(child);
        delete child;
    }
    for (const QJsonValue& value : items) {
        const QJsonObject item = value.toObject();
        if (item.value(QStringLiteral("kind")).toString() != QStringLiteral("folder")) {
            continue;
        }
        QStandardItem* node = new QStandardItem(item.value(QStringLiteral("name")).toString());
        node->setEditable(false);
        node->setData(QVariant::fromValue<qlonglong>(
                          static_cast<qint64>(item.value(QStringLiteral("id")).toDouble())),
                      Qt::UserRole);
        nodes_.insert(node->data(Qt::UserRole).toLongLong(), node);
        parent->appendRow(node);
    }
    tree_->expand(parent->index());
}

void MainWindow::dropSubtree(QStandardItem* item) {
    if (item == nullptr) {
        return;
    }
    nodes_.remove(item->data(Qt::UserRole).toLongLong());
    for (int i = 0; i < item->rowCount(); ++i) {
        dropSubtree(item->child(i));
    }
}

QStandardItem* MainWindow::nodeFor(qint64 id) const {
    return nodes_.value(id, nullptr);
}

void MainWindow::onFilesLoaded(const QJsonArray& items, qint64 usedBytes, qint64 quotaBytes) {
    fileModel_->removeRows(0, fileModel_->rowCount());
    for (const QJsonValue& value : items) {
        const QJsonObject item = value.toObject();
        const QString kind = item.value(QStringLiteral("kind")).toString();
        const QString name = item.value(QStringLiteral("name")).toString();
        const qint64 size = static_cast<qint64>(item.value(QStringLiteral("size")).toDouble());
        const qint64 updatedAt =
            static_cast<qint64>(item.value(QStringLiteral("updated_at")).toDouble());
        const qint64 id = static_cast<qint64>(item.value(QStringLiteral("id")).toDouble());

        QStandardItem* nameItem =
            new QStandardItem(kind == QStringLiteral("folder")
                                  ? QStringLiteral("📁 %1").arg(name)
                                  : name);
        nameItem->setEditable(false);
        nameItem->setData(QVariant::fromValue<qlonglong>(id), Qt::UserRole);
        nameItem->setData(kind, Qt::UserRole + 1);

        QStandardItem* sizeItem = new QStandardItem(
            kind == QStringLiteral("folder") ? QStringLiteral("—") : formatSize(size));
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        QStandardItem* timeItem = new QStandardItem(formatTime(updatedAt));
        fileModel_->appendRow(QList<QStandardItem*>() << nameItem << sizeItem << timeItem);
    }

    populateTree(currentId_, items);
    updateQuota(usedBytes, quotaBytes);
    if (pendingSelectId_ > 0) {
        selectRowById(pendingSelectId_);
        pendingSelectId_ = 0;
    }
}

void MainWindow::onFolderCreated(qint64 id, const QString& finalName) {
    Q_UNUSED(finalName);
    pendingSelectId_ = id;
    api_->listFiles(currentId_);
}

void MainWindow::selectRowById(qint64 id) {
    for (int row = 0; row < fileModel_->rowCount(); ++row) {
        QStandardItem* item = fileModel_->item(row, 0);
        if (item != nullptr && item->data(Qt::UserRole).toLongLong() == id) {
            table_->selectRow(row);
            table_->scrollTo(fileModel_->index(row, 0));
            return;
        }
    }
}

void MainWindow::onFailed(const QString& message) {
    showStatusMessage(message, true);
}

void MainWindow::onUnauthorized() {
    QMessageBox::warning(this, QStringLiteral("登录已过期"), QStringLiteral("登录已过期，请重新登录"));
    emit signedOut();
}

void MainWindow::onTreeCurrentChanged(const QModelIndex& current, const QModelIndex& previous) {
    Q_UNUSED(previous);
    if (!current.isValid()) {
        return;
    }
    const qint64 id = current.data(Qt::UserRole).toLongLong();
    if (id != currentId_) {
        navigateTo(id);
    }
}

void MainWindow::onRowActivated(const QModelIndex& index) {
    QStandardItem* nameItem = fileModel_->item(index.row(), 0);
    if (nameItem == nullptr) {
        return;
    }
    if (nameItem->data(Qt::UserRole + 1).toString() != QStringLiteral("folder")) {
        return;
    }
    navigateTo(nameItem->data(Qt::UserRole).toLongLong());
}

void MainWindow::refresh() {
    api_->listFiles(currentId_);
}

void MainWindow::createFolder() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("新建文件夹"),
                                               QStringLiteral("文件夹名称"), QLineEdit::Normal,
                                               QString(), &ok)
                             .trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    api_->createFolder(currentId_, name);
}

void MainWindow::logout() {
    api_->logout();
}

void MainWindow::updateQuota(qint64 usedBytes, qint64 quotaBytes) {
    usageText_ = QStringLiteral("已用 %1 / %2").arg(formatSize(usedBytes), formatSize(quotaBytes));
    showStatusMessage(usageText_, false);

    int percent = 0;
    if (quotaBytes > 0) {
        percent = static_cast<int>(usedBytes * 100 / quotaBytes);
    }
    const bool over = percent > 100;
    if (over) {
        percent = 100;
    }
    quotaBar_->setValue(percent);
    quotaBar_->setStyleSheet(
        QStringLiteral("QProgressBar { background:#F2F3F5; border:none; border-radius:6px; }"
                       "QProgressBar::chunk { background:%1; border-radius:6px; }")
            .arg(over ? theme::kErrorText.name() : theme::kPrimary.name()));
}

void MainWindow::showStatusMessage(const QString& message, bool error) {
    if (statusLabel_ == nullptr) {
        return;
    }
    statusLabel_->setText(message);
    statusLabel_->setStyleSheet(
        QStringLiteral("color:%1; font-size:12px;")
            .arg(error ? theme::kErrorText.name() : theme::kTextMuted.name()));
    if (error) {
        QTimer::singleShot(3000, this, [this]() {
            statusLabel_->setText(usageText_);
            statusLabel_->setStyleSheet(
                QStringLiteral("color:%1; font-size:12px;").arg(theme::kTextMuted.name()));
        });
    }
}
