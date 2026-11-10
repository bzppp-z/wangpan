#include "ui/previewpanel.h"

#include <QByteArray>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "apiclient.h"
#include "ui/theme.h"

namespace {

QString formatSize(qint64 bytes) {
    const char* const units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    return QStringLiteral("%1 %2").arg(value, 0, 'f', 1).arg(QLatin1String(units[unit]));
}

}  // namespace

PreviewPanel::PreviewPanel(ApiClient* api, QWidget* parent)
    : QWidget(parent),
      api_(api),
      stack_(nullptr),
      nameLabel_(nullptr),
      subLabel_(nullptr),
      noteLabel_(nullptr),
      zoomLabel_(nullptr),
      text_(nullptr),
      imageScroll_(nullptr),
      imageLabel_(nullptr),
      errorLabel_(nullptr),
      unsupportedDownload_(nullptr) {
    setObjectName(QStringLiteral("previewPanel"));
    setMinimumWidth(280);
    setStyleSheet(QStringLiteral("#previewPanel { background:%1; border-left:1px solid %2; }")
                      .arg(theme::kPanelBg.name())
                      .arg(theme::kPanelBorder.name()));

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(buildHeader());

    stack_ = new QStackedWidget(this);
    stack_->addWidget(buildEmptyPage());        // PageEmpty
    stack_->addWidget(buildLoadingPage());      // PageLoading
    stack_->addWidget(buildTextPage());         // PageText
    stack_->addWidget(buildImagePage());        // PageImage
    stack_->addWidget(buildUnsupportedPage());  // PageUnsupported
    stack_->addWidget(buildErrorPage());        // PageError
    layout->addWidget(stack_, 1);

    clearPreview();
}

QWidget* PreviewPanel::buildHeader() {
    QWidget* header = new QWidget(this);
    header->setFixedHeight(44);
    header->setStyleSheet(QStringLiteral("background:#FFFFFF; border-bottom:1px solid %1;")
                              .arg(theme::kPanelBorder.name()));
    QHBoxLayout* layout = new QHBoxLayout(header);
    layout->setContentsMargins(12, 6, 8, 6);
    layout->setSpacing(8);

    QWidget* meta = new QWidget(header);
    QVBoxLayout* metaLayout = new QVBoxLayout(meta);
    metaLayout->setContentsMargins(0, 0, 0, 0);
    metaLayout->setSpacing(2);

    nameLabel_ = new QLabel(meta);
    nameLabel_->setStyleSheet(QStringLiteral("color:%1; font-size:12px;").arg(theme::kText.name()));
    subLabel_ = new QLabel(meta);
    subLabel_->setStyleSheet(QStringLiteral("color:%1; font-size:12px;").arg(theme::kTextMuted.name()));
    metaLayout->addWidget(nameLabel_);
    metaLayout->addWidget(subLabel_);
    layout->addWidget(meta, 1);

    zoomLabel_ = new QLabel(header);
    zoomLabel_->setStyleSheet(QStringLiteral("color:%1; font-size:12px;").arg(theme::kTextMuted.name()));
    zoomLabel_->hide();
    layout->addWidget(zoomLabel_);

    QPushButton* close = new QPushButton(QStringLiteral("×"), header);
    close->setFixedSize(24, 24);
    close->setCursor(Qt::PointingHandCursor);
    close->setStyleSheet(
        QStringLiteral("QPushButton { border:none; background:transparent; color:%1; font-size:16px; }"
                       "QPushButton:hover { background:%2; border-radius:4px; color:%3; }")
            .arg(theme::kTextMuted.name())
            .arg(theme::kPanelBorder.name())
            .arg(theme::kText.name()));
    connect(close, &QPushButton::clicked, this, &PreviewPanel::closeRequested);
    layout->addWidget(close);

    return header;
}

