#include "ui/mainwindow.h"

#include <QAbstractItemView>
#include <QAction>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QJsonObject>
#include <QLabel>
#include <QLayoutItem>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPoint>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStorageInfo>
#include <QTableView>
#include <QTimer>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

#include "apiclient.h"
#include "transfer/transfermanager.h"
#include "ui/previewpanel.h"
#include "ui/theme.h"
#include "ui/trashdialog.h"
#include "ui/transferpanel.h"

namespace {

// 底部 32 高摘要行：整行可点击，回调由主窗口注入。
class TransferBar : public QFrame {
public:
    explicit TransferBar(QWidget* parent = nullptr) : QFrame(parent) {}

    std::function<void()> onClick;

protected:
    void mousePressEvent(QMouseEvent* event) override {
        QFrame::mousePressEvent(event);
        if (onClick) {
            onClick();
        }
    }
};

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
      transfers_(nullptr),
      transferPanel_(nullptr),
      transferBar_(nullptr),
      transferSummaryLabel_(nullptr),
      transferArrow_(nullptr),
      transferExpanded_(false),
      previewPanel_(nullptr),
      splitter_(nullptr),
      vSplitter_(nullptr),
      previewLayoutBucket_(-1),
      currentId_(0),
      currentItem_(nullptr),
      pendingSelectId_(0),
      tree_(nullptr),
      treeModel_(nullptr),
      table_(nullptr),
      fileModel_(nullptr),
      breadcrumbLayout_(nullptr),
      statusLabel_(nullptr),
      quotaBar_(nullptr),
      downloadButton_(nullptr),
      transientStatus_(false),
      statusSeq_(0) {
    setWindowTitle(QStringLiteral("网盘"));
    resize(1100, 700);
    setMinimumSize(1024, 640);
    setAcceptDrops(true);

    transfers_ = new TransferManager(api_, this);
    buildUi();

    connect(transfers_, &TransferManager::summaryChanged, this, &MainWindow::onTransferSummary);
    connect(transfers_, &TransferManager::uploadCompleted, this, &MainWindow::onUploadCompleted);
    connect(transfers_, &TransferManager::downloadCompleted, this, &MainWindow::onDownloadCompleted);
    connect(transfers_, &TransferManager::failed, this, &MainWindow::onFailed);

    connect(api_, &ApiClient::filesLoaded, this, &MainWindow::onFilesLoaded);
    connect(api_, &ApiClient::folderCreated, this, &MainWindow::onFolderCreated);
    connect(api_, &ApiClient::renamed, this, &MainWindow::onRenamed);
    connect(api_, &ApiClient::deleted, this, &MainWindow::onDeleted);
    connect(api_, &ApiClient::failed, this, &MainWindow::onFailed);
    connect(api_, &ApiClient::unauthorized, this, &MainWindow::onUnauthorized);
    connect(api_, &ApiClient::loggedOut, this, &MainWindow::signedOut);

    // 上次未完成的传输：恢复为等待中，由用户点"继续"再开始。
    transfers_->restorePersisted();

    navigateTo(0);
}

void MainWindow::buildUi() {
    QWidget* central = new QWidget(this);
    QVBoxLayout* rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    rootLayout->addWidget(buildToolBar());

    QWidget* filePanel = buildFilePanel();
    splitter_ = new QSplitter(Qt::Horizontal, central);
    splitter_->setChildrenCollapsible(false);
    splitter_->addWidget(tree_);
    splitter_->addWidget(filePanel);
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);

    // 第三栏预览面板默认隐藏，双击文件 / 右键"预览"时才展开。
    previewPanel_ = new PreviewPanel(api_, splitter_);
    previewPanel_->setMinimumWidth(280);
    previewPanel_->hide();
    splitter_->addWidget(previewPanel_);
    splitter_->setStretchFactor(2, 0);
    splitter_->setSizes(QList<int>() << 240 << 860 << 0);
    connect(previewPanel_, &PreviewPanel::closeRequested, this, &MainWindow::onPreviewClosed);
    connect(previewPanel_, &PreviewPanel::downloadRequested, this,
            &MainWindow::onPreviewDownloadRequested);
    connect(previewPanel_, &PreviewPanel::fileMissing, this, &MainWindow::refresh);

