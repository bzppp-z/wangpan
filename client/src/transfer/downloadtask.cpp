#include "downloadtask.h"

#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include "apiclient.h"
#include "hashworker.h"
#include "transferutil.h"

namespace {

const int kMaxRetries = 3;
const int kSpeedWindowMs = 3000;

}  // namespace

DownloadTask::DownloadTask(ApiClient* api, qint64 remoteId, const QString& remoteName, qint64 size,
                           const QString& savePath, QObject* parent)
    : QObject(parent),
      api_(api),
      remoteId_(remoteId),
      remoteName_(remoteName),
      size_(size),
      savePath_(savePath),
      partPath_(savePath + QStringLiteral(".part")) {}

DownloadTask::~DownloadTask() {
    canceled_ = true;
    if (hasher_ != nullptr) {
        hasher_->cancel();
        hasher_->wait(3000);
        hasher_->deleteLater();
        hasher_ = nullptr;
    }
    abortReply();
    closePartFile();
    // 故意不删除 .part：关闭客户端后重下同一文件可以接着下。
}

void DownloadTask::start() {
    if (state_ != Waiting) {
        return;
    }
    beginTransfer();
}

void DownloadTask::beginTransfer() {
    const QFileInfo part(partPath_);
    offset_ = part.exists() ? part.size() : 0;
    if (offset_ > size_) {
        // .part 比远端文件还大，说明它已经不可用，直接清掉重下。
        QFile::remove(partPath_);
        offset_ = 0;
    }
    received_ = offset_;

    if (size_ > 0 && received_ == size_) {
        // 上次已经下完但没来得及校验/改名。
        verify();
        return;
    }

    setState(Running);
    clock_.start();
    samples_.clear();
    samples_.enqueue(qMakePair(qint64(0), received_));
    emit progress(received_, size_, 0);
    beginRequest();
}

bool DownloadTask::openPartFile() {
    if (part_.isOpen()) {
        return true;
    }
    part_.setFileName(partPath_);
    // 必须用 ReadWrite：WriteOnly 会截断已存在的 .part，续传数据就没了。
    if (!part_.open(QIODevice::ReadWrite)) {
        fail(QStringLiteral("无法写入文件，请检查保存位置"));
        return false;
    }
    if (!part_.resize(offset_)) {
        fail(QStringLiteral("无法写入文件，请检查磁盘空间"));
        return false;
    }
    // 续传时必须把写指针移到已有内容末尾，否则会从头覆盖已下部分。
    if (!part_.seek(offset_)) {
        fail(QStringLiteral("无法写入文件，请检查磁盘空间"));
        return false;
    }
    return true;
}

void DownloadTask::beginRequest() {
    if (canceled_ || paused_) {
        return;
    }
    if (!openPartFile()) {
        return;
    }
    const QNetworkRequest request = api_->requestForDownload(remoteId_, offset_);
    reply_ = api_->manager()->get(request);
    connect(reply_, &QNetworkReply::readyRead, this, &DownloadTask::handleReadyRead);
    connect(reply_, &QNetworkReply::finished, this, &DownloadTask::handleReplyFinished);
}

void DownloadTask::appendData(const QByteArray& data) {
    if (!part_.isOpen()) {
        fail(QStringLiteral("无法写入文件，请检查保存位置"));
        return;
    }
    const qint64 written = part_.write(data);
    if (written != data.size()) {
        fail(QStringLiteral("写入文件失败，请检查磁盘空间"));
        return;
    }
    received_ += written;
    reportProgress();
}

void DownloadTask::handleReadyRead() {
    if (canceled_ || paused_ || reply_ == nullptr) {
        return;
    }
    const int status = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status < 200 || status >= 300) {
        return;  // 错误响应体留到 finished 再解析
    }
    if (status == 200 && offset_ > 0 && !restarted_) {
        // 服务端不支持 Range：清掉已下部分，从 0 重下。
        part_.resize(0);
        part_.seek(0);
        offset_ = 0;
        received_ = 0;
        restarted_ = true;
        samples_.clear();
        samples_.enqueue(qMakePair(qint64(0), qint64(0)));
        clock_.restart();
    }
    const QByteArray data = reply_->readAll();
    if (!data.isEmpty()) {
        appendData(data);
    }
}

void DownloadTask::handleReplyFinished() {
    QNetworkReply* reply = reply_;
    reply_ = nullptr;
    if (reply == nullptr) {
        return;
    }
    reply->deleteLater();
    if (canceled_) {
        return;
    }

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != QNetworkReply::NoError) {
        closePartFile();
        ++retry_;
        if (retry_ <= kMaxRetries) {
            offset_ = received_;
            retryLater(1000 << (retry_ - 1), [this]() { beginRequest(); });
            return;
        }
        offset_ = received_;
        pauseWithError(QStringLiteral("网络中断，已暂停"));
        return;
    }

    if (status == 401) {
        closePartFile();
        fail(QStringLiteral("登录已过期，请重新登录"));
        return;
    }
    if (status == 404) {
        closePartFile();
        fail(QStringLiteral("文件已被删除"));
        return;
    }
    if (status == 416) {
        // .part 与远端不一致（远端文件被替换或变短）：删掉重下一次。
        closePartFile();
        if (!restarted_) {
            QFile::remove(partPath_);
            offset_ = 0;
            received_ = 0;
            restarted_ = true;
            beginRequest();
            return;
        }
        fail(QStringLiteral("文件已变化，请重新下载"));
        return;
    }
    if (status < 200 || status >= 300) {
        closePartFile();
        fail(ApiClient::errorTextFor(status, QString::fromUtf8(reply->readAll()), reply->error()));
        return;
    }

    // 少见情况：整个响应体直到 finished 才可读，此时也要先处理"200 忽略 Range"。
    if (status == 200 && offset_ > 0 && !restarted_) {
        part_.resize(0);
        part_.seek(0);
        offset_ = 0;
        received_ = 0;
        restarted_ = true;
    }
    const QByteArray rest = reply->readAll();
    if (!rest.isEmpty()) {
        appendData(rest);
    }
    if (received_ != size_) {
        closePartFile();
        ++retry_;
        if (retry_ <= kMaxRetries) {
            offset_ = received_;
            retryLater(1000 << (retry_ - 1), [this]() { beginRequest(); });
            return;
        }
        offset_ = received_;
        pauseWithError(QStringLiteral("网络中断，已暂停"));
        return;
    }

    serverSha_ = QString::fromUtf8(reply->rawHeader("X-Content-SHA256")).trimmed();
    retry_ = 0;
    part_.flush();
    closePartFile();
    verify();
}