QWidget* PreviewPanel::buildCenteredPage(const QString& title, const QString& detail,
                                         QPushButton** actionButton, const QString& actionText,
                                         bool errorStyle) {
    QWidget* page = new QWidget(this);
    QVBoxLayout* layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(6);
    layout->addStretch(1);

    QLabel* titleLabel = new QLabel(title, page);
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setWordWrap(true);
    titleLabel->setStyleSheet(QStringLiteral("color:%1; font-size:13px;")
                                  .arg(errorStyle ? theme::kErrorText.name() : theme::kText.name()));
    layout->addWidget(titleLabel);

    QLabel* detailLabel = new QLabel(detail, page);
    detailLabel->setAlignment(Qt::AlignCenter);
    detailLabel->setWordWrap(true);
    detailLabel->setStyleSheet(
        QStringLiteral("color:%1; font-size:12px;").arg(theme::kTextMuted.name()));
    layout->addWidget(detailLabel);

    if (actionButton != nullptr) {
        QPushButton* button = new QPushButton(actionText, page);
        button->setFixedHeight(28);
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(
            QStringLiteral("QPushButton { background:#FFFFFF; border:1px solid %1; border-radius:4px;"
                           " color:%2; font-size:12px; padding:0 14px; }"
                           "QPushButton:hover { border-color:%3; color:%3; }")
                .arg(theme::kBorder.name())
                .arg(theme::kText.name())
                .arg(theme::kPrimary.name()));
        QHBoxLayout* buttonRow = new QHBoxLayout();
        buttonRow->addStretch(1);
        buttonRow->addWidget(button);
        buttonRow->addStretch(1);
        layout->addSpacing(6);
        layout->addLayout(buttonRow);
        *actionButton = button;
    }

    layout->addStretch(1);
    if (errorStyle) {
        errorLabel_ = titleLabel;
    }
    return page;
}

QWidget* PreviewPanel::buildEmptyPage() {
    return buildCenteredPage(QStringLiteral("双击文件可预览"),
                             QStringLiteral("支持 图片 / 文本"), nullptr, QString(), false);
}

QWidget* PreviewPanel::buildLoadingPage() {
    return buildCenteredPage(QStringLiteral("加载中…"), QString(), nullptr, QString(), false);
}

