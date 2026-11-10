#include "ui/loginwindow.h"

#include <QEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include "apiclient.h"
#include "ui/theme.h"

namespace {

QString inputStyle() {
    return QStringLiteral(
               "QLineEdit { height:%1px; border:1px solid %2; border-radius:4px;"
               " padding:0 12px; font-size:14px; color:%3; background:#FFFFFF; }"
               "QLineEdit:focus { border-color:%4; }")
        .arg(theme::kInputH)
        .arg(theme::kBorder.name())
        .arg(theme::kText.name())
        .arg(theme::kPrimary.name());
}

void usePlaceholderColor(QLineEdit* edit) {
    QPalette palette = edit->palette();
    palette.setColor(QPalette::PlaceholderText, theme::kTextMuted);
    edit->setPalette(palette);
}

QString linkStyle(const QColor& color) {
    return QStringLiteral(
               "QPushButton { border:none; background:transparent; font-size:13px; color:%1; }"
               "QPushButton:hover { text-decoration:underline; }"
               "QPushButton:disabled { color:%2; }")
        .arg(color.name())
        .arg(theme::kTextMuted.name());
}

// 左侧品牌区：垂直渐变 + 纯文字，不引入任何第三方素材。
class BrandPanel : public QWidget {
public:
    explicit BrandPanel(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedWidth(theme::kBrandW);

        QVBoxLayout* layout = new QVBoxLayout(this);
        layout->setContentsMargins(theme::kMargin, 0, 0, 0);
        layout->addStretch();

        QLabel* title = new QLabel(QStringLiteral("网盘"), this);
        title->setStyleSheet(QStringLiteral("font-size:28px; font-weight:700; color:%1;")
                                 .arg(theme::kText.name()));
        layout->addWidget(title);
        layout->addSpacing(8);

        QLabel* subtitle = new QLabel(QStringLiteral("私有云存储 · 多用户版"), this);
        subtitle->setStyleSheet(
            QStringLiteral("font-size:13px; color:%1;").arg(theme::kTextMuted.name()));
        layout->addWidget(subtitle);
        layout->addStretch();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        QLinearGradient gradient(0, 0, 0, height());
        gradient.setColorAt(0.0, theme::kBrandFrom);
        gradient.setColorAt(1.0, theme::kBrandTo);
        painter.fillRect(rect(), gradient);
    }
};

// 密码框：容器负责边框与聚焦高亮，内部是输入框与"显示/隐藏"按钮。
class PasswordField : public QFrame {
public:
    explicit PasswordField(QWidget* parent = nullptr) : QFrame(parent) {
        setObjectName(QStringLiteral("passwordField"));
        setFixedHeight(theme::kInputH);
        setStyleSheet(
            QStringLiteral("#passwordField { background:#FFFFFF; border:1px solid %1;"
                           " border-radius:4px; }"
                           "#passwordField[focused=\"true\"] { border-color:%2; }")
                .arg(theme::kBorder.name())
                .arg(theme::kPrimary.name()));

        edit_ = new QLineEdit(this);
        edit_->setFrame(false);
        edit_->setEchoMode(QLineEdit::Password);
        edit_->setPlaceholderText(QStringLiteral("密码"));
        edit_->setStyleSheet(QStringLiteral(
                                 "QLineEdit { border:none; background:transparent; font-size:14px;"
                                 " color:%1; padding-left:8px; }")
                                 .arg(theme::kText.name()));
        usePlaceholderColor(edit_);

        toggle_ = new QToolButton(this);
        toggle_->setText(QStringLiteral("显示"));
        toggle_->setCursor(Qt::PointingHandCursor);
        toggle_->setFixedSize(32, 24);
        toggle_->setStyleSheet(QStringLiteral(
                                   "QToolButton { border:none; color:%1; font-size:12px; }"
                                   "QToolButton:hover { color:%2; }")
                                   .arg(theme::kTextMuted.name())
                                   .arg(theme::kPrimary.name()));

        QHBoxLayout* layout = new QHBoxLayout(this);
        layout->setContentsMargins(4, 0, 6, 0);
        layout->setSpacing(4);
        layout->addWidget(edit_, 1);
        layout->addWidget(toggle_, 0);

        connect(toggle_, &QToolButton::clicked, this, [this]() {
            const bool masked = edit_->echoMode() == QLineEdit::Password;
            edit_->setEchoMode(masked ? QLineEdit::Normal : QLineEdit::Password);
            toggle_->setText(masked ? QStringLiteral("隐藏") : QStringLiteral("显示"));
        });
        edit_->installEventFilter(this);
    }

