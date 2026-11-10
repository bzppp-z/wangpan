#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include "transferutil.h"

class TestTransfer : public QObject {
    Q_OBJECT
private slots:
    void countsChunks() {
        QCOMPARE(TransferUtil::chunkCount(0, 4194304), qint64(0));
        QCOMPARE(TransferUtil::chunkCount(1, 4194304), qint64(1));
        QCOMPARE(TransferUtil::chunkCount(4194304, 4194304), qint64(1));
        QCOMPARE(TransferUtil::chunkCount(4194305, 4194304), qint64(2));
    }
    void computesChunkLength() {
        QCOMPARE(TransferUtil::chunkLength(0, 5242880, 4194304), qint64(4194304));
        QCOMPARE(TransferUtil::chunkLength(1, 5242880, 4194304), qint64(1048576));
    }
    void formatsSize() {
        QCOMPARE(TransferUtil::formatSize(0), QString("0 B"));
        QCOMPARE(TransferUtil::formatSize(1536), QString("1.5 KB"));
        QCOMPARE(TransferUtil::formatSize(5242880), QString("5.0 MB"));
    }
    void computesEta() {
        QCOMPARE(TransferUtil::etaSeconds(0, 0), qint64(0));
        QCOMPARE(TransferUtil::etaSeconds(1048576, 1024), qint64(1024));
    }
    void formatEtaText() {
        QCOMPARE(TransferUtil::formatEta(0), QString("00:00"));
        QCOMPARE(TransferUtil::formatEta(90), QString("01:30"));
        QCOMPARE(TransferUtil::formatEta(3600), QString("1:00:00"));
        QCOMPARE(TransferUtil::formatEta(3725), QString("1:02:05"));
    }
    void uniqueLocalPathAvoidsOverwrite() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.path();

        // 目录里没有同名文件时直接用原名，不编号。
        QCOMPARE(TransferUtil::uniqueLocalPath(path, "a.txt"),
                 QDir(path).filePath("a.txt"));

        QFile first(QDir(path).filePath("a.txt"));
        QVERIFY(first.open(QIODevice::WriteOnly));
        first.close();
        QCOMPARE(TransferUtil::uniqueLocalPath(path, "a.txt"),
                 QDir(path).filePath("a (1).txt"));

        QFile second(QDir(path).filePath("a (1).txt"));
        QVERIFY(second.open(QIODevice::WriteOnly));
        second.close();
        QCOMPARE(TransferUtil::uniqueLocalPath(path, "a.txt"),
                 QDir(path).filePath("a (2).txt"));

        // 远程名里的目录分隔符不能把文件带出目标目录。
        QCOMPARE(TransferUtil::uniqueLocalPath(path, "../evil.txt"),
                 QDir(path).filePath(".._evil.txt"));
    }
};

QTEST_MAIN(TestTransfer)
#include "test_transfer.moc"
