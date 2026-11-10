#include "transfermanager.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <algorithm>

#include "apiclient.h"
#include "transfer/downloadtask.h"
#include "transfer/transferutil.h"
#include "transfer/uploadtask.h"

namespace {

const int kPersistIntervalMs = 500;

TransferState mapUploadState(UploadTask::State state) {
    switch (state) {
        case UploadTask::Waiting:
            return TransferState::Waiting;
        case UploadTask::Hashing:
        case UploadTask::Running:
        case UploadTask::Verifying:
            return TransferState::Running;
        case UploadTask::Paused:
            return TransferState::Paused;
        case UploadTask::Completed:
            return TransferState::Completed;
        case UploadTask::Failed:
            return TransferState::Failed;
        case UploadTask::Canceled:
            return TransferState::Canceled;
    }
    return TransferState::Waiting;
}

TransferState mapDownloadState(DownloadTask::State state) {
    switch (state) {
        case DownloadTask::Waiting:
            return TransferState::Waiting;
        case DownloadTask::Running:
        case DownloadTask::Verifying:
            return TransferState::Running;
        case DownloadTask::Paused:
            return TransferState::Paused;
        case DownloadTask::Completed:
            return TransferState::Completed;
        case DownloadTask::Failed:
            return TransferState::Failed;
        case DownloadTask::Canceled:
            return TransferState::Canceled;
    }
    return TransferState::Waiting;
}

// 相对路径所在目录；"a.bin" 这种根目录文件返回空串。
QString parentRelOf(const QString& rel) {
    const QString path = QFileInfo(rel).path();
    return path == QStringLiteral(".") ? QString() : path;
}

bool isTerminal(TransferState state) {
    return state == TransferState::Completed || state == TransferState::Failed ||
           state == TransferState::Canceled;
}

}  // namespace

TransferManager::TransferManager(ApiClient* api, QObject* parent) : QObject(parent), api_(api) {
    timer_ = new QTimer(this);
    timer_->setInterval(kPersistIntervalMs);
    connect(timer_, &QTimer::timeout, this, [this]() {
        updateSummary();
        if (dirty_) {
            savePersisted();
        }
    });
    timer_->start();
}

TransferManager::~TransferManager() {
    haltQueue_ = true;
    // 先落盘未完成任务，再断开网络；这样关闭客户端后重开还能继续。
    savePersisted();
    for (Entry& entry : entries_) {
        if (entry.up != nullptr) {
            entry.up->disconnect(this);
            entry.up->cancel();
            entry.up->deleteLater();
            entry.up = nullptr;
        }
        if (entry.down != nullptr) {
            entry.down->disconnect(this);
            entry.down->cancel();
            entry.down->deleteLater();
            entry.down = nullptr;
        }
    }
    entries_.clear();
    if (dirReply_ != nullptr) {
        dirReply_->disconnect(this);
        dirReply_->abort();
        dirReply_->deleteLater();
        dirReply_ = nullptr;
    }
}

int TransferManager::indexOf(const QString& id) const {
    for (int i = 0; i < entries_.size(); ++i) {
        if (entries_.at(i).id == id) {
            return i;
        }
    }
    return -1;
}

void TransferManager::addEntry(const Entry& entry) {
    entries_.append(entry);
    emit taskUpdated(entry.id);
    updateSummary();
}

QString TransferManager::addUpload(const QString& localPath, qint64 parentId,
                                   const QString& desiredName) {
    const QFileInfo info(localPath);
    Entry entry;
    entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.upload = true;
    entry.parentId = parentId;
    entry.localPath = info.absoluteFilePath();
    entry.name = desiredName.isEmpty() ? info.fileName() : desiredName;
    entry.size = info.size();
    entry.state = TransferState::Waiting;
    addEntry(entry);
    markDirty();
    startWaiting();
    return entry.id;
}

QString TransferManager::addDownload(qint64 remoteId, const QString& remoteName, qint64 size,
                                     const QString& savePath) {
    Entry entry;
    entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.upload = false;
    entry.remoteId = remoteId;
    entry.name = remoteName;
    entry.size = size;
    entry.savePath = savePath;
    entry.state = TransferState::Waiting;
    addEntry(entry);
    markDirty();
    startWaiting();
    return entry.id;
}

void TransferManager::addPaths(qint64 parentId, const QStringList& paths) {
    for (const QString& path : paths) {
        const QFileInfo info(path);
        if (info.isDir()) {
            enqueueFolder(parentId, path);
        } else if (info.isFile()) {
            addUpload(info.absoluteFilePath(), parentId, info.fileName());
        } else {
            emit failed(QStringLiteral("无法读取：%1").arg(path));
        }
    }
    createNextDir();
}

