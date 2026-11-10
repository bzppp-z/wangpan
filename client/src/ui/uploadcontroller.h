#pragma once

#include <QHash>
#include <QObject>
#include <QPair>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QVector>

class ApiClient;
class QNetworkReply;
class UploadTask;

// 把"本地路径 → 上传任务"的逻辑从窗口里抽出来：目录按层级创建、文件排队、串行执行。
// 本阶段并发固定为 1（PRD 3.2），阶段 5 会把它换成统一传输队列。
class UploadController : public QObject {
    Q_OBJECT
public:
    explicit UploadController(ApiClient* api, QObject* parent = nullptr);
    ~UploadController() override;

    // 把本地文件与文件夹加入队列；文件夹按原有层级建目录（空目录也会创建）。
    void addPaths(qint64 parentId, const QStringList& paths);
    bool isBusy() const;
    // 登出/关窗：删除服务端上传会话并丢弃队列。
    void cancelAll();

signals:
    // 状态栏摘要；percent < 0 表示这一段只有文字、不显示进度条。
    void summaryChanged(const QString& text, int percent);
    void taskFinished(qint64 fileId, qint64 targetParentId, const QString& finalName);
    void taskFailed(const QString& message);

private:
    struct FileJob {
        qint64 parentId;
        QString localPath;
        QString name;
    };
    // rel 是相对被拖入文件夹根的路径（根自身为空串），parentRel 是上级目录的 rel。
    struct DirSpec {
        QString rel;
        QString name;
        QString parentRel;
    };
    struct DirBatch {
        qint64 targetId = 0;
        QVector<DirSpec> dirs;  // 按层级升序：父目录一定在子目录之前
        QVector<QPair<QString, QString>> files;  // (本地绝对路径, 所在目录 rel)
        int next = 0;
        QHash<QString, qint64> ids;  // rel → 服务端 folder id
    };

    void enqueueFolder(qint64 parentId, const QString& dirPath);
    void startNext();
    void createNextDir();
    void handleDirReply(QNetworkReply* reply);
    void startUpload();
    void reportSummary();
    void onTaskFinished();
    void clearSummary();

    ApiClient* api_;
    QQueue<DirBatch> batches_;
    QQueue<FileJob> fileJobs_;
    UploadTask* task_ = nullptr;
    QNetworkReply* dirReply_ = nullptr;
    int percent_ = 0;
    qint64 speed_ = 0;
};
