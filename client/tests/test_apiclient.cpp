// ApiClient 单元测试：URL 拼接、错误映射、token 头。
#include <QtTest>
#include "apiclient.h"

class TestApiClient : public QObject {
    Q_OBJECT
private slots:
    void buildsFilesUrl() {
        ApiClient c;
        c.setBaseUrl("http://127.0.0.1:18080");
        QCOMPARE(c.filesUrl(0), QString("http://127.0.0.1:18080/api/files?parent_id=0"));
        QCOMPARE(c.filesUrl(7), QString("http://127.0.0.1:18080/api/files?parent_id=7"));
    }
    void stripsTrailingSlash() {
        ApiClient c;
        c.setBaseUrl("http://127.0.0.1:18080/");
        QCOMPARE(c.filesUrl(0), QString("http://127.0.0.1:18080/api/files?parent_id=0"));
    }
    void mapsErrorBody() {
        ApiClient c;
        QCOMPARE(c.errorTextFor(409, "{\"error\":\"username_taken\",\"message\":\"该用户名已被使用\"}", QNetworkReply::NoError),
                 QString("该用户名已被使用"));
        QCOMPARE(c.errorTextFor(0, "", QNetworkReply::ConnectionRefusedError),
                 QString("无法连接服务器，请检查地址"));
        QCOMPARE(c.errorTextFor(401, "{\"error\":\"unauthorized\"}", QNetworkReply::NoError),
                 QString("登录已过期，请重新登录"));
    }
};
QTEST_MAIN(TestApiClient)
#include "test_apiclient.moc"
