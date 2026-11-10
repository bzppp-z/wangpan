#pragma once

#include <QString>
#include <QtGlobal>

// 上传相关的纯函数：分片计算与展示格式。不依赖网络、文件与界面，便于单元测试。
namespace TransferUtil {

// ceil(size / chunkSize)；空文件为 0 片。
qint64 chunkCount(qint64 size, qint64 chunkSize);

// 第 index 片的长度；越界或参数非法返回 0。
qint64 chunkLength(int index, qint64 size, qint64 chunkSize);

// 1024 进制，保留一位小数：0 B / 1.5 KB / 5.0 MB / 1.0 GB。
QString formatSize(qint64 bytes);

// 速度展示：formatSize(bytesPerSec) + "/s"。
QString formatSpeed(qint64 bytesPerSec);

// 预计剩余秒数；速度为 0 时返回 0。
qint64 etaSeconds(qint64 remainingBytes, qint64 bytesPerSec);

// 剩余时间展示：90 → "01:30"，3600 → "1:00:00"，0 或负数 → "00:00"。
QString formatEta(qint64 seconds);

// 在 dir 下为 name 找一个不覆盖已有文件的路径：重名依次追加 " (1)"、" (2)"。
// name 中的路径分隔符会被替换成下划线，避免远程文件名把文件写到目录外。
QString uniqueLocalPath(const QString& dir, const QString& name);

}  // namespace TransferUtil