    // 文件区与传输面板上下分栏：面板默认隐藏，展开时用分隔条调整高度（140–320）。
    vSplitter_ = new QSplitter(Qt::Vertical, central);
    vSplitter_->setChildrenCollapsible(false);
    vSplitter_->addWidget(splitter_);
    transferPanel_ = new TransferPanel(transfers_, vSplitter_);
    vSplitter_->addWidget(transferPanel_);
    transferPanel_->hide();  // 收起态只保留底部 32 高摘要行
    vSplitter_->setStretchFactor(0, 1);
    vSplitter_->setStretchFactor(1, 0);
    rootLayout->addWidget(vSplitter_, 1);
    rootLayout->addWidget(buildTransferBar());

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

QWidget* MainWindow::buildTransferBar() {
    TransferBar* bar = new TransferBar(this);
    bar->setObjectName(QStringLiteral("transferBar"));
    bar->setFixedHeight(32);
    bar->setCursor(Qt::PointingHandCursor);
    bar->setStyleSheet(QStringLiteral("QFrame#transferBar { background:#FAFBFC;"
                                      " border-top:1px solid #E5E6EB; }"));
    QHBoxLayout* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(16, 0, 16, 0);
    layout->setSpacing(8);

    transferSummaryLabel_ = new QLabel(bar);
    transferSummaryLabel_->setStyleSheet(
        QStringLiteral("color:%1; font-size:12px;").arg(theme::kText.name()));
    transferArrow_ = new QLabel(QStringLiteral("▴"), bar);
    transferArrow_->setStyleSheet(
        QStringLiteral("color:%1; font-size:12px;").arg(theme::kTextMuted.name()));

    layout->addWidget(transferSummaryLabel_);
    layout->addStretch();
    layout->addWidget(transferArrow_);
    bar->onClick = [this]() { setTransferExpanded(!transferExpanded_); };
    bar->hide();
    transferBar_ = bar;
    return bar;
}

void MainWindow::onTransferSummary(const QString& text, bool anyActive) {
    Q_UNUSED(anyActive);
    if (transferBar_ == nullptr) {
        return;
    }
    if (text.isEmpty()) {
        transferBar_->hide();
        setTransferExpanded(false);
        return;
    }
    transferSummaryLabel_->setText(text);
    transferBar_->show();
    transferArrow_->setText(transferExpanded_ ? QStringLiteral("▾") : QStringLiteral("▴"));
}

void MainWindow::setTransferExpanded(bool expanded) {
    if (transferPanel_ == nullptr || vSplitter_ == nullptr) {
        return;
    }
    transferExpanded_ = expanded && !transfers_->isEmpty();
    if (transferExpanded_) {
        transferPanel_->show();
        const int total = vSplitter_->height();
        const int panelHeight = qBound(140, 180, 320);
        vSplitter_->setSizes(QList<int>() << qMax(320, total - panelHeight) << panelHeight);
    } else {
        transferPanel_->hide();
    }
    if (transferArrow_ != nullptr) {
        transferArrow_->setText(transferExpanded_ ? QStringLiteral("▾") : QStringLiteral("▴"));
    }
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

    QPushButton* upload = new QPushButton(QStringLiteral("上传 ▾"), bar);
    upload->setFixedHeight(32);
    upload->setCursor(Qt::PointingHandCursor);
    upload->setStyleSheet(secondaryStyle);
    connect(upload, &QPushButton::clicked, this, [this, upload]() {
        QMenu menu(this);
        QAction* filesAction = menu.addAction(QStringLiteral("上传文件"));
        QAction* folderAction = menu.addAction(QStringLiteral("上传文件夹"));
        QAction* chosen = menu.exec(upload->mapToGlobal(QPoint(0, upload->height() + 4)));
        if (chosen == filesAction) {
            uploadFiles();
        } else if (chosen == folderAction) {
            uploadFolder();
        }
    });

    downloadButton_ = new QPushButton(QStringLiteral("下载"), bar);
    downloadButton_->setFixedHeight(32);
    downloadButton_->setCursor(Qt::PointingHandCursor);
    downloadButton_->setStyleSheet(secondaryStyle);
    downloadButton_->setEnabled(false);
    downloadButton_->setToolTip(QStringLiteral("下载选中的文件"));
    connect(downloadButton_, &QPushButton::clicked, this, &MainWindow::startDownload);

    QPushButton* trashButton = new QPushButton(QStringLiteral("回收站"), bar);
    trashButton->setFixedHeight(32);
    trashButton->setCursor(Qt::PointingHandCursor);
    trashButton->setStyleSheet(secondaryStyle);
    connect(trashButton, &QPushButton::clicked, this, &MainWindow::openTrash);

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
    layout->addWidget(downloadButton_);
    layout->addWidget(trashButton);
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
    table_->setObjectName(QStringLiteral("fileTable"));
    table_->setModel(fileModel_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
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
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &MainWindow::updateDownloadAction);

    // F2 仅单选可用；Delete 支持多选，逐个软删除。
    QShortcut* renameShortcut = new QShortcut(QKeySequence(Qt::Key_F2), table_);
    renameShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(renameShortcut, &QShortcut::activated, this, &MainWindow::renameSelected);
    QShortcut* deleteShortcut = new QShortcut(QKeySequence::Delete, table_);
    deleteShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(deleteShortcut, &QShortcut::activated, this, &MainWindow::deleteSelected);

    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex index = table_->indexAt(pos);
        if (!index.isValid()) {
            return;
        }
        table_->selectRow(index.row());
        QStandardItem* nameItem = fileModel_->item(index.row(), 0);
        if (nameItem == nullptr) {
            return;
        }
        const bool isFile = nameItem->data(Qt::UserRole + 1).toString() == QStringLiteral("file");

        // 顺序固定：预览、下载、重命名、删除、回收站；文件夹行不出现前两项。
        QMenu menu(this);
        QAction* previewAction = menu.addAction(QStringLiteral("预览"));
        QAction* downloadAction = menu.addAction(QStringLiteral("下载"));
        menu.addSeparator();
        QAction* renameAction = menu.addAction(QStringLiteral("重命名"));
        QAction* deleteAction = menu.addAction(QStringLiteral("删除"));
        menu.addSeparator();
        QAction* trashAction = menu.addAction(QStringLiteral("回收站"));
        previewAction->setVisible(isFile);
        downloadAction->setVisible(isFile);

        QAction* chosen = menu.exec(table_->viewport()->mapToGlobal(pos));
        if (chosen == previewAction && isFile) {
            openPreview(nameItem);
        } else if (chosen == downloadAction && isFile) {
            startDownload();
        } else if (chosen == renameAction) {
            renameSelected();
        } else if (chosen == deleteAction) {
            deleteSelected();
        } else if (chosen == trashAction) {
            openTrash();
        }
    });
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
        nameItem->setData(QVariant::fromValue<qlonglong>(size), Qt::UserRole + 2);
        nameItem->setData(name, Qt::UserRole + 3);  // 原始名称，文件夹展示时带 📁 前缀

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

