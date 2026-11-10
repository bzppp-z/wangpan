#pragma once

#include <QHash>
#include <QWidget>

class QLabel;
class QPoint;
class QPushButton;
class QStackedWidget;
class QTableWidget;
class QTimer;
class TransferManager;
struct TransferSnapshot;

// 展开态传输面板：头部（标题 + 清空已完成）与 7 列任务列表；空任务时显示空态。
class TransferPanel : public QWidget {
    Q_OBJECT
public:
    explicit TransferPanel(TransferManager* manager, QWidget* parent = nullptr);

private:
    void rebuild();
    void refreshRows();
    void updateRow(int row, const TransferSnapshot& snapshot);
    void showContextMenu(const QPoint& pos);

    TransferManager* manager_;
    QTableWidget* table_;
    QStackedWidget* stack_;
    QLabel* emptyLabel_;
    QPushButton* clearButton_;
    QTimer* refreshTimer_;
    QHash<QString, int> rows_;
};