    QLineEdit* edit() const { return edit_; }
    QToolButton* toggle() const { return toggle_; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == edit_ &&
            (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)) {
            setProperty("focused", event->type() == QEvent::FocusIn);
            style()->unpolish(this);
            style()->polish(this);
        }
        return QFrame::eventFilter(watched, event);
    }

private:
    QLineEdit* edit_;
    QToolButton* toggle_;
};

}  // namespace

LoginWindow::LoginWindow(ApiClient* api, QWidget* parent)
    : QDialog(parent),
      api_(api),
      mode_(LoginMode),
      busy_(false),
      tabs_(nullptr),
      loginTab_(nullptr),
      scanTab_(nullptr),
      usernameEdit_(nullptr),
      passwordField_(nullptr),
      passwordEdit_(nullptr),
      confirmRow_(nullptr),
      confirmEdit_(nullptr),
      errorBox_(nullptr),
      errorLabel_(nullptr),
      primaryButton_(nullptr),
      registerLink_(nullptr),
      serverLink_(nullptr) {
    setWindowTitle(QStringLiteral("网盘"));
    setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowCloseButtonHint |
                   Qt::WindowMinimizeButtonHint);
    setFixedSize(theme::kWindowW, theme::kWindowH);

    QHBoxLayout* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(new BrandPanel(this));
    root->addWidget(buildFormPanel(), 1);

    connect(api_, &ApiClient::loginSucceeded, this,
            [this](qint64 userId, qint64) { handleLoginSucceeded(userId); });
    connect(api_, &ApiClient::registered, this,
            [this](qint64 userId, qint64) { handleLoginSucceeded(userId); });
    connect(api_, &ApiClient::failed, this, [this](const QString& message) {
        setBusy(false);
        showError(message);
    });

    // 只恢复上次用户名与服务器地址；密码从不落盘。
    QSettings settings(QStringLiteral("Wangpan"), QStringLiteral("Client"));
    usernameEdit_->setText(settings.value(QStringLiteral("login/lastUsername")).toString());
    const QString savedUrl = settings.value(QStringLiteral("server/baseUrl")).toString();
    if (!savedUrl.isEmpty()) {
        api_->setBaseUrl(savedUrl);
    }

    if (QScreen* screen = QGuiApplication::primaryScreen()) {
        const QRect available = screen->availableGeometry();
        move(available.center() - QPoint(width() / 2, height() / 2));
    }
    usernameEdit_->setFocus();
}

