#include <QApplication>
#include <QFont>

#include "apiclient.h"
#include "ui/loginwindow.h"
#include "ui/mainwindow.h"
#include "ui/theme.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Wangpan"));
    app.setApplicationName(QStringLiteral("Client"));
    app.setFont(QFont(QString::fromLatin1(theme::kFontFamily), 9));

    ApiClient api;
    api.setBaseUrl(QStringLiteral("http://127.0.0.1:8080"));

    LoginWindow login(&api);
    MainWindow* mainWindow = nullptr;

    QObject::connect(&login, &LoginWindow::loggedIn, [&](const QString& token, qint64) {
        api.setToken(token);
        mainWindow = new MainWindow(&api);
        QObject::connect(mainWindow, &MainWindow::signedOut, &login, [&]() {
            if (mainWindow != nullptr) {
                mainWindow->deleteLater();
                mainWindow = nullptr;
            }
            login.show();
            login.raise();
            login.activateWindow();
        });
        mainWindow->show();
    });

    login.show();
    return app.exec();
}
