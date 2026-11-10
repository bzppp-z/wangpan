#pragma once

#include <QHash>
#include <QObject>
#include <QPair>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QVector>

class ApiClient;
class DownloadTask;
class UploadTask;
class QNetworkReply;
class QTimer;

// 上传 / 下载共用的任务状态。
enum class TransferState { Waiting, Running, Paused, Completed, Failed, Canceled };

// 传输面板与摘要行使用的只读快照。
struct TransferSnapshot {
    QString id;
    bool upload = true;
    QString name;
    qint64 size = 0;
    qint64 done = 0;
    qint64 speed = 0;
    TransferState state = TransferState::Waiting;
    QString error;
    QString localPath;
    QString savePath;
    // 从磁盘恢复、尚未点击"继续"。
    bool restored = false;
};

// 上传 / 下载统一队列：最多 3 个同时进行，其余等待；支持暂停/继续/取消/重试/清空。
// 未完成任务写入 AppDataLocation/transfers.json，下次登录恢复为"等待中"，由用户点继续。
class TransferManager : public QObject {
    Q_OBJECT
public:
    static const int kMaxConcurrent = 3;

    explicit TransferManager(ApiClient* api, QObject* parent = nullptr);
    ~TransferManager() override;

    // 把本地文件 / 文件夹加入上传队列；文件夹按原有层级创建目录（空目录也会创建）。
    void addPaths(qint64 parentId, const QStringList& paths);
    QString addUpload(const QString& localPath, qint64 parentId, const QString& desiredName);
    QString addDownload(qint64 remoteId, const QString& remoteName, qint64 size,
                        const QString& savePath);

    void pause(const QString& id);
    void resume(const QString& id);
    void cancel(const QString& id);
    void retry(const QString& id);
    void clearCompleted();
    // 登出：取消所有任务并删除持久化记录。
    void cancelAll();

    QVector<TransferSnapshot> snapshots() const;
    bool isEmpty() const;
    // 登录后调用：恢复上次未完成任务，状态置"等待中"，不自动开始。
    void restorePersisted();

signals:
    void taskUpdated(const QString& id);
    // 底部摘要：全部任务清空时 text 为空。
    void summaryChanged(const QString& text, bool anyActive);
    void uploadCompleted(qint64 fileId, qint64 parentId, const QString& finalName);
    void downloadCompleted(const QString& savePath);
    // 可展示给用户的失败 / 中断提示。
    void failed(const QString& message);

private:
    struct Entry {
        QString id;
        bool upload = true;
        UploadTask* up = nullptr;
        DownloadTask* down = nullptr;
        qint64 parentId = 0;
        QString localPath;
        QString name;
        qint64 remoteId = 0;
        QString savePath;
        qint64 size = 0;
        qint64 done = 0;
        qint64 speed = 0;
        TransferState state = TransferState::Waiting;
        QString error;
        // 从磁盘恢复、尚未点击继续：即使有空闲并发位也不自动开始。
        bool restored = false;
    };
    // 上传文件夹时先按层级建目录，再把文件逐个入队。
    struct DirSpec {
        QString rel;
        QString name;
        QString parentRel;
    };
    struct DirBatch {
        qint64 targetId = 0;
        QVector<DirSpec> dirs;
        int next = 0;
        QVector<QPair<QString, QString>> files;  // (本地绝对路径, 所在目录 rel)
        QHash<QString, qint64> ids;              // rel → 服务端 folder id
    };

    int indexOf(const QString& id) const;
    void addEntry(const Entry& entry);
    void attachUpload(int index);
    void attachDownload(int index);
    void destroyTask(Entry& entry);
    void startWaiting();
    void startEntry(int index);
    void onTaskFinished(const QString& id);
    void updateSummary();
    int runningCount() const;
    void markDirty();
    void savePersisted();
    QString persistPath() const;

    void enqueueFolder(qint64 parentId, const QString& dirPath);
    void createNextDir();
    void handleDirReply(QNetworkReply* reply);

    ApiClient* api_;
    QVector<Entry> entries_;
    QVector<DirBatch> batches_;
    QNetworkReply* dirReply_ = nullptr;
    QTimer* timer_;
    bool haltQueue_ = false;
    bool dirty_ = false;
    QString lastSummary_;
    bool lastActive_ = false;
};
