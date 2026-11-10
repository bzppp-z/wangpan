#pragma once

#include <QPixmap>
#include <QString>
#include <QWidget>

class ApiClient;
class QLabel;
class QNetworkReply;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QStackedWidget;

// 右侧预览面板：图片 / 文本按白名单在线查看，不落临时文件。
// 六种状态用 QStackedWidget 切页，不重建控件；空/加载/文本/图片/不支持/错误。
class PreviewPanel : public QWidget {
    Q_OBJECT
public:
    explicit PreviewPanel(ApiClient* api, QWidget* parent = nullptr);

    // 打开某个文件的预览（同一文件重复调用会重新拉取）。
    void showFile(qint64 fileId, const QString& name, qint64 size);
    // 回到空态并取消在飞请求。
    void clearPreview();
    qint64 currentFileId() const { return fileId_; }

signals:
    void closeRequested();
    // 不支持预览时用户点"下载"。
    void downloadRequested(qint64 fileId, const QString& name, qint64 size);
    // 预览时发现文件已被删除：主窗口应刷新列表。
    void fileMissing();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    enum Page { PageEmpty, PageLoading, PageText, PageImage, PageUnsupported, PageError };

    QWidget* buildHeader();
    QWidget* buildEmptyPage();
    QWidget* buildLoadingPage();
    QWidget* buildTextPage();
    QWidget* buildImagePage();
    QWidget* buildUnsupportedPage();
    QWidget* buildErrorPage();

    QWidget* buildCenteredPage(const QString& title, const QString& detail,
                               QPushButton** actionButton, const QString& actionText,
                               bool errorStyle);

    void requestPreview();
    void showPage(Page page);
    void showUnsupported();
    void showError(const QString& message);
    void applyZoom();
    void resetZoom();
    void applyHeaderElide();
    void updateHeaderSubtitle(const QString& type);

    ApiClient* api_;
    qint64 fileId_ = 0;
    QString name_;
    qint64 size_ = 0;
    QNetworkReply* reply_ = nullptr;

    QStackedWidget* stack_;
    QLabel* nameLabel_;
    QLabel* subLabel_;
    QLabel* noteLabel_;
    QLabel* zoomLabel_;
    QPlainTextEdit* text_;
    QScrollArea* imageScroll_;
    QLabel* imageLabel_;
    QLabel* errorLabel_;
    QPushButton* unsupportedDownload_;

    QPixmap pixmap_;
    int zoomPercent_ = 100;
};
