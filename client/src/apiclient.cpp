#include "apiclient.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace {

// 所有请求的统一超时：超过后主动 abort，避免界面无限等待。
const int kTimeoutMs = 10000;

// 取出错误响应体里的中文 message；非 JSON 或没有 message 时返回空串。
QString messageFromBody(const QString& body) {
    const QJsonDocument doc = QJsonDocument::fromJson(body.toUtf8());
    if (!doc.isObject()) {
        return QString();
    }
    return doc.object().value("message").toString();
}

}  // namespace

ApiClient::ApiClient(QObject* parent) : QObject(parent), nam_(new QNetworkAccessManager(this)) {}

void ApiClient::setBaseUrl(const QString& baseUrl) {
    QString trimmed = baseUrl.trimmed();
    while (trimmed.endsWith('/')) {
        trimmed.chop(1);
    }
    baseUrl_ = trimmed;
}

QString ApiClient::baseUrl() const {
    return baseUrl_;
}

void ApiClient::setToken(const QString& token) {
    token_ = token;
}

QString ApiClient::token() const {
    return token_;
}

QString ApiClient::filesUrl(qlonglong parentId) const {
    return baseUrl_ + QStringLiteral("/api/files?parent_id=%1").arg(parentId);
}

QNetworkAccessManager* ApiClient::manager() const {
    return nam_;
}

QNetworkRequest ApiClient::requestFor(const QString& path, const QString& contentType) const {
    QNetworkRequest req{QUrl(baseUrl_ + path)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    if (!token_.isEmpty()) {
        req.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
    }
    return req;
}

QNetworkRequest ApiClient::requestForDownload(qlonglong fileId, qint64 offset) const {
    const QNetworkRequest base =
        requestFor(QStringLiteral("/api/files/%1/content").arg(fileId),
                   QStringLiteral("application/octet-stream"));
    QNetworkRequest req = base;
    req.setRawHeader("Accept-Encoding", "identity");
    if (offset > 0) {
        req.setRawHeader("Range", QByteArray("bytes=") + QByteArray::number(offset) + "-");
    }
    return req;
}

QString ApiClient::errorTextFor(int status, const QString& body, QNetworkReply::NetworkError err) {
    if (status == 0) {
        switch (err) {
            case QNetworkReply::ConnectionRefusedError:
            case QNetworkReply::HostNotFoundError:
            case QNetworkReply::RemoteHostClosedError:
            case QNetworkReply::UnknownNetworkError:
            case QNetworkReply::NetworkSessionFailedError:
                return QStringLiteral("无法连接服务器，请检查地址");
            case QNetworkReply::TimeoutError:
            case QNetworkReply::OperationCanceledError:
                return QStringLiteral("请求超时，请检查网络");
            default:
                return QStringLiteral("网络异常，请稍后重试");
        }
    }

    const QString message = messageFromBody(body);
    if (!message.isEmpty()) {
        return message;
    }
    switch (status) {
        case 400:
            return QStringLiteral("请求参数有误，请检查后重试");
        case 401:
            return QStringLiteral("登录已过期，请重新登录");
        case 413:
            return QStringLiteral("内容过大，请检查输入");
        case 507:
            return QStringLiteral("空间不足，请先清理文件");
        case 500:
        case 502:
        case 503:
            return QStringLiteral("服务器内部错误，请稍后重试");
        default:
            return QStringLiteral("请求失败（HTTP %1）").arg(status);
    }
}

void ApiClient::login(const QString& username, const QString& password) {
    QJsonObject payload;
    payload["username"] = username;
    payload["password"] = password;
    const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    sendPost(makeRequest(baseUrl_ + QStringLiteral("/api/login")), body, false,
             [this](const QJsonObject& obj) {
                 token_ = obj.value("token").toString();
                 const qint64 userId = static_cast<qint64>(obj.value("user_id").toDouble());
                 const qint64 expiresAt = static_cast<qint64>(obj.value("expires_at").toDouble());
                 emit loginSucceeded(userId, expiresAt);
             });
}

void ApiClient::registerUser(const QString& username, const QString& password) {
    QJsonObject payload;
    payload["username"] = username;
    payload["password"] = password;
    const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    sendPost(makeRequest(baseUrl_ + QStringLiteral("/api/register")), body, false,
             [this](const QJsonObject& obj) {
                 token_ = obj.value("token").toString();
                 const qint64 userId = static_cast<qint64>(obj.value("user_id").toDouble());
                 const qint64 expiresAt = static_cast<qint64>(obj.value("expires_at").toDouble());
                 emit registered(userId, expiresAt);
             });
}

void ApiClient::logout() {
    sendPost(makeRequest(baseUrl_ + QStringLiteral("/api/logout")), QByteArray(), true,
             [this](const QJsonObject&) {
                 token_.clear();
                 emit loggedOut();
             });
}

void ApiClient::listFiles(qlonglong parentId) {
    sendGet(makeRequest(filesUrl(parentId)), true, [this](const QJsonObject& obj) {
        const qint64 used = static_cast<qint64>(obj.value("used_bytes").toDouble());
        const qint64 quota = static_cast<qint64>(obj.value("quota_bytes").toDouble());
        emit filesLoaded(obj.value("items").toArray(), used, quota);
    });
}

void ApiClient::createFolder(qlonglong parentId, const QString& name) {
    QJsonObject payload;
    payload["parent_id"] = static_cast<double>(parentId);
    payload["name"] = name;
    const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    sendPost(makeRequest(baseUrl_ + QStringLiteral("/api/folders")), body, true,
             [this](const QJsonObject& obj) {
                 const qint64 id = static_cast<qint64>(obj.value("id").toDouble());
                 emit folderCreated(id, obj.value("name").toString());
             });
}

QNetworkRequest ApiClient::makeRequest(const QString& path) const {
    QNetworkRequest req{QUrl(path)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!token_.isEmpty()) {
        req.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
    }
    return req;
}

void ApiClient::sendGet(const QNetworkRequest& req, bool sessionEndpoint,
                        const std::function<void(const QJsonObject&)>& onSuccess) {
    QNetworkReply* reply = nam_->get(req);
    QTimer* timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout->start(kTimeoutMs);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, sessionEndpoint, onSuccess]() { finish(reply, sessionEndpoint, onSuccess); });
}

void ApiClient::sendPost(const QNetworkRequest& req, const QByteArray& body, bool sessionEndpoint,
                         const std::function<void(const QJsonObject&)>& onSuccess) {
    // 显式传 body（甚至空体），保证请求带 Content-Length：cpp-httplib 等待无长度 POST 会卡到超时。
    QNetworkReply* reply = nam_->post(req, body);
    QTimer* timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout->start(kTimeoutMs);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, sessionEndpoint, onSuccess]() { finish(reply, sessionEndpoint, onSuccess); });
}

void ApiClient::finish(QNetworkReply* reply, bool sessionEndpoint,
                       const std::function<void(const QJsonObject&)>& onSuccess) {
    reply->deleteLater();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray raw = reply->readAll();
    const QString body = QString::fromUtf8(raw);

    if (status == 0) {  // 没拿到 HTTP 响应，只能按 Qt 的网络错误判断
        emit failed(errorTextFor(0, body, reply->error()));
        return;
    }
    if (sessionEndpoint && status == 401) {
        emit unauthorized();
        return;
    }
    if (status < 200 || status >= 300) {
        emit failed(errorTextFor(status, body, QNetworkReply::NoError));
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    onSuccess(doc.isObject() ? doc.object() : QJsonObject());
}
