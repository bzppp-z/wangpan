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
};

QTEST_MAIN(TestTransfer)
#include "test_transfer.moc"
