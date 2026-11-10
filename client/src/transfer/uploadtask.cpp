#include "uploadtask.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <functional>

#include "apiclient.h"
#include "hashworker.h"
#include "transferutil.h"

namespace {

const int kMaxRetries = 3;
const int kSpeedWindowMs = 3000;

}  // namespace

UploadTask::UploadTask(ApiClient* api, qint64 parentId, const QString& localPath,
                       const QString& remoteName, QObject* parent)
    : QObject(parent), api_(api), parentId_(parentId), localPath_(localPath), remoteName_(remoteName) {}

UploadTask::~UploadTask() {
    canceled_ = true;
    if (hasher_ != nullptr) {
        hasher_->cancel();
        hasher_->wait(3000);
    }
    if (reply_ != nullptr) {
        reply_->disconnect(this);
        reply_->abort();
        reply_->deleteLater();
        reply_ = nullptr;
    }
    if (file_.isOpen()) {
        file_.close();
    }
    cancelRemoteSession();
}

void UploadTask::start() {
    if (state_ != Waiting) {
        return;
    }
    const QFileInfo info(localPath_);
    if (!info.isFile()) {
        fail(QStringLiteral("无法读取文件：%1").arg(localPath_));
        return;
    }
    size_ = info.size();
    setState(Hashing);
    beginHash();
}

void UploadTask::beginHash() {
    hasher_ = new HashWorker(localPath_, this);
    connect(hasher_, &HashWorker::progress, this, [this](int percent) {
        if (canceled_) {
            return;
        }
        emit hashProgress(percent);
    });
    connect(hasher_, &HashWorker::done, this, [this](const QString& sha) {
        if (hasher_ != nullptr) {
            hasher_->deleteLater();
            hasher_ = nullptr;
        }
        if (canceled_) {
            return;
        }
        sha_ = sha;
        if (paused_) {
            setState(Paused);
            return;
        }
        setState(Running);
        requestInit();
    });
    connect(hasher_, &HashWorker::failed, this, [this](const QString& message) {
        if (hasher_ != nullptr) {
            hasher_->deleteLater();
            hasher_ = nullptr;
        }
        if (canceled_) {
            return;
        }
        fail(message);
    });
    hasher_->start();
}

void UploadTask::pause() {
    if (state_ == Completed || state_ == Failed || state_ == Canceled || state_ == Paused) {
        return;
    }
    paused_ = true;
    // 没有请求在飞时立刻停下；有请求则在响应回调里转 Paused。
    if (state_ == Running && reply_ == nullptr) {
        setState(Paused);
    }
}

void UploadTask::resume() {
    if (state_ != Paused) {
        return;
    }
    paused_ = false;
    retry_ = 0;
    errorText_.clear();
    if (sha_.isEmpty()) {
        setState(Hashing);
        beginHash();
        return;
    }
    setState(Running);
    requestInit();
}

void UploadTask::cancel() {
    if (state_ == Completed || state_ == Failed || state_ == Canceled) {
        return;
    }
    canceled_ = true;
    paused_ = false;
    if (reply_ != nullptr) {
        reply_->disconnect(this);
        reply_->abort();
        reply_->deleteLater();
        reply_ = nullptr;
    }
    if (hasher_ != nullptr) {
        hasher_->cancel();
        hasher_->wait(3000);
        hasher_->deleteLater();
        hasher_ = nullptr;
    }
    cancelRemoteSession();
    if (file_.isOpen()) {
        file_.close();
    }
    setState(Canceled);
    emit finished();
}

void UploadTask::setState(State state) {
    if (state_ == state) {
        return;
    }
    state_ = state;
    emit stateChanged(state);
}

void UploadTask::fail(const QString& message) {
    if (state_ == Completed || state_ == Failed || state_ == Canceled) {
        return;
    }
    errorText_ = message;
    setState(Failed);
    emit finished();
}

void UploadTask::pauseWithError(const QString& message) {
    paused_ = true;
    retry_ = 0;
    errorText_ = message;
    setState(Paused);
    emit interrupted(message);
}

void UploadTask::retryLater(int delayMs, const std::function<void()>& action) {
    QTimer::singleShot(delayMs, this, [this, action]() {
        if (canceled_ || paused_) {
            return;
        }
        action();
    });
}

