#pragma once

#include <QElapsedTimer>
#include <QFile>
#include <QObject>
#include <QPair>
#include <QQueue>
#include <QSet>
#include <QString>

#include <functional>

class ApiClient;
class HashWorker;
class QNetworkReply;

// 单个文件的一次上传作业：后台算哈希 → init（秒传/续传）→ 顺序发分片 → complete。
// 只通过 ApiClient::requestFor() 取请求头，不自己保存 token。
class UploadTask : public QObject {
    Q_OBJECT
public:
    // 与 PRD 3.2 对齐：Hashing/Verifying 就是界面上的"校验中"。
    enum State { Waiting, Hashing, Running, Paused, Verifying, Completed, Failed, Canceled };
    Q_ENUM(State)

    UploadTask(ApiClient* api, qint64 parentId, const QString& localPath,
               const QString& remoteName, QObject* parent = nullptr);
    ~UploadTask() override;

    void start();
    // 当前分片响应到达后停下；已暂停时调用无副作用。
    void pause();
    // 重新 init 拿到已收分片后接着传。
    void resume();
    // 终态：删除服务端会话并结束后不再发信号。
    void cancel();

    State state() const { return state_; }
    // 稳定标识（QUuid），传输队列与持久化按它索引任务。
    QString id() const { return id_; }
    QString localPath() const { return localPath_; }
    QString remoteName() const { return remoteName_; }
    qint64 targetParentId() const { return parentId_; }
    qint64 fileSize() const { return size_; }
    QString sha256() const { return sha_; }
    QString finalName() const { return finalName_; }
    qint64 fileId() const { return fileId_; }
    bool instant() const { return instant_; }
    QString errorText() const { return errorText_; }

signals:
    void stateChanged(UploadTask::State state);
    void hashProgress(int percent);
    // sent/total 字节与 3 秒滑动窗口速度（字节/秒）。
    void progress(qint64 sent, qint64 total, qint64 bytesPerSec);
    // 网络中断而暂停：界面用 message 做提示，任务仍可 resume()。
    void interrupted(const QString& message);
    // Completed / Failed / Canceled 时各发一次。
    void finished();

private:
    void setState(State state);
    void fail(const QString& message);
    void beginHash();
    void requestInit();
    void handleInitReply(QNetworkReply* reply);
    void sendNextChunk();
    void sendChunk(int index);
    void handleChunkReply(int index, QNetworkReply* reply);
    void requestComplete();
    void handleCompleteReply(QNetworkReply* reply);
    void retryLater(int delayMs, const std::function<void()>& action);
    void pauseWithError(const QString& message);
    void reportProgress();
    qint64 lengthOfChunk(int index) const;
    void cancelRemoteSession();

    ApiClient* api_;
    qint64 parentId_;
    QString localPath_;
    QString remoteName_;
    QString id_;

    qint64 size_ = 0;
    QString sha_;
    QString sessionId_;
    qint64 chunkSize_ = 0;
    int chunkCount_ = 0;
    QSet<int> received_;
    int retry_ = 0;

    State state_ = Waiting;
    bool paused_ = false;
    bool canceled_ = false;

    HashWorker* hasher_ = nullptr;
    QNetworkReply* reply_ = nullptr;
    QFile file_;

    QString finalName_;
    QString errorText_;
    qint64 fileId_ = 0;
    bool instant_ = false;

    qint64 sentBytes_ = 0;
    QElapsedTimer clock_;
    QQueue<QPair<qint64, qint64>> samples_;
};
