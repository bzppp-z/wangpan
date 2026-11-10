#include "ui/uploadcontroller.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <algorithm>

#include "apiclient.h"
#include "transfer/transferutil.h"
#include "transfer/uploadtask.h"

namespace {

// 摘要过长会挤掉配额信息，超出后截断。
QString elideSummary(const QString& text) {
    return text.size() > 64 ? text.left(61) + QStringLiteral("...") : text;
}

// 相对路径所在目录；"a.bin" 这种根目录文件返回空串。
QString parentRelOf(const QString& rel) {
    const QString path = QFileInfo(rel).path();
    return path == QStringLiteral(".") ? QString() : path;
}

}  // namespace

UploadController::UploadController(ApiClient* api, QObject* parent)
    : QObject(parent), api_(api) {}

UploadController::~UploadController() {
    cancelAll();
}

bool UploadController::isBusy() const {
    return task_ != nullptr || dirReply_ != nullptr || !batches_.isEmpty() || !fileJobs_.isEmpty();
}

void UploadController::addPaths(qint64 parentId, const QStringList& paths) {
    bool queued = false;
    for (const QString& path : paths) {
        const QFileInfo info(path);
        if (info.isDir()) {
            enqueueFolder(parentId, path);
            queued = true;
        } else if (info.isFile()) {
            FileJob job;
            job.parentId = parentId;
            job.localPath = info.absoluteFilePath();
            job.name = info.fileName();
            fileJobs_.enqueue(job);
            queued = true;
        } else {
            emit taskFailed(QStringLiteral("无法读取：%1").arg(path));
        }
    }
    if (queued) {
        startNext();
    }
}