void UploadTask::requestInit() {
    QJsonObject payload;
    payload["parent_id"] = static_cast<double>(parentId_);
    payload["name"] = remoteName_;
    payload["size"] = static_cast<double>(size_);
    payload["sha256"] = sha_;

    const QNetworkRequest request = api_->requestFor(QStringLiteral("/api/upload/init"));
    reply_ = api_->manager()->post(request,
                                   QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply_, &QNetworkReply::finished, this, [this]() {
        QNetworkReply* finished = reply_;
        reply_ = nullptr;
        handleInitReply(finished);
    });
}

void UploadTask::handleInitReply(QNetworkReply* reply) {
    if (reply == nullptr) {
        return;
    }
    reply->deleteLater();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray raw = reply->readAll();
    if (canceled_) {
        return;
    }

    // 507 是配额不足，不是可重试的服务端故障，必须先于 5xx 重试分支判断。
    if (status == 401) {
        fail(QStringLiteral("登录已过期，请重新登录"));
        return;
    }
    if (status == 507) {
        fail(QStringLiteral("空间不足，请先清理文件"));
        return;
    }
    if (status == 0 || status >= 500) {
        ++retry_;
        if (retry_ <= kMaxRetries) {
            retryLater(1000 << (retry_ - 1), [this]() { requestInit(); });
            return;
        }
        pauseWithError(QStringLiteral("网络中断，已暂停"));
        return;
    }
    if (status < 200 || status >= 300) {
        fail(ApiClient::errorTextFor(status, QString::fromUtf8(raw), QNetworkReply::NoError));
        return;
    }

    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    if (status == 201 && obj.value("instant").toBool()) {
        finalName_ = obj.value("name").toString();
        fileId_ = static_cast<qint64>(obj.value("file_id").toDouble());
        instant_ = true;
        sentBytes_ = size_;
        emit progress(size_, size_, 0);
        setState(Completed);
        emit finished();
        return;
    }

    sessionId_ = obj.value("session_id").toString();
    chunkSize_ = static_cast<qint64>(obj.value("chunk_size").toDouble());
    chunkCount_ = obj.value("chunk_count").toInt();
    if (sessionId_.isEmpty() || chunkSize_ <= 0) {
        fail(QStringLiteral("服务器返回的上传参数不完整"));
        return;
    }

    received_.clear();
    sentBytes_ = 0;
    const QJsonArray items = obj.value("received").toArray();
    for (int i = 0; i < items.size(); ++i) {
        const int index = items.at(i).toInt();
        if (index >= 0 && index < chunkCount_ && !received_.contains(index)) {
            received_.insert(index);
            sentBytes_ += lengthOfChunk(index);
        }
    }
    retry_ = 0;
    clock_.start();
    samples_.clear();
    samples_.enqueue(qMakePair(qint64(0), sentBytes_));

    setState(Running);
    emit progress(sentBytes_, size_, 0);
    if (paused_) {
        setState(Paused);
        return;
    }
    sendNextChunk();
}

void UploadTask::sendNextChunk() {
    if (canceled_ || paused_) {
        return;
    }
    for (int i = 0; i < chunkCount_; ++i) {
        if (!received_.contains(i)) {
            sendChunk(i);
            return;
        }
    }
    requestComplete();
}

void UploadTask::sendChunk(int index) {
    if (canceled_ || paused_) {
        return;
    }
    if (!file_.isOpen()) {
        file_.setFileName(localPath_);
        if (!file_.open(QIODevice::ReadOnly)) {
            fail(QStringLiteral("无法读取文件：%1").arg(localPath_));
            return;
        }
    }
    const qint64 length = lengthOfChunk(index);
    if (!file_.seek(static_cast<qint64>(index) * chunkSize_)) {
        fail(QStringLiteral("读取文件失败"));
        return;
    }
    const QByteArray data = file_.read(length);
    if (static_cast<qint64>(data.size()) != length) {
        fail(QStringLiteral("读取文件失败"));
        return;
    }

    const QString path = QStringLiteral("/api/upload/%1/chunks/%2").arg(sessionId_).arg(index);
    const QNetworkRequest request =
        api_->requestFor(path, QStringLiteral("application/octet-stream"));
    reply_ = api_->manager()->put(request, data);
    connect(reply_, &QNetworkReply::finished, this, [this, index]() {
        QNetworkReply* finished = reply_;
        reply_ = nullptr;
        handleChunkReply(index, finished);
    });
}

