#include "hashworker.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>

HashWorker::HashWorker(const QString& filePath, QObject* parent)
    : QThread(parent), path_(filePath), canceled_(0) {}

void HashWorker::cancel() {
    canceled_.storeRelease(1);
}

void HashWorker::run() {
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) {
        emit failed(QStringLiteral("无法读取文件：%1").arg(path_));
        return;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    const qint64 total = file.size();
    qint64 readTotal = 0;
    int lastPercent = -1;
    QByteArray buffer(1024 * 1024, '\0');

    while (!canceled_.loadAcquire()) {
        const qint64 n = file.read(buffer.data(), buffer.size());
        if (n < 0) {
            emit failed(QStringLiteral("读取文件失败：%1").arg(path_));
            return;
        }
        if (n == 0) {
            break;
        }
        hash.addData(buffer.constData(), static_cast<int>(n));
        readTotal += n;
        const int percent = total > 0 ? static_cast<int>(readTotal * 100 / total) : 100;
        if (percent != lastPercent) {
            lastPercent = percent;
            emit progress(percent);
        }
    }
    if (canceled_.loadAcquire()) {
        return;
    }
    emit done(QString::fromLatin1(hash.result().toHex()));
}