QStandardItem* MainWindow::singleSelectedItem() const {
    if (table_ == nullptr || table_->selectionModel() == nullptr) {
        return nullptr;
    }
    const QModelIndexList rows = table_->selectionModel()->selectedRows();
    if (rows.size() != 1) {
        return nullptr;
    }
    return fileModel_->item(rows.first().row(), 0);
}

bool MainWindow::isAncestorOfCurrent(qint64 id) const {
    for (QStandardItem* node = currentItem_; node != nullptr; node = node->parent()) {
        if (node->data(Qt::UserRole).toLongLong() == id) {
            return true;
        }
    }
    return false;
}

void MainWindow::renameSelected() {
    QStandardItem* item = singleSelectedItem();
    if (item == nullptr) {
        return;
    }
    const QString currentName = item->data(Qt::UserRole + 3).toString();

    QInputDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("重命名"));
    dialog.setLabelText(QStringLiteral("新名称"));
    dialog.setTextValue(currentName);
    if (QLineEdit* edit = dialog.findChild<QLineEdit*>()) {
        edit->selectAll();
    }
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const QString name = dialog.textValue().trimmed();
    if (name.isEmpty() || name == currentName) {
        return;  // 空名或未变化不发请求
    }
    api_->renameFile(item->data(Qt::UserRole).toLongLong(), name);
}

void MainWindow::deleteSelected() {
    if (table_ == nullptr || table_->selectionModel() == nullptr) {
        return;
    }
    const QModelIndexList rows = table_->selectionModel()->selectedRows();
    for (const QModelIndex& index : rows) {
        QStandardItem* item = fileModel_->item(index.row(), 0);
        if (item != nullptr) {
            api_->deleteFile(item->data(Qt::UserRole).toLongLong());
        }
    }
}

void MainWindow::onRenamed(qint64 id, const QString& finalName) {
    pendingSelectId_ = id;
    showStatusMessage(QStringLiteral("已重命名为 %1").arg(finalName), false);
    api_->listFiles(currentId_);
}

