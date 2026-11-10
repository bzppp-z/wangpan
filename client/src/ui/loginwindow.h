#pragma once

#include <QDialog>
#include <QString>

class ApiClient;
class QLabel;
class QLineEdit;
class QPushButton;
class QWidget;

// 登录 / 注册窗口（独立窗口，两种状态在同一窗口内切换）。
// 只把上次用户名与服务器地址写入 QSettings，绝不保存密码。
class LoginWindow : public QDialog {
    Q_OBJECT
public:
    explicit LoginWindow(ApiClient* api, QWidget* parent = nullptr);

signals:
    void loggedIn(const QString& token, qint64 userId);

private slots:
    void submit();
    void toggleMode();
    void changeServer();
    void showError(const QString& message);
    void clearError();

private:
    enum Mode { LoginMode, RegisterMode };

    void buildUi();
    QWidget* buildBrandPanel();
    QWidget* buildFormPanel();
    void setMode(Mode mode);
    void setBusy(bool busy);
    void handleLoginSucceeded(qint64 userId);
    void updatePrimaryText();

    ApiClient* api_;
    Mode mode_;
    bool busy_;

    QWidget* tabs_;
    QPushButton* loginTab_;
    QPushButton* scanTab_;
    QLineEdit* usernameEdit_;
    QWidget* passwordField_;
    QLineEdit* passwordEdit_;
    QWidget* confirmRow_;
    QLineEdit* confirmEdit_;
    QWidget* errorBox_;
    QLabel* errorLabel_;
    QPushButton* primaryButton_;
    QPushButton* registerLink_;
    QPushButton* serverLink_;
};