QWidget* PreviewPanel::buildTextPage() {
    QWidget* page = new QWidget(this);
    QVBoxLayout* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    noteLabel_ = new QLabel(QStringLiteral("文件过大，仅显示前 1 MB"), page);
    noteLabel_->setStyleSheet(
        QStringLiteral("background:#FFF7E8; color:%1; font-size:12px; padding:6px 12px;")
            .arg(theme::kTextMuted.name()));
    noteLabel_->hide();
    layout->addWidget(noteLabel_);

    text_ = new QPlainTextEdit(page);
    text_->setReadOnly(true);
    text_->setLineWrapMode(QPlainTextEdit::NoWrap);
    text_->setStyleSheet(QStringLiteral("QPlainTextEdit { background:#FFFFFF; border:none;"
                                        " color:%1; padding:8px 10px; }")
                             .arg(theme::kText.name()));
    QFont mono(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSizeF(9.5);  // 约 12px
    text_->setFont(mono);
    layout->addWidget(text_, 1);
    return page;
}

QWidget* PreviewPanel::buildImagePage() {
    imageScroll_ = new QScrollArea(this);
    imageScroll_->setFrameShape(QFrame::NoFrame);
    imageScroll_->setWidgetResizable(false);
    imageScroll_->setAlignment(Qt::AlignCenter);
    imageScroll_->setStyleSheet(QStringLiteral("QScrollArea { background:%1; border:none; }")
                                    .arg(theme::kPanelBg.name()));

    imageLabel_ = new QLabel();
    imageLabel_->setAlignment(Qt::AlignCenter);
    imageScroll_->setWidget(imageLabel_);
    imageScroll_->viewport()->installEventFilter(this);
    return imageScroll_;
}

QWidget* PreviewPanel::buildUnsupportedPage() {
    QWidget* page = buildCenteredPage(QStringLiteral("暂不支持预览"), QStringLiteral("请下载后查看"),
                                      &unsupportedDownload_, QStringLiteral("下载"), false);
    connect(unsupportedDownload_, &QPushButton::clicked, this, [this]() {
        if (fileId_ != 0) {
            emit downloadRequested(fileId_, name_, size_);
        }
    });
    return page;
}

QWidget* PreviewPanel::buildErrorPage() {
    QWidget* page = buildCenteredPage(QStringLiteral("加载失败"), QStringLiteral("请稍后重试"),
                                      nullptr, QString(), true);
    // 错误页的"重试"按钮单独创建，点击后按当前 fileId 重新请求。
    QPushButton* retry = new QPushButton(QStringLiteral("重试"), page);
    retry->setFixedHeight(28);
    retry->setCursor(Qt::PointingHandCursor);
    retry->setStyleSheet(
        QStringLiteral("QPushButton { background:#FFFFFF; border:1px solid %1; border-radius:4px;"
                       " color:%2; font-size:12px; padding:0 14px; }"
                       "QPushButton:hover { border-color:%3; color:%3; }")
            .arg(theme::kBorder.name())
            .arg(theme::kText.name())
            .arg(theme::kPrimary.name()));
    QVBoxLayout* layout = qobject_cast<QVBoxLayout*>(page->layout());
    if (layout != nullptr) {
        QHBoxLayout* row = new QHBoxLayout();
        row->addStretch(1);
        row->addWidget(retry);
        row->addStretch(1);
        layout->insertLayout(layout->count() - 1, row);
    }
    connect(retry, &QPushButton::clicked, this, [this]() {
        if (fileId_ == 0) {
            clearPreview();
            return;
        }
        showPage(PageLoading);
        requestPreview();
    });
    return page;
}

void PreviewPanel::showFile(qint64 fileId, const QString& name, qint64 size) {
    fileId_ = fileId;
    name_ = name;
    size_ = size;
    nameLabel_->setText(name_);
    applyHeaderElide();
    zoomLabel_->hide();
    noteLabel_->hide();
    updateHeaderSubtitle(QString());
    showPage(PageLoading);
    requestPreview();
}

void PreviewPanel::clearPreview() {
    if (reply_ != nullptr) {
        reply_->disconnect(this);
        reply_->abort();
        reply_->deleteLater();
        reply_ = nullptr;
    }
    fileId_ = 0;
    name_.clear();
    size_ = 0;
    pixmap_ = QPixmap();
    zoomPercent_ = 100;
    nameLabel_->clear();
    subLabel_->clear();
    zoomLabel_->hide();
    noteLabel_->hide();
    showPage(PageEmpty);
}

void PreviewPanel::requestPreview() {
    if (reply_ != nullptr) {
        reply_->disconnect(this);
        reply_->abort();
        reply_->deleteLater();
        reply_ = nullptr;
    }
    const QNetworkRequest request =
        api_->requestFor(QStringLiteral("/api/files/%1/preview").arg(fileId_));
    reply_ = api_->manager()->get(request);
    connect(reply_, &QNetworkReply::finished, this, [this]() {
        QNetworkReply* reply = reply_;
        reply_ = nullptr;
        if (reply == nullptr) {
            return;
        }
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (status == 200) {
            const QByteArray body = reply->readAll();
            const QString contentType = QString::fromUtf8(reply->rawHeader("Content-Type"));
            const bool truncated = reply->rawHeader("X-Truncated") == QByteArray("1");
            if (contentType.startsWith(QStringLiteral("image/"))) {
                QPixmap pixmap;
                if (!pixmap.loadFromData(body)) {
                    showUnsupported();  // 半张图不如明确说不支持
                    return;
                }
                pixmap_ = pixmap;
                zoomPercent_ = 100;
                zoomLabel_->setText(QStringLiteral("100%"));
                zoomLabel_->show();
                updateHeaderSubtitle(QStringLiteral("图片"));
                showPage(PageImage);
                applyZoom();
                return;
            }
            if (contentType.startsWith(QStringLiteral("text/"))) {
                text_->setPlainText(QString::fromUtf8(body));
                noteLabel_->setVisible(truncated);
                updateHeaderSubtitle(QStringLiteral("文本"));
                showPage(PageText);
                return;
            }
            showUnsupported();
            return;
        }

        if (status == 415) {
            showUnsupported();
            return;
        }
        if (status == 404) {
            showError(QStringLiteral("文件已被删除"));
            emit fileMissing();
            return;
        }
        if (status == 401) {
            showError(QStringLiteral("登录已过期，请重新登录"));
            return;
        }
        showError(ApiClient::errorTextFor(status, QString::fromUtf8(reply->readAll()),
                                          reply->error()));
    });
}

void PreviewPanel::showPage(Page page) {
    stack_->setCurrentIndex(static_cast<int>(page));
}

void PreviewPanel::showUnsupported() {
    updateHeaderSubtitle(QStringLiteral("不支持预览"));
    showPage(PageUnsupported);
}

void PreviewPanel::showError(const QString& message) {
    if (errorLabel_ != nullptr) {
        errorLabel_->setText(message);
    }
    showPage(PageError);
}

void PreviewPanel::updateHeaderSubtitle(const QString& type) {
    QString text = formatSize(size_);
    if (!type.isEmpty()) {
        text += QStringLiteral(" · ") + type;
    }
    subLabel_->setText(text);
}

void PreviewPanel::applyZoom() {
    if (pixmap_.isNull() || imageScroll_ == nullptr) {
        return;
    }
    const QSize viewport = imageScroll_->viewport()->size() - QSize(12, 12);
    if (viewport.width() <= 0 || viewport.height() <= 0) {
        return;
    }
    const double fitW = static_cast<double>(viewport.width()) / pixmap_.width();
    const double fitH = static_cast<double>(viewport.height()) / pixmap_.height();
    const double fit = fitW < fitH ? fitW : fitH;
    const double scale = fit * zoomPercent_ / 100.0;
    const QSize target(qMax(1, static_cast<int>(pixmap_.width() * scale)),
                       qMax(1, static_cast<int>(pixmap_.height() * scale)));
    imageLabel_->setPixmap(pixmap_.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    imageLabel_->resize(imageLabel_->pixmap()->size());
    zoomLabel_->setText(QStringLiteral("%1%").arg(zoomPercent_));
}

void PreviewPanel::resetZoom() {
    zoomPercent_ = 100;
    applyZoom();
}

bool PreviewPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == imageScroll_->viewport() && stack_->currentIndex() == PageImage) {
        if (event->type() == QEvent::Wheel) {
            QWheelEvent* wheel = static_cast<QWheelEvent*>(event);
            const int delta = wheel->angleDelta().y();
            if (delta == 0) {
                return false;
            }
            const int next = qBound(10, zoomPercent_ + (delta > 0 ? 10 : -10), 500);
            if (next == zoomPercent_) {
                return true;
            }
            const QPoint pos = wheel->pos();
            const double ratio = static_cast<double>(next) / zoomPercent_;
            zoomPercent_ = next;
            applyZoom();
            // 以鼠标位置为中心缩放：滚动条按比例换算。
            QScrollBar* h = imageScroll_->horizontalScrollBar();
            QScrollBar* v = imageScroll_->verticalScrollBar();
            h->setValue(static_cast<int>((h->value() + pos.x()) * ratio - pos.x()));
            v->setValue(static_cast<int>((v->value() + pos.y()) * ratio - pos.y()));
            return true;
        }
        if (event->type() == QEvent::MouseButtonDblClick) {
            resetZoom();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void PreviewPanel::applyHeaderElide() {
    if (nameLabel_ == nullptr) {
        return;
    }
    const int reserve = 24 + 8 + 8 + (zoomLabel_->isVisible() ? 44 : 0);
    const int available = qMax(40, width() - reserve - 16);
    nameLabel_->setText(nameLabel_->fontMetrics().elidedText(name_, Qt::ElideMiddle, available));
}

void PreviewPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    applyHeaderElide();
    if (stack_ != nullptr && stack_->currentIndex() == PageImage) {
        applyZoom();
    }
}