void MainWindow::onDeleted(qint64 id) {
    if (isAncestorOfCurrent(id)) {
        navigateTo(0);
        return;
    }
    // 删除后立即刷新，当前目录清空时自动回到空态；配额不变（软删除）。
    api_->listFiles(currentId_);
}

void MainWindow::openTrash() {
    TrashDialog dialog(api_, this);
    connect(&dialog, &TrashDialog::changed, this, &MainWindow::refresh);
    dialog.exec();
    refresh();  // 恢复/彻底删除可能改变当前目录与配额
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
    // 重命名 / 删除时条目可能已被其他地方移除，统一按"文件已被删除"提示并刷新。
    if (message == QStringLiteral("文件不存在")) {
        showStatusMessage(QStringLiteral("文件已被删除"), true);
        api_->listFiles(currentId_);
        return;
    }
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
        openPreview(nameItem);
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
    transfers_->cancelAll();
    setTransferExpanded(false);
    api_->logout();
}

void MainWindow::uploadFiles() {
    const QStringList paths = QFileDialog::getOpenFileNames(this, QStringLiteral("选择要上传的文件"));
    if (!paths.isEmpty()) {
        transfers_->addPaths(currentId_, paths);
    }
}

void MainWindow::uploadFolder() {
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择要上传的文件夹"));
    if (!dir.isEmpty()) {
        transfers_->addPaths(currentId_, QStringList() << dir);
    }
}

QStandardItem* MainWindow::selectedFileItem() const {
    if (table_ == nullptr || table_->selectionModel() == nullptr) {
        return nullptr;
    }
    const QModelIndexList rows = table_->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        return nullptr;
    }
    QStandardItem* item = fileModel_->item(rows.first().row(), 0);
    if (item == nullptr || item->data(Qt::UserRole + 1).toString() != QStringLiteral("file")) {
        return nullptr;
    }
    return item;
}

void MainWindow::updateDownloadAction() {
    if (downloadButton_ != nullptr) {
        downloadButton_->setEnabled(selectedFileItem() != nullptr);
    }
}

void MainWindow::startDownload() {
    QStandardItem* item = selectedFileItem();
    if (item == nullptr) {
        showStatusMessage(QStringLiteral("请先选择一个文件"), true);
        return;
    }

    const qint64 remoteId = item->data(Qt::UserRole).toLongLong();
    const QString remoteName = item->text();
    const qint64 size = item->data(Qt::UserRole + 2).toLongLong();

    QSettings settings;
    QString dir = settings.value(QStringLiteral("download/lastDir")).toString();
    if (dir.isEmpty() || !QDir(dir).exists()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    }
    if (dir.isEmpty()) {
        dir = QDir::homePath();
    }
    const QString path =
        QFileDialog::getSaveFileName(this, QStringLiteral("保存到"), QDir(dir).filePath(remoteName));
    if (path.isEmpty()) {
        return;
    }

    // 已存在的 .part 会被续传，磁盘只需再容纳剩余部分。
    qint64 partSize = QFileInfo(path + QStringLiteral(".part")).size();
    if (partSize > size) {
        partSize = 0;
    }
    const QStorageInfo storage(QFileInfo(path).absolutePath());
    if (storage.isValid() && storage.bytesAvailable() < size - partSize) {
        QMessageBox::warning(this, QStringLiteral("磁盘空间不足"),
                             QStringLiteral("剩余空间不足以保存该文件，请换一个位置。"));
        return;
    }
    settings.setValue(QStringLiteral("download/lastDir"), QFileInfo(path).absolutePath());

    transfers_->addDownload(remoteId, remoteName, size, path);
    showStatusMessage(QStringLiteral("已加入下载队列：%1").arg(remoteName), false);
}

void MainWindow::openPreview(QStandardItem* item) {
    if (item == nullptr || previewPanel_ == nullptr) {
        return;
    }
    if (item->data(Qt::UserRole + 1).toString() != QStringLiteral("file")) {
        return;
    }
    if (previewPanel_->isHidden()) {
        previewPanel_->show();
        applyPreviewLayout();
    }
    previewPanel_->showFile(item->data(Qt::UserRole).toLongLong(), item->text(),
                            item->data(Qt::UserRole + 2).toLongLong());
}

void MainWindow::openPreviewForSelected() {
    openPreview(selectedFileItem());
}