QWidget* LoginWindow::buildFormPanel() {
    QWidget* panel = new QWidget(this);
    panel->setStyleSheet(QStringLiteral("background:#FFFFFF;"));

    QVBoxLayout* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(theme::kMargin, 32, theme::kMargin, 28);
    layout->setSpacing(0);

    // 顶部页签：账号登录可用，扫码登录置灰。
    tabs_ = new QWidget(panel);
    tabs_->setFixedHeight(44);
    QHBoxLayout* tabsLayout = new QHBoxLayout(tabs_);
    tabsLayout->setContentsMargins(0, 0, 0, 0);
    tabsLayout->setSpacing(32);

    loginTab_ = new QPushButton(QStringLiteral("账号登录"), tabs_);
    loginTab_->setFlat(true);
    loginTab_->setCursor(Qt::PointingHandCursor);
    loginTab_->setFixedHeight(44);
    loginTab_->setStyleSheet(
        QStringLiteral("QPushButton { border:none; border-bottom:2px solid %1; background:transparent;"
                       " font-size:15px; font-weight:600; color:%2; padding:0; }")
            .arg(theme::kPrimary.name())
            .arg(theme::kText.name()));

    scanTab_ = new QPushButton(QStringLiteral("扫码登录"), tabs_);
    scanTab_->setFlat(true);
    scanTab_->setFixedHeight(44);
    scanTab_->setEnabled(false);
    scanTab_->setToolTip(QStringLiteral("暂未开放"));
    scanTab_->setStyleSheet(
        QStringLiteral("QPushButton { border:none; border-bottom:2px solid transparent;"
                       " background:transparent; font-size:15px; color:%1; padding:0; }")
            .arg(QStringLiteral("#C9CDD4")));

    tabsLayout->addWidget(loginTab_);
    tabsLayout->addWidget(scanTab_);
    tabsLayout->addStretch();
    layout->addWidget(tabs_);
    layout->addSpacing(24);

    usernameEdit_ = new QLineEdit(panel);
    usernameEdit_->setPlaceholderText(QStringLiteral("用户名"));
    usernameEdit_->setFixedSize(theme::kInputW, theme::kInputH);
    usernameEdit_->setStyleSheet(inputStyle());
    usePlaceholderColor(usernameEdit_);
    layout->addWidget(usernameEdit_);
    layout->addSpacing(16);

    PasswordField* passwordField = new PasswordField(panel);
    passwordField->setFixedWidth(theme::kInputW);
    passwordField_ = passwordField;
    passwordEdit_ = passwordField->edit();
    layout->addWidget(passwordField_);

    // 确认密码：注册态才显示；整行隐藏时不留空隙。
    confirmRow_ = new QWidget(panel);
    confirmRow_->setFixedHeight(56);
    QVBoxLayout* confirmLayout = new QVBoxLayout(confirmRow_);
    confirmLayout->setContentsMargins(0, 16, 0, 0);
    confirmLayout->setSpacing(0);
    confirmEdit_ = new QLineEdit(confirmRow_);
    confirmEdit_->setPlaceholderText(QStringLiteral("确认密码"));
    confirmEdit_->setEchoMode(QLineEdit::Password);
    confirmEdit_->setFixedHeight(theme::kInputH);
    confirmEdit_->setStyleSheet(inputStyle());
    usePlaceholderColor(confirmEdit_);
    confirmLayout->addWidget(confirmEdit_);
    confirmRow_->hide();
    layout->addWidget(confirmRow_);

    // 错误条占位固定 32px，出现时按钮位置不下移。
    layout->addSpacing(12);
    errorBox_ = new QWidget(panel);
    errorBox_->setFixedHeight(32);
    QVBoxLayout* errorLayout = new QVBoxLayout(errorBox_);
    errorLayout->setContentsMargins(0, 0, 0, 0);
    errorLayout->setSpacing(0);
    errorLabel_ = new QLabel(errorBox_);
    errorLabel_->setFixedHeight(32);
    errorLabel_->setStyleSheet(
        QStringLiteral("background:%1; color:%2; border-radius:4px;"
                       " padding-left:12px; font-size:13px;")
            .arg(theme::kErrorBg.name())
            .arg(theme::kErrorText.name()));
    errorLabel_->hide();
    errorLayout->addWidget(errorLabel_);
    layout->addWidget(errorBox_);
    layout->addSpacing(12);

    primaryButton_ = new QPushButton(QStringLiteral("登 录"), panel);
    primaryButton_->setCursor(Qt::PointingHandCursor);
    primaryButton_->setFixedSize(theme::kInputW, theme::kInputH);
    primaryButton_->setStyleSheet(
        QStringLiteral("QPushButton { background:%1; color:#FFFFFF; border:none;"
                       " border-radius:4px; font-size:15px; }"
                       "QPushButton:hover { background:%2; }"
                       "QPushButton:pressed { background:%3; }"
                       "QPushButton:disabled { background:%4; }")
            .arg(theme::kPrimary.name())
            .arg(theme::kPrimaryHover.name())
            .arg(theme::kPrimaryPressed.name())
            .arg(theme::kPrimaryDisabled.name()));
    layout->addWidget(primaryButton_);

    layout->addStretch();

    QHBoxLayout* links = new QHBoxLayout();
    links->setContentsMargins(0, 0, 0, 0);
    links->setSpacing(0);
    registerLink_ = new QPushButton(QStringLiteral("注册账号"), panel);
    registerLink_->setFlat(true);
    registerLink_->setCursor(Qt::PointingHandCursor);
    registerLink_->setStyleSheet(linkStyle(theme::kPrimary));
    serverLink_ = new QPushButton(QStringLiteral("设置服务器"), panel);
    serverLink_->setFlat(true);
    serverLink_->setCursor(Qt::PointingHandCursor);
    serverLink_->setStyleSheet(linkStyle(theme::kTextMuted));
    links->addWidget(registerLink_);
    links->addStretch();
    links->addWidget(serverLink_);
    layout->addLayout(links);

    connect(primaryButton_, &QPushButton::clicked, this, &LoginWindow::submit);
    connect(registerLink_, &QPushButton::clicked, this, &LoginWindow::toggleMode);
    connect(serverLink_, &QPushButton::clicked, this, &LoginWindow::changeServer);
    connect(usernameEdit_, &QLineEdit::returnPressed, this, &LoginWindow::submit);
    connect(passwordEdit_, &QLineEdit::returnPressed, this, &LoginWindow::submit);
    connect(confirmEdit_, &QLineEdit::returnPressed, this, &LoginWindow::submit);

    setTabOrder(usernameEdit_, passwordEdit_);
    setTabOrder(passwordEdit_, passwordField->toggle());
    setTabOrder(passwordField->toggle(), primaryButton_);
    setTabOrder(primaryButton_, registerLink_);
    setTabOrder(registerLink_, serverLink_);

    return panel;
}

