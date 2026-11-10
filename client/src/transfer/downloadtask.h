#pragma once

#include <QElapsedTimer>
#include <QFile>
#include <QObject>
#include <QPair>
#include <QQueue>
#include <QString>

#include <functional>

class ApiClient;
class HashWorker;
class QNetworkReply;

// 单个文件的一次下载作业：写 .part → Range 续传 → SHA-256 校验 → 改名成最终文件。
// 只通过 ApiClient::requestForDownload() 取请求，不自己保存 token。
class DownloadTask : public QObject {
    Q_OBJECT
public:
    enum State { Waiting, Running, Paused, Verifying, Completed, Failed, Canceled };
    Q_ENUM(State)

    DownloadTask(ApiClient* api, qint64 remoteId, const QString& remoteName, qint64 size,
                 const QString& savePath, QObject* parent = nullptr);
    ~DownloadTask() override;

    // 开始：检测同名 .part 决定是否续传、已下完则直接校验。
    void start();
    // 中止在飞请求并保留 .part；已暂停/终态时无副作用。
    void pause();
    void resume();
    // 终态：删除 .part 并结束后不再发信号。
    void cancel();

    State state() const { return state_; }
    // 稳定标识（QUuid），传输队列与持久化按它索引任务。
    QString id() const { return id_; }
    qint64 remoteId() const { return remoteId_; }
    QString remoteName() const { return remoteName_; }
    QString savePath() const { return savePath_; }
    QString partPath() const { return partPath_; }
    qint64 fileSize() const { return size_; }
    qint64 receivedBytes() const { return received_; }
    QString errorText() const { return errorText_; }

signals:
    void stateChanged(DownloadTask::State state);
    // received/total 字节与 3 秒滑动窗口速度（字节/秒）。
    void progress(qint64 received, qint64 total, qint64 bytesPerSec);
    // 网络中断而暂停：任务仍可 resume()。
    void interrupted(const QString& message);
    // Completed / Failed / Canceled 时各发一次。
    void finished();

private:
    void setState(State state);
    void fail(const QString& message);
    void pauseWithError(const QString& message);
    void beginTransfer();
    void beginRequest();
    void appendData(const QByteArray& data);
    void handleReadyRead();
    void handleReplyFinished();
    void verify();
    void finalize();
    void retryLater(int delayMs, const std::function<void()>& action);
    void reportProgress();
    void closePartFile();
    bool openPartFile();
    void syncOffset();
    void abortReply();

    ApiClient* api_;
    qint64 remoteId_;
    QString remoteName_;
    QString id_;
    qint64 size_;
    QString savePath_;
    QString partPath_;

    State state_ = Waiting;
    bool paused_ = false;
    bool canceled_ = false;
    bool restarted_ = false;  // 服务端不接受 Range 时只允许回到 0 重下一次
    int retry_ = 0;

    QNetworkReply* reply_ = nullptr;
    QFile part_;
    qint64 offset_ = 0;    // 本次请求的起点
    qint64 received_ = 0;  // 已落盘字节数
    QString serverSha_;
    QString errorText_;

    HashWorker* hasher_ = nullptr;
    QElapsedTimer clock_;
    QQueue<QPair<qint64, qint64>> samples_;
};