void MainWindow::onPreviewClosed() {
    if (previewPanel_ == nullptr || splitter_ == nullptr) {
        return;
    }
    previewPanel_->clearPreview();
    previewPanel_->hide();
    previewLayoutBucket_ = -1;
    // 面板收起后列表占满剩余宽度。
    splitter_->setSizes(QList<int>() << 240 << qMax(0, splitter_->width() - 240) << 0);
}

void MainWindow::onPreviewDownloadRequested(qint64 fileId, const QString& name, qint64 size) {
    Q_UNUSED(name);
    Q_UNUSED(size);
    // 面板里的文件必然来自当前列表；选中它后复用统一下载流程。
    selectRowById(fileId);
    if (selectedFileItem() == nullptr) {
        showStatusMessage(QStringLiteral("文件已不在当前目录，请刷新后重试"), true);
        return;
    }
    startDownload();
}

void MainWindow::applyPreviewLayout() {
    if (splitter_ == nullptr || previewPanel_ == nullptr) {
        return;
    }
    // 窄窗（1024）树 200 / 预览 300，宽窗树 240 / 预览 360，列表吃剩余宽度且不少于 320。
    const int width = splitter_->width();
    const bool narrow = width < 1200;
    const int bucket = narrow ? 0 : 1;
    if (bucket == previewLayoutBucket_) {
        return;
    }
    previewLayoutBucket_ = bucket;

    const int treeWidth = narrow ? 200 : 240;
    int previewWidth = narrow ? 300 : 360;
    int filesWidth = width - treeWidth - previewWidth;
    if (filesWidth < 320) {
        previewWidth = qMax(280, width - treeWidth - 320);
        filesWidth = qMax(0, width - treeWidth - previewWidth);
    }
    splitter_->setSizes(QList<int>() << treeWidth << filesWidth << previewWidth);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        for (const QUrl& url : event->mimeData()->urls()) {
            if (url.isLocalFile()) {
                event->acceptProposedAction();
                return;
            }
        }
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
    QStringList paths;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            paths << url.toLocalFile();
        }
    }
    if (paths.isEmpty()) {
        return;
    }
    event->acceptProposedAction();
    transfers_->addPaths(currentId_, paths);
}

void MainWindow::onUploadCompleted(qint64 fileId, qint64 targetParentId, const QString& finalName) {
    showStatusMessage(QStringLiteral("上传完成：%1").arg(finalName), false);
    if (targetParentId == currentId_) {
        pendingSelectId_ = fileId;
    }
    // 无论是否在当前目录，都刷新一次以同步配额。
    api_->listFiles(currentId_);
}

void MainWindow::onDownloadCompleted(const QString& savePath) {
    showStatusMessage(QStringLiteral("下载完成：%1").arg(QFileInfo(savePath).fileName()), false);
}

void MainWindow::updateQuota(qint64 usedBytes, qint64 quotaBytes) {
    usageText_ = QStringLiteral("已用 %1 / %2").arg(formatSize(usedBytes), formatSize(quotaBytes));
    // 临时消息（完成/失败提示）展示期间不要被配额刷新覆盖。
    if (!transientStatus_) {
        applyUsageText();
        statusLabel_->setStyleSheet(
            QStringLiteral("color:%1; font-size:12px;").arg(theme::kTextMuted.name()));
    }

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
    const int seq = ++statusSeq_;
    transientStatus_ = true;
    statusLabel_->setText(message);
    statusLabel_->setStyleSheet(
        QStringLiteral("color:%1; font-size:12px;")
            .arg(error ? theme::kErrorText.name() : theme::kSuccess.name()));
    QTimer::singleShot(3000, this, [this, seq]() {
        if (seq != statusSeq_) {
            return;
        }
        transientStatus_ = false;
        applyUsageText();
        statusLabel_->setStyleSheet(
            QStringLiteral("color:%1; font-size:12px;").arg(theme::kTextMuted.name()));
    });
}

void MainWindow::applyUsageText() {
    if (statusLabel_ == nullptr || transientStatus_) {
        return;
    }
    const int width = statusLabel_->width();
    if (width <= 20) {
        statusLabel_->setText(usageText_);
        return;
    }
    statusLabel_->setText(statusLabel_->fontMetrics().elidedText(usageText_, Qt::ElideMiddle, width));
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    applyUsageText();
    if (previewPanel_ != nullptr && !previewPanel_->isHidden()) {
        applyPreviewLayout();
    }
}
