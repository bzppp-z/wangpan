#include "transferutil.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

namespace TransferUtil {

namespace {

// 远程文件名只作为建议名：分隔符与控制字符不能带进本地路径。
QString safeName(const QString& name) {
    QString out = name;
    out.replace(QLatin1Char('/'), QLatin1Char('_'));
    out.replace(QLatin1Char('\\'), QLatin1Char('_'));
    out = out.trimmed();
    if (out.isEmpty() || out == QStringLiteral(".") || out == QStringLiteral("..")) {
        return QStringLiteral("download");
    }
    return out;
}

}  // namespace

qint64 chunkCount(qint64 size, qint64 chunkSize) {
    if (size <= 0 || chunkSize <= 0) {
        return 0;
    }
    return (size + chunkSize - 1) / chunkSize;
}

qint64 chunkLength(int index, qint64 size, qint64 chunkSize) {
    if (index < 0 || size < 0 || chunkSize <= 0) {
        return 0;
    }
    const qint64 offset = static_cast<qint64>(index) * chunkSize;
    if (offset >= size) {
        return 0;
    }
    const qint64 left = size - offset;
    return left < chunkSize ? left : chunkSize;
}

QString formatSize(qint64 bytes) {
    if (bytes < 0) {
        return QStringLiteral("—");
    }
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    const double value = static_cast<double>(bytes);
    if (bytes < 1024LL * 1024) {
        return QStringLiteral("%1 KB").arg(value / 1024.0, 0, 'f', 1);
    }
    if (bytes < 1024LL * 1024 * 1024) {
        return QStringLiteral("%1 MB").arg(value / (1024.0 * 1024.0), 0, 'f', 1);
    }
    return QStringLiteral("%1 GB").arg(value / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
}

QString formatSpeed(qint64 bytesPerSec) {
    if (bytesPerSec <= 0) {
        return QStringLiteral("0 B/s");
    }
    return formatSize(bytesPerSec) + QStringLiteral("/s");
}

qint64 etaSeconds(qint64 remainingBytes, qint64 bytesPerSec) {
    if (remainingBytes <= 0 || bytesPerSec <= 0) {
        return 0;
    }
    return remainingBytes / bytesPerSec;
}

QString formatEta(qint64 seconds) {
    if (seconds <= 0) {
        return QStringLiteral("00:00");
    }
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    const qint64 secs = seconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(secs, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(secs, 2, 10, QLatin1Char('0'));
}

QString uniqueLocalPath(const QString& dir, const QString& name) {
    const QString safe = safeName(name);
    const QDir target(dir);
    const QString direct = target.filePath(safe);
    if (!QFileInfo::exists(direct)) {
        return direct;
    }

    // 拆出扩展名再编号，"a.txt" → "a (1).txt"；隐藏文件（.gitignore）整体当基名。
    QString base = safe;
    QString ext;
    const int dot = safe.lastIndexOf(QLatin1Char('.'));
    if (dot > 0) {
        base = safe.left(dot);
        ext = safe.mid(dot);
    }
    for (int i = 1; i < 1000; ++i) {
        const QString candidate =
            target.filePath(QStringLiteral("%1 (%2)%3").arg(base).arg(i).arg(ext));
        if (!QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return target.filePath(QStringLiteral("%1 (%2)%3")
                               .arg(base)
                               .arg(QDateTime::currentMSecsSinceEpoch())
                               .arg(ext));
}

}  // namespace TransferUtil