void UploadController::enqueueFolder(qint64 parentId, const QString& dirPath) {
    const QDir dir(dirPath);
    if (!dir.exists()) {
        emit taskFailed(QStringLiteral("文件夹不存在：%1").arg(dirPath));
        return;
    }
    const QString base = dir.absolutePath();
    const QString rootName = dir.dirName().isEmpty() ? QStringLiteral("新建文件夹") : dir.dirName();

    DirBatch batch;
    batch.targetId = parentId;
    DirSpec root;
    root.rel = QString();
    root.name = rootName;
    batch.dirs.append(root);

    // 先收集全部子目录再按层级排序：父目录先建，子目录才能拿到父目录 id。
    QVector<DirSpec> subdirs;
    QDirIterator dirIt(base, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (dirIt.hasNext()) {
        dirIt.next();
        const QString rel = QDir(base).relativeFilePath(dirIt.filePath());
        if (rel.isEmpty() || rel.startsWith(QStringLiteral(".."))) {
            continue;
        }
        DirSpec spec;
        spec.rel = rel;
        spec.name = dirIt.fileName();
        spec.parentRel = parentRelOf(rel);
        subdirs.append(spec);
    }
    std::sort(subdirs.begin(), subdirs.end(), [](const DirSpec& a, const DirSpec& b) {
        const int depthA = a.rel.count(QLatin1Char('/'));
        const int depthB = b.rel.count(QLatin1Char('/'));
        return depthA == depthB ? a.rel < b.rel : depthA < depthB;
    });
    batch.dirs += subdirs;

    QDirIterator fileIt(base, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (fileIt.hasNext()) {
        fileIt.next();
        const QString rel = QDir(base).relativeFilePath(fileIt.filePath());
        batch.files.append(qMakePair(fileIt.filePath(), parentRelOf(rel)));
    }

    batches_.enqueue(batch);
}

void UploadController::startNext() {
    if (dirReply_ != nullptr || task_ != nullptr) {
        return;
    }
    if (!batches_.isEmpty()) {
        createNextDir();
        return;
    }
    if (!fileJobs_.isEmpty()) {
        startUpload();
        return;
    }
    clearSummary();
}

void UploadController::createNextDir() {
    if (dirReply_ != nullptr || batches_.isEmpty()) {
        return;
    }
    DirBatch& batch = batches_[0];
    if (batch.next >= batch.dirs.size()) {
        // 目录都建好了，把本批文件按所属目录入队。
        const QVector<QPair<QString, QString>> files = batch.files;
        for (const QPair<QString, QString>& file : files) {
            const qint64 parentId = batch.ids.value(file.second, -1);
            if (parentId < 0) {
                emit taskFailed(QStringLiteral("目录创建失败，已跳过：%1").arg(file.first));
                continue;
            }
            FileJob job;
            job.parentId = parentId;
            job.localPath = file.first;
            job.name = QFileInfo(file.first).fileName();
            fileJobs_.enqueue(job);
        }
        batches_.dequeue();
        startNext();
        return;
    }

    const DirSpec spec = batch.dirs.at(batch.next);
    const qint64 parentId =
        spec.rel.isEmpty() ? batch.targetId : batch.ids.value(spec.parentRel, -1);
    if (parentId < 0) {
        emit taskFailed(QStringLiteral("上级目录创建失败，已跳过：%1").arg(spec.name));
        batches_.dequeue();
        startNext();
        return;
    }

    emit summaryChanged(
        QStringLiteral("准备上传 · 创建目录 %1/%2").arg(batch.next + 1).arg(batch.dirs.size()), -1);

    QJsonObject payload;
    payload["parent_id"] = static_cast<double>(parentId);
    payload["name"] = spec.name;
    const QNetworkRequest request = api_->requestFor(QStringLiteral("/api/folders"));
    dirReply_ =
        api_->manager()->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(dirReply_, &QNetworkReply::finished, this, [this]() {
        QNetworkReply* reply = dirReply_;
        dirReply_ = nullptr;
        handleDirReply(reply);
    });
}

void UploadController::handleDirReply(QNetworkReply* reply) {
    if (reply == nullptr) {
        return;
    }
    reply->deleteLater();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray raw = reply->readAll();
    if (batches_.isEmpty()) {  // cancelAll() 之后迟到的响应
        return;
    }
    DirBatch& batch = batches_[0];
    if (status < 200 || status >= 300) {
        emit taskFailed(ApiClient::errorTextFor(status, QString::fromUtf8(raw), reply->error()));
        batches_.dequeue();
        startNext();
        return;
    }
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    batch.ids.insert(batch.dirs.at(batch.next).rel,
                     static_cast<qint64>(obj.value(QStringLiteral("id")).toDouble()));
    ++batch.next;
    createNextDir();
}

void UploadController::startUpload() {
    const FileJob job = fileJobs_.dequeue();
    percent_ = 0;
    speed_ = 0;
    task_ = new UploadTask(api_, job.parentId, job.localPath, job.name, this);

    connect(task_, &UploadTask::stateChanged, this, [this](UploadTask::State) { reportSummary(); });
    connect(task_, &UploadTask::hashProgress, this, [this](int percent) {
        percent_ = percent;
        reportSummary();
    });
    connect(task_, &UploadTask::progress, this, [this](qint64 sent, qint64 total, qint64 bytesPerSec) {
        percent_ = total > 0 ? static_cast<int>(sent * 100 / total) : 100;
        speed_ = bytesPerSec;
        reportSummary();
    });
    connect(task_, &UploadTask::interrupted, this, [this](const QString& message) {
        // 本阶段没有传输面板可重试：提示后放弃该任务，继续队列里的下一个。
        emit taskFailed(message);
        if (task_ != nullptr) {
            task_->cancel();
        }
    });
    connect(task_, &UploadTask::finished, this, &UploadController::onTaskFinished);
    task_->start();
}

void UploadController::reportSummary() {
    if (task_ == nullptr) {
        return;
    }
    const int pending = 1 + fileJobs_.size();
    const QString name = task_->remoteName();
    QString text;
    switch (task_->state()) {
        case UploadTask::Hashing:
            text = QStringLiteral("校验中 %1 · %2 %3%").arg(pending).arg(name).arg(percent_);
            break;
        case UploadTask::Running:
            text = QStringLiteral("上传中 %1 · %2 %3%").arg(pending).arg(name).arg(percent_);
            if (speed_ > 0) {
                text += QStringLiteral(" · ") + TransferUtil::formatSpeed(speed_);
            }
            break;
        case UploadTask::Verifying:
            text = QStringLiteral("上传中 %1 · %2 100%").arg(pending).arg(name);
            break;
        case UploadTask::Paused:
            text = QStringLiteral("已暂停 %1 · %2 %3%").arg(pending).arg(name).arg(percent_);
            break;
        default:
            return;
    }
    emit summaryChanged(elideSummary(text), percent_);
}

void UploadController::onTaskFinished() {
    if (task_ == nullptr) {
        return;
    }
    UploadTask* task = task_;
    task_ = nullptr;
    const UploadTask::State state = task->state();
    const qint64 fileId = task->fileId();
    const qint64 parentId = task->targetParentId();
    const QString finalName = task->finalName();
    const QString error = task->errorText();
    task->deleteLater();

    if (state == UploadTask::Completed) {
        emit taskFinished(fileId, parentId, finalName);
    } else if (state == UploadTask::Failed) {
        emit taskFailed(error.isEmpty() ? QStringLiteral("上传失败") : error);
    }
    startNext();
}

void UploadController::clearSummary() {
    emit summaryChanged(QString(), -1);
}

void UploadController::cancelAll() {
    batches_.clear();
    fileJobs_.clear();
    if (dirReply_ != nullptr) {
        QNetworkReply* reply = dirReply_;
        dirReply_ = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    if (task_ != nullptr) {
        UploadTask* task = task_;
        task_ = nullptr;
        task->disconnect(this);
        task->cancel();
        task->deleteLater();
    }
    clearSummary();
}