void DownloadTask::verify() {
    if (serverSha_.isEmpty()) {
        // 服务端没给校验值就信任 HTTP 完整性，直接落成最终文件。
        finalize();
        return;
    }
    setState(Verifying);
    hasher_ = new HashWorker(partPath_, this);
    connect(hasher_, &HashWorker::done, this, [this](const QString& sha) {
        if (hasher_ != nullptr) {
            hasher_->deleteLater();
            hasher_ = nullptr;
        }
        if (canceled_) {
            return;
        }
        if (sha.compare(serverSha_, Qt::CaseInsensitive) != 0) {
            QFile::remove(partPath_);
            fail(QStringLiteral("文件校验失败，请重试"));
            return;
        }
        finalize();
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

void DownloadTask::finalize() {
    QString target = savePath_;
    if (QFileInfo::exists(target)) {
        const QFileInfo info(savePath_);
        target = TransferUtil::uniqueLocalPath(info.absolutePath(), info.fileName());
    }
    if (!QFile::rename(partPath_, target)) {
        fail(QStringLiteral("保存文件失败，请检查磁盘权限"));
        return;
    }
    savePath_ = target;
    received_ = size_;
    emit progress(size_, size_, 0);
    setState(Completed);
    emit finished();
}

void DownloadTask::pause() {
    if (state_ == Completed || state_ == Failed || state_ == Canceled || state_ == Paused) {
        return;
    }
    if (state_ == Verifying) {
        return;  // 校验只差几秒，不再打断
    }
    paused_ = true;
    abortReply();
    closePartFile();
    if (state_ == Running) {
        setState(Paused);
    }
}

void DownloadTask::resume() {
    if (state_ != Paused) {
        return;
    }
    paused_ = false;
    retry_ = 0;
    errorText_.clear();
    syncOffset();
    received_ = offset_;
    if (size_ > 0 && received_ == size_) {
        verify();
        return;
    }
    setState(Running);
    clock_.restart();
    samples_.clear();
    samples_.enqueue(qMakePair(qint64(0), received_));
    emit progress(received_, size_, 0);
    beginRequest();
}

void DownloadTask::cancel() {
    if (state_ == Completed || state_ == Failed || state_ == Canceled) {
        return;
    }
    canceled_ = true;
    paused_ = false;
    if (hasher_ != nullptr) {
        hasher_->cancel();
        hasher_->wait(3000);
        hasher_->deleteLater();
        hasher_ = nullptr;
    }
    abortReply();
    closePartFile();
    QFile::remove(partPath_);
    setState(Canceled);
    emit finished();
}

void DownloadTask::syncOffset() {
    const QFileInfo part(partPath_);
    offset_ = part.exists() ? part.size() : 0;
    if (offset_ > size_) {
        QFile::remove(partPath_);
        offset_ = 0;
    }
}

void DownloadTask::setState(State state) {
    if (state_ == state) {
        return;
    }
    state_ = state;
    emit stateChanged(state);
}

void DownloadTask::fail(const QString& message) {
    if (state_ == Completed || state_ == Failed || state_ == Canceled) {
        return;
    }
    errorText_ = message;
    setState(Failed);
    emit finished();
}

void DownloadTask::pauseWithError(const QString& message) {
    paused_ = true;
    retry_ = 0;
    errorText_ = message;
    setState(Paused);
    emit interrupted(message);
}

void DownloadTask::retryLater(int delayMs, const std::function<void()>& action) {
    QTimer::singleShot(delayMs, this, [this, action]() {
        if (canceled_ || paused_) {
            return;
        }
        action();
    });
}

void DownloadTask::reportProgress() {
    const qint64 now = clock_.isValid() ? clock_.elapsed() : 0;
    samples_.enqueue(qMakePair(now, received_));
    // 只保留最近 3 秒的样本，但至少保留两个点用于算速度。
    while (samples_.size() > 2 && samples_.at(1).first + kSpeedWindowMs < now) {
        samples_.dequeue();
    }
    qint64 speed = 0;
    if (!samples_.isEmpty()) {
        const qint64 elapsed = now - samples_.first().first;
        if (elapsed > 0) {
            speed = (received_ - samples_.first().second) * 1000 / elapsed;
        }
    }
    emit progress(received_, size_, speed);
}

void DownloadTask::closePartFile() {
    if (part_.isOpen()) {
        part_.close();
    }
}

void DownloadTask::abortReply() {
    if (reply_ == nullptr) {
        return;
    }
    reply_->disconnect(this);
    reply_->abort();
    reply_->deleteLater();
    reply_ = nullptr;
}
