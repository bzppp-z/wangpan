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

}  // namespace TransferUtil
