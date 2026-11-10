#pragma once

#include <QHash>
#include <QJsonArray>
#include <QMainWindow>
#include <QString>

class ApiClient;
class TransferManager;
class TransferPanel;
class PreviewPanel;
class QDragEnterEvent;
class QDropEvent;
class QFrame;
class QLabel;
class QPushButton;
class QProgressBar;
class QResizeEvent;
class QSplitter;
class QStandardItem;
class QStandardItemModel;
class QTableView;
class QTreeView;
class QHBoxLayout;
class QModelIndex;

// 主窗口：左目录树 + 右文件列表 + 底部传输摘要（可展开）与配额。
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(ApiClient* api, QWidget* parent = nullptr);

    // 截图与调试使用：访问统一传输队列。
    TransferManager* transferManager() const { return transfers_; }

signals:
    // 主动退出登录或会话过期，外层负责重新显示登录窗。
    void signedOut();

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void refresh();
    void createFolder();
    void logout();
    void uploadFiles();
    void uploadFolder();
    void startDownload();
    void updateDownloadAction();
    void openPreviewForSelected();
    void onPreviewClosed();
    void onPreviewDownloadRequested(qint64 fileId, const QString& name, qint64 size);
    void onTransferSummary(const QString& text, bool anyActive);
    void setTransferExpanded(bool expanded);
    void onUploadCompleted(qint64 fileId, qint64 parentId, const QString& finalName);
    void onDownloadCompleted(const QString& savePath);
    void onFilesLoaded(const QJsonArray& items, qint64 usedBytes, qint64 quotaBytes);
    void onFolderCreated(qint64 id, const QString& finalName);
    void onFailed(const QString& message);
    void onUnauthorized();
    void onTreeCurrentChanged(const QModelIndex& current, const QModelIndex& previous);
    void onRowActivated(const QModelIndex& index);

private:
    void buildUi();
    QWidget* buildToolBar();
    QWidget* buildFilePanel();
    QWidget* buildTransferBar();
    void navigateTo(qint64 folderId);
    void rebuildBreadcrumb();
    void populateTree(qint64 parentId, const QJsonArray& items);
    void dropSubtree(QStandardItem* item);
    QStandardItem* nodeFor(qint64 id) const;
    void updateQuota(qint64 usedBytes, qint64 quotaBytes);
    // 临时消息统一 3 秒后恢复"已用 X / Y"。
    void showStatusMessage(const QString& message, bool error);
    void selectRowById(qint64 id);
    // 当前选中的是文件时返回其条目，文件夹或未选中返回 nullptr。
    QStandardItem* selectedFileItem() const;
    // 双击文件 / 右键"预览"：打开右侧面板并请求预览。
    void openPreview(QStandardItem* item);
    // 打开预览面板时按窗口宽度分配三栏（窄窗预览 300，宽窗 360）。
    void applyPreviewLayout();
    // 状态栏宽度变化时把配额文案按中间省略，避免挤掉右侧进度条。
    void applyUsageText();

    ApiClient* api_;
    TransferManager* transfers_;
    TransferPanel* transferPanel_;
    QFrame* transferBar_;
    QLabel* transferSummaryLabel_;
    QLabel* transferArrow_;
    bool transferExpanded_;
    PreviewPanel* previewPanel_;
    QSplitter* splitter_;
    QSplitter* vSplitter_;
    int previewLayoutBucket_;
    qint64 currentId_;
    QStandardItem* currentItem_;
    qint64 pendingSelectId_;

    QTreeView* tree_;
    QStandardItemModel* treeModel_;
    QHash<qint64, QStandardItem*> nodes_;

    QTableView* table_;
    QStandardItemModel* fileModel_;
    QHBoxLayout* breadcrumbLayout_;

    QLabel* statusLabel_;
    QProgressBar* quotaBar_;
    QPushButton* downloadButton_;
    QString usageText_;
    // 临时消息展示期间，配额刷新不能覆盖它；statusSeq_ 用来作废过期的恢复定时器。
    bool transientStatus_;
    int statusSeq_;
};
