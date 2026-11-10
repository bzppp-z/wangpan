#include "transferutil.h"

namespace TransferUtil {

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

}  // namespace TransferUtil