void UploadTask::handleChunkReply(int index, QNetworkReply* reply) {
    if (reply == nullptr) {
        return;
    }
    reply->deleteLater();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray raw = reply->readAll();
    if (canceled_) {
        return;
    }

    if (status == 0 || status >= 500) {
        ++retry_;
        if (retry_ <= kMaxRetries) {
            retryLater(1000 << (retry_ - 1), [this, index]() { sendChunk(index); });
            return;
        }
        pauseWithError(QStringLiteral("网络中断，已暂停"));
        return;
    }
    if (status == 401) {
        fail(QStringLiteral("登录已过期，请重新登录"));
        return;
    }
    if (status < 200 || status >= 300) {
        fail(ApiClient::errorTextFor(status, QString::fromUtf8(raw), QNetworkReply::NoError));
        return;
    }

    received_.insert(index);
    sentBytes_ += lengthOfChunk(index);
    retry_ = 0;
    reportProgress();
    if (paused_) {
        setState(Paused);
        return;
    }
    sendNextChunk();
}

void UploadTask::requestComplete() {
    if (canceled_ || paused_) {
        return;
    }
    setState(Verifying);
    const QNetworkRequest request =
        api_->requestFor(QStringLiteral("/api/upload/%1/complete").arg(sessionId_));
    // 显式空体：保证带 Content-Length，避免服务端等到读超时。
    reply_ = api_->manager()->post(request, QByteArray());
    connect(reply_, &QNetworkReply::finished, this, [this]() {
        QNetworkReply* finished = reply_;
        reply_ = nullptr;
        handleCompleteReply(finished);
    });
}

void UploadTask::handleCompleteReply(QNetworkReply* reply) {
    if (reply == nullptr) {
        return;
    }
    reply->deleteLater();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray raw = reply->readAll();
    if (canceled_) {
        return;
    }

    if (status == 401) {
        fail(QStringLiteral("登录已过期，请重新登录"));
        return;
    }
    if (status == 507) {
        fail(QStringLiteral("空间不足，请先清理文件"));
        return;
    }
    if (status == 0 || status >= 500) {
        ++retry_;
        if (retry_ <= kMaxRetries) {
            retryLater(1000 << (retry_ - 1), [this]() { requestComplete(); });
            return;
        }
        pauseWithError(QStringLiteral("网络中断，已暂停"));
        return;
    }
    if (status < 200 || status >= 300) {
        fail(ApiClient::errorTextFor(status, QString::fromUtf8(raw), QNetworkReply::NoError));
        return;
    }

    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    const QString serverSha = obj.value("sha256").toString();
    const qint64 serverSize = static_cast<qint64>(obj.value("size").toDouble());
    if (serverSha != sha_ || serverSize != size_) {
        fail(QStringLiteral("文件校验失败，请重试"));
        return;
    }
    finalName_ = obj.value("name").toString();
    fileId_ = static_cast<qint64>(obj.value("file_id").toDouble());
    instant_ = false;
    sentBytes_ = size_;
    emit progress(size_, size_, 0);
    setState(Completed);
    emit finished();
}

void UploadTask::reportProgress() {
    const qint64 now = clock_.isValid() ? clock_.elapsed() : 0;
    samples_.enqueue(qMakePair(now, sentBytes_));
    // 只保留最近 3 秒的样本，但至少保留两个点用于算速度。
    while (samples_.size() > 2 && samples_.at(1).first + kSpeedWindowMs < now) {
        samples_.dequeue();
    }
    qint64 speed = 0;
    if (!samples_.isEmpty()) {
        const qint64 elapsed = now - samples_.first().first;
        if (elapsed > 0) {
            speed = (sentBytes_ - samples_.first().second) * 1000 / elapsed;
        }
    }
    emit progress(sentBytes_, size_, speed);
}

qint64 UploadTask::lengthOfChunk(int index) const {
    return TransferUtil::chunkLength(index, size_, chunkSize_);
}

void UploadTask::cancelRemoteSession() {
    if (sessionId_.isEmpty() || api_ == nullptr) {
        return;
    }
    const QNetworkRequest request =
        api_->requestFor(QStringLiteral("/api/upload/%1").arg(sessionId_));
    sessionId_.clear();
    QNetworkReply* reply = api_->manager()->deleteResource(request);
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
}
