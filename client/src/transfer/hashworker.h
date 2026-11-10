#pragma once

#include <QAtomicInt>
#include <QString>
#include <QThread>

// 在后台线程计算文件 SHA-256：1 MiB 分块读取，界面不会被大文件卡住。
// 自己持有的 QThread 子类，只在 start()/cancel() 上被主线程调用。
class HashWorker : public QThread {
    Q_OBJECT
public:
    explicit HashWorker(const QString& filePath, QObject* parent = nullptr);

    // 请求尽快停止；run() 在下一个分块边界退出，不发射 done。
    void cancel();

signals:
    void progress(int percent);
    void done(const QString& sha256);
    void failed(const QString& message);

protected:
    void run() override;

private:
    QString path_;
    QAtomicInt canceled_;
};
