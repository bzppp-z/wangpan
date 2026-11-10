#include <QApplication>
#include <QFont>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

#include "apiclient.h"
#include "ui/loginwindow.h"
#include "ui/theme.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Wangpan"));
    app.setApplicationName(QStringLiteral("Client"));
    app.setFont(QFont(QString::fromLatin1(theme::kFontFamily), 9));

    ApiClient api;
    api.setBaseUrl(QStringLiteral("http://127.0.0.1:8080"));

    LoginWindow login(&api);

    // 阶段 2 Task 3 接入真正的主窗口；此占位窗只验证登录成功流程。
    QWidget* shell = new QWidget;
    shell->setWindowTitle(QStringLiteral("网盘"));
    shell->resize(1100, 700);
    QVBoxLayout* shellLayout = new QVBoxLayout(shell);
    QLabel* hint = new QLabel(QStringLiteral("登录成功，主界面将在下一步接入"), shell);
    hint->setAlignment(Qt::AlignCenter);
    shellLayout->addWidget(hint);
    shell->hide();

    QObject::connect(&login, &LoginWindow::loggedIn, [&api, shell](const QString& token, qint64) {
        api.setToken(token);
        shell->show();
    });

    login.show();
    return app.exec();
}