void TransferManager::enqueueFolder(qint64 parentId, const QString& dirPath) {
    const QDir dir(dirPath);
    if (!dir.exists()) {
        emit failed(QStringLiteral("文件夹不存在：%1").arg(dirPath));
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

    batches_.append(batch);
}

void TransferManager::createNextDir() {
    if (dirReply_ != nullptr || batches_.isEmpty() || haltQueue_) {
        return;
    }
    DirBatch& batch = batches_[0];
    if (batch.next >= batch.dirs.size()) {
        // 目录都建好了，把本批文件逐个加入上传队列。
        const QVector<QPair<QString, QString>> files = batch.files;
        for (const QPair<QString, QString>& file : files) {
            const qint64 id = batch.ids.value(file.second, -1);
            if (id < 0) {
                emit failed(QStringLiteral("目录创建失败，已跳过：%1").arg(file.first));
                continue;
            }
            addUpload(file.first, id, QFileInfo(file.first).fileName());
        }
        batches_.removeFirst();
        createNextDir();
        return;
    }

    const DirSpec spec = batch.dirs.at(batch.next);
    const qint64 parentId = spec.rel.isEmpty() ? batch.targetId : batch.ids.value(spec.parentRel, -1);
    if (parentId < 0) {
        emit failed(QStringLiteral("上级目录创建失败，已跳过：%1").arg(spec.name));
        batches_.removeFirst();
        createNextDir();
        return;
    }

    QJsonObject payload;
    payload["parent_id"] = static_cast<double>(parentId);
    payload["name"] = spec.name;
    const QNetworkRequest request = api_->requestFor(QStringLiteral("/api/folders"));
    dirReply_ = api_->manager()->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(dirReply_, &QNetworkReply::finished, this, [this]() {
        QNetworkReply* reply = dirReply_;
        dirReply_ = nullptr;
        handleDirReply(reply);
    });
}

void TransferManager::handleDirReply(QNetworkReply* reply) {
    if (reply == nullptr) {
        return;
    }
    reply->deleteLater();
    if (batches_.isEmpty()) {  // cancelAll() 之后迟到的响应
        return;
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray raw = reply->readAll();
    if (status < 200 || status >= 300) {
        emit failed(ApiClient::errorTextFor(status, QString::fromUtf8(raw), reply->error()));
        batches_.removeFirst();
        createNextDir();
        return;
    }
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    DirBatch& batch = batches_[0];
    batch.ids.insert(batch.dirs.at(batch.next).rel,
                     static_cast<qint64>(obj.value(QStringLiteral("id")).toDouble()));
    ++batch.next;
    createNextDir();
}

void TransferManager::attachUpload(int index) {
    const QString id = entries_.at(index).id;
    UploadTask* task = new UploadTask(api_, entries_.at(index).parentId,
                                      entries_.at(index).localPath, entries_.at(index).name, this);
    entries_[index].up = task;

    connect(task, &UploadTask::stateChanged, this, [this, id](UploadTask::State state) {
        const int i = indexOf(id);
        if (i < 0) {
            return;
        }
        entries_[i].state = mapUploadState(state);
        if (state == UploadTask::Failed && entries_.at(i).up != nullptr) {
            entries_[i].error = entries_.at(i).up->errorText();
        }
        emit taskUpdated(id);
        updateSummary();
        markDirty();
    });
    connect(task, &UploadTask::progress, this,
            [this, id](qint64 sent, qint64 total, qint64 bytesPerSec) {
                const int i = indexOf(id);
                if (i < 0) {
                    return;
                }
                entries_[i].done = sent;
                entries_[i].size = total;
                entries_[i].speed = bytesPerSec;
            });
    connect(task, &UploadTask::interrupted, this,
            [this](const QString& message) { emit failed(message); });
    connect(task, &UploadTask::finished, this, [this, id]() { onTaskFinished(id); });
}

void TransferManager::attachDownload(int index) {
    const QString id = entries_.at(index).id;
    DownloadTask* task = new DownloadTask(api_, entries_.at(index).remoteId,
                                          entries_.at(index).name, entries_.at(index).size,
                                          entries_.at(index).savePath, this);
    entries_[index].down = task;

    connect(task, &DownloadTask::stateChanged, this, [this, id](DownloadTask::State state) {
        const int i = indexOf(id);
        if (i < 0) {
            return;
        }
        entries_[i].state = mapDownloadState(state);
        if (state == DownloadTask::Failed && entries_.at(i).down != nullptr) {
            entries_[i].error = entries_.at(i).down->errorText();
        }
        emit taskUpdated(id);
        updateSummary();
        markDirty();
    });
    connect(task, &DownloadTask::progress, this,
            [this, id](qint64 received, qint64 total, qint64 bytesPerSec) {
                const int i = indexOf(id);
                if (i < 0) {
                    return;
                }
                entries_[i].done = received;
                entries_[i].size = total;
                entries_[i].speed = bytesPerSec;
            });
    connect(task, &DownloadTask::interrupted, this,
            [this](const QString& message) { emit failed(message); });
    connect(task, &DownloadTask::finished, this, [this, id]() { onTaskFinished(id); });
}

void TransferManager::startEntry(int index) {
    if (index < 0 || index >= entries_.size()) {
        return;
    }
    entries_[index].state = TransferState::Running;
    entries_[index].restored = false;
    entries_[index].error.clear();
    entries_[index].speed = 0;
    emit taskUpdated(entries_.at(index).id);

    if (entries_.at(index).upload) {
        attachUpload(index);
        UploadTask* task = entries_.at(index).up;
        if (task != nullptr) {
            task->start();  // 可能同步失败，onTaskFinished 会清空 up
        }
    } else {
        attachDownload(index);
        DownloadTask* task = entries_.at(index).down;
        if (task != nullptr) {
            task->start();
        }
    }
    updateSummary();
    markDirty();
}

void TransferManager::startWaiting() {
    if (haltQueue_) {
        return;
    }
    while (runningCount() < kMaxConcurrent) {
        int pick = -1;
        for (int i = 0; i < entries_.size(); ++i) {
            const Entry& entry = entries_.at(i);
            if (entry.state == TransferState::Waiting && !entry.restored) {
                pick = i;
                break;
            }
        }
        if (pick < 0) {
            break;
        }
        startEntry(pick);
    }
}

int TransferManager::runningCount() const {
    int count = 0;
    for (const Entry& entry : entries_) {
        if (entry.state == TransferState::Running) {
            ++count;
        }
    }
    return count;
}

void TransferManager::onTaskFinished(const QString& id) {
    const int index = indexOf(id);
    if (index < 0) {
        return;
    }
    Entry& entry = entries_[index];
    if (entry.state == TransferState::Completed) {
        entry.done = entry.size;
        entry.speed = 0;
        if (entry.upload) {
            const qint64 fileId = entry.up != nullptr ? entry.up->fileId() : 0;
            const QString finalName = entry.up != nullptr ? entry.up->finalName() : entry.name;
            entry.name = finalName;
            destroyTask(entry);
            emit taskUpdated(id);
            updateSummary();
            markDirty();
            emit uploadCompleted(fileId, entry.parentId, finalName);
        } else {
            const QString savePath = entry.down != nullptr ? entry.down->savePath() : entry.savePath;
            entry.savePath = savePath;
            destroyTask(entry);
            emit taskUpdated(id);
            updateSummary();
            markDirty();
            emit downloadCompleted(savePath);
        }
    } else {
        if (entry.state == TransferState::Failed) {
            entry.error = entry.up != nullptr ? entry.up->errorText()
                                              : (entry.down != nullptr ? entry.down->errorText()
                                                                       : entry.error);
        }
        entry.speed = 0;
        destroyTask(entry);
        emit taskUpdated(id);
        updateSummary();
        markDirty();
        if (entry.state == TransferState::Failed && !entry.error.isEmpty()) {
            emit failed(entry.error);
        }
    }
    startWaiting();
}

void TransferManager::destroyTask(Entry& entry) {
    if (entry.up != nullptr) {
        entry.up->deleteLater();
        entry.up = nullptr;
    }
    if (entry.down != nullptr) {
        entry.down->deleteLater();
        entry.down = nullptr;
    }
}

void TransferManager::pause(const QString& id) {
    const int index = indexOf(id);
    if (index < 0) {
        return;
    }
    Entry& entry = entries_[index];
    if (entry.state == TransferState::Running) {
        if (entry.up != nullptr) {
            entry.up->pause();
        } else if (entry.down != nullptr) {
            entry.down->pause();
        }
        return;
    }
    if (entry.state == TransferState::Waiting) {
        // 还没开始的任务直接标记暂停，让出并发位给后面的任务。
        entry.state = TransferState::Paused;
        emit taskUpdated(id);
        updateSummary();
        markDirty();
    }
}

void TransferManager::resume(const QString& id) {
    const int index = indexOf(id);
    if (index < 0) {
        return;
    }
    Entry& entry = entries_[index];
    const bool restoredWaiting = entry.state == TransferState::Waiting && entry.restored;
    if (entry.state != TransferState::Paused && !restoredWaiting) {
        return;
    }
    entry.restored = false;
    if (entry.up != nullptr || entry.down != nullptr) {
        if (runningCount() >= kMaxConcurrent) {
            emit failed(QStringLiteral("最多同时进行 %1 个传输任务，请先暂停或等待其它任务")
                            .arg(kMaxConcurrent));
            return;
        }
        if (entry.up != nullptr) {
            entry.up->resume();
        } else {
            entry.down->resume();
        }
        return;
    }
    // 恢复后从未启动过：重新排队。
    entry.state = TransferState::Waiting;
    emit taskUpdated(id);
    startWaiting();
}

void TransferManager::cancel(const QString& id) {
    const int index = indexOf(id);
    if (index < 0) {
        return;
    }
    Entry& entry = entries_[index];
    if (isTerminal(entry.state)) {
        return;
    }
    if (entry.up != nullptr) {
        entry.up->cancel();  // Canceled → finished → onTaskFinished 收尾
        return;
    }
    if (entry.down != nullptr) {
        entry.down->cancel();
        return;
    }
    entry.state = TransferState::Canceled;
    emit taskUpdated(id);
    updateSummary();
    markDirty();
    startWaiting();
}

void TransferManager::retry(const QString& id) {
    const int index = indexOf(id);
    if (index < 0) {
        return;
    }
    Entry& entry = entries_[index];
    if (entry.state != TransferState::Failed && entry.state != TransferState::Canceled) {
        return;
    }
    destroyTask(entry);
    entry.state = TransferState::Waiting;
    entry.restored = false;
    entry.error.clear();
    entry.done = 0;
    entry.speed = 0;
    if (entry.upload) {
        entry.size = QFileInfo(entry.localPath).size();
    }
    emit taskUpdated(id);
    startWaiting();
    updateSummary();
    markDirty();
}

void TransferManager::clearCompleted() {
    for (int i = entries_.size() - 1; i >= 0; --i) {
        const TransferState state = entries_.at(i).state;
        if (state == TransferState::Completed || state == TransferState::Canceled) {
            destroyTask(entries_[i]);
            entries_.removeAt(i);
        }
    }
    updateSummary();
    markDirty();
}

void TransferManager::cancelAll() {
    haltQueue_ = true;
    for (Entry& entry : entries_) {
        if (entry.up != nullptr) {
            entry.up->disconnect(this);
            entry.up->cancel();
            entry.up->deleteLater();
            entry.up = nullptr;
        }
        if (entry.down != nullptr) {
            entry.down->disconnect(this);
            entry.down->cancel();
            entry.down->deleteLater();
            entry.down = nullptr;
        }
    }
    entries_.clear();
    batches_.clear();
    if (dirReply_ != nullptr) {
        dirReply_->disconnect(this);
        dirReply_->abort();
        dirReply_->deleteLater();
        dirReply_ = nullptr;
    }
    QFile::remove(persistPath());
    dirty_ = false;
    lastSummary_.clear();
    lastActive_ = false;
    emit summaryChanged(QString(), false);
}

QVector<TransferSnapshot> TransferManager::snapshots() const {
    QVector<TransferSnapshot> result;
    result.reserve(entries_.size());
    for (const Entry& entry : entries_) {
        TransferSnapshot snapshot;
        snapshot.id = entry.id;
        snapshot.upload = entry.upload;
        snapshot.name = entry.name;
        snapshot.size = entry.size;
        snapshot.done = entry.done;
        snapshot.speed = entry.speed;
        snapshot.state = entry.state;
        snapshot.error = entry.error;
        snapshot.localPath = entry.localPath;
        snapshot.savePath = entry.savePath;
        snapshot.restored = entry.restored;
        result.append(snapshot);
    }
    return result;
}

bool TransferManager::isEmpty() const {
    return entries_.isEmpty();
}

void TransferManager::updateSummary() {
    int running = 0;
    int waiting = 0;
    int paused = 0;
    int failedCount = 0;
    int completed = 0;
    int canceled = 0;
    qint64 speed = 0;
    for (const Entry& entry : entries_) {
        switch (entry.state) {
            case TransferState::Running:
                ++running;
                speed += entry.speed;
                break;
            case TransferState::Waiting:
                ++waiting;
                break;
            case TransferState::Paused:
                ++paused;
                break;
            case TransferState::Failed:
                ++failedCount;
                break;
            case TransferState::Completed:
                ++completed;
                break;
            case TransferState::Canceled:
                ++canceled;
                break;
        }
    }

    QString text;
    if (!entries_.isEmpty()) {
        if (running > 0) {
            text = QStringLiteral("传输：%1 进行中").arg(running);
            if (waiting > 0) {
                text += QStringLiteral(" · %1 等待").arg(waiting);
            }
            if (speed > 0) {
                text += QStringLiteral(" · ") + TransferUtil::formatSpeed(speed);
            }
        } else if (waiting > 0) {
            text = QStringLiteral("传输：%1 等待").arg(waiting);
        } else {
            QStringList parts;
            if (paused > 0) {
                parts << QStringLiteral("%1 已暂停").arg(paused);
            }
            if (failedCount > 0) {
                parts << QStringLiteral("%1 失败").arg(failedCount);
            }
            if (completed > 0) {
                parts << QStringLiteral("%1 已完成").arg(completed);
            }
            if (canceled > 0) {
                parts << QStringLiteral("%1 已取消").arg(canceled);
            }
            if (parts.isEmpty()) {
                return;
            }
            text = QStringLiteral("传输：") + parts.join(QStringLiteral(" · "));
        }
    }
    const bool anyActive = running > 0 || waiting > 0;
    if (text == lastSummary_ && anyActive == lastActive_) {
        return;
    }
    lastSummary_ = text;
    lastActive_ = anyActive;
    emit summaryChanged(text, anyActive);
}

void TransferManager::markDirty() {
    dirty_ = true;
}

QString TransferManager::persistPath() const {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dir + QStringLiteral("/transfers.json");
}

void TransferManager::savePersisted() {
    QJsonArray items;
    for (const Entry& entry : entries_) {
        if (isTerminal(entry.state)) {
            continue;
        }
        QJsonObject obj;
        if (entry.upload) {
            obj["type"] = QStringLiteral("upload");
            obj["localPath"] = entry.localPath;
            obj["parentId"] = static_cast<double>(entry.parentId);
            obj["name"] = entry.name;
            obj["size"] = static_cast<double>(entry.size);
            obj["sha256"] = entry.up != nullptr ? entry.up->sha256() : QString();
        } else {
            obj["type"] = QStringLiteral("download");
            obj["remoteId"] = static_cast<double>(entry.remoteId);
            obj["name"] = entry.name;
            obj["savePath"] = entry.savePath;
            obj["size"] = static_cast<double>(entry.size);
        }
        items.append(obj);
    }

    QJsonObject root;
    root["version"] = 1;
    root["items"] = items;

    const QString path = persistPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.close();
    dirty_ = false;
}

void TransferManager::restorePersisted() {
    QFile file(persistPath());
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();

    const QJsonArray items = root.value(QStringLiteral("items")).toArray();
    for (const QJsonValue& value : items) {
        const QJsonObject obj = value.toObject();
        const QString type = obj.value(QStringLiteral("type")).toString();
        Entry entry;
        entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        entry.restored = true;
        entry.state = TransferState::Waiting;

        if (type == QStringLiteral("upload")) {
            entry.upload = true;
            entry.localPath = obj.value(QStringLiteral("localPath")).toString();
            entry.parentId = static_cast<qint64>(obj.value(QStringLiteral("parentId")).toDouble());
            entry.name = obj.value(QStringLiteral("name")).toString();
            entry.size = static_cast<qint64>(obj.value(QStringLiteral("size")).toDouble());
            if (!QFileInfo(entry.localPath).isFile()) {
                entry.state = TransferState::Failed;
                entry.error = QStringLiteral("源文件不存在");
            } else if (entry.size <= 0) {
                entry.size = QFileInfo(entry.localPath).size();
            }
        } else if (type == QStringLiteral("download")) {
            entry.upload = false;
            entry.remoteId = static_cast<qint64>(obj.value(QStringLiteral("remoteId")).toDouble());
            entry.name = obj.value(QStringLiteral("name")).toString();
            entry.savePath = obj.value(QStringLiteral("savePath")).toString();
            entry.size = static_cast<qint64>(obj.value(QStringLiteral("size")).toDouble());
            const QFileInfo part(entry.savePath + QStringLiteral(".part"));
            if (part.exists()) {
                entry.done = qMin(part.size(), entry.size);
            }
        } else {
            continue;
        }

        addEntry(entry);
        if (entry.state == TransferState::Failed) {
            emit failed(QStringLiteral("源文件不存在：%1").arg(entry.name));
        }
    }
    updateSummary();
}
