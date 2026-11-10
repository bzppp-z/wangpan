#pragma once

#include <QDialog>
#include <QJsonArray>

class ApiClient;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTableWidget;

// 回收站：列出被软删除的子树根，支持恢复、彻底删除与清空。
class TrashDialog : public QDialog {
    Q_OBJECT
public:
    explicit TrashDialog(ApiClient* api, QWidget* parent = nullptr);

signals:
    // 恢复 / 彻底删除 / 清空成功后触发，外层据此刷新文件列表与配额。
    void changed();

private:
    void buildUi();
    void reload();
    void populate(const QJsonArray& items);
    QList<qint64> selectedIds() const;
    void updateButtons();
    void restoreSelected();
    void purgeSelected();
    void clearAll();
    void showFooterMessage(const QString& message, bool error);

    ApiClient* api_;
    QTableWidget* table_;
    QStackedWidget* stack_;
    QLabel* emptyLabel_;
    QLabel* footerHint_;
    QPushButton* clearButton_;
    QPushButton* restoreButton_;
    QPushButton* purgeButton_;
};