void LoginWindow::submit() {
    if (busy_) {
        return;
    }
    clearError();

    const QString username = usernameEdit_->text().trimmed();
    const QString password = passwordEdit_->text();
    if (username.isEmpty() || password.isEmpty()) {
        showError(QStringLiteral("请输入用户名和密码"));
        return;
    }
    if (mode_ == RegisterMode && password != confirmEdit_->text()) {
        showError(QStringLiteral("两次输入的密码不一致"));
        return;
    }

    setBusy(true);
    if (mode_ == LoginMode) {
        api_->login(username, password);
    } else {
        api_->registerUser(username, password);
    }
}

void LoginWindow::toggleMode() {
    clearError();
    setMode(mode_ == LoginMode ? RegisterMode : LoginMode);
}

void LoginWindow::changeServer() {
    clearError();
    bool ok = false;
    const QString url = QInputDialog::getText(this, QStringLiteral("设置服务器"),
                                              QStringLiteral("服务器地址（以 http:// 或 https:// 开头）"),
                                              QLineEdit::Normal, api_->baseUrl(), &ok)
                            .trimmed();
    if (!ok) {
        return;
    }
    if (!url.startsWith(QStringLiteral("http://")) &&
        !url.startsWith(QStringLiteral("https://"))) {
        showError(QStringLiteral("地址必须以 http:// 或 https:// 开头"));
        return;
    }
    api_->setBaseUrl(url);
    QSettings settings(QStringLiteral("Wangpan"), QStringLiteral("Client"));
    settings.setValue(QStringLiteral("server/baseUrl"), api_->baseUrl());
}

void LoginWindow::showError(const QString& message) {
    errorLabel_->setText(message);
    errorLabel_->show();
}

void LoginWindow::clearError() {
    errorLabel_->clear();
    errorLabel_->hide();
}

void LoginWindow::setMode(Mode mode) {
    mode_ = mode;
    const bool registering = mode_ == RegisterMode;

    tabs_->setVisible(!registering);
    confirmRow_->setVisible(registering);
    confirmEdit_->clear();
    passwordEdit_->clear();
    registerLink_->setText(registering ? QStringLiteral("返回登录") : QStringLiteral("注册账号"));
    updatePrimaryText();
    usernameEdit_->setFocus();
}

void LoginWindow::setBusy(bool busy) {
    busy_ = busy;
    usernameEdit_->setEnabled(!busy);
    passwordField_->setEnabled(!busy);
    confirmEdit_->setEnabled(!busy);
    primaryButton_->setEnabled(!busy);
    registerLink_->setEnabled(!busy);
    serverLink_->setEnabled(!busy);
    updatePrimaryText();
}

void LoginWindow::updatePrimaryText() {
    if (busy_) {
        primaryButton_->setText(mode_ == LoginMode ? QStringLiteral("登录中…")
                                                   : QStringLiteral("注册中…"));
    } else {
        primaryButton_->setText(mode_ == LoginMode ? QStringLiteral("登 录")
                                                   : QStringLiteral("注 册"));
    }
}

void LoginWindow::handleLoginSucceeded(qint64 userId) {
    QSettings settings(QStringLiteral("Wangpan"), QStringLiteral("Client"));
    settings.setValue(QStringLiteral("login/lastUsername"), usernameEdit_->text().trimmed());
    settings.setValue(QStringLiteral("server/baseUrl"), api_->baseUrl());
    passwordEdit_->clear();
    confirmEdit_->clear();
    setBusy(false);
    emit loggedIn(api_->token(), userId);
    accept();
}
