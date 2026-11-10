#pragma once

#include <QHash>
#include <QJsonArray>
#include <QMainWindow>
#include <QString>

class ApiClient;
class QLabel;
class QPushButton;
class QProgressBar;
class QStandardItem;
class QStandardItemModel;
class QTableView;
class QTreeView;
class QHBoxLayout;
class QModelIndex;

// 主窗口：左目录树 + 右文件列表 + 底部配额。
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(ApiClient* api, QWidget* parent = nullptr);

signals:
    // 主动退出登录或会话过期，外层负责重新显示登录窗。
    void signedOut();

private slots:
    void refresh();
    void createFolder();
    void logout();
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
    void navigateTo(qint64 folderId);
    void rebuildBreadcrumb();
    void populateTree(qint64 parentId, const QJsonArray& items);
    void dropSubtree(QStandardItem* item);
    QStandardItem* nodeFor(qint64 id) const;
    void updateQuota(qint64 usedBytes, qint64 quotaBytes);
    void showStatusMessage(const QString& message, bool error);
    void selectRowById(qint64 id);

    ApiClient* api_;
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
    QString usageText_;
};
