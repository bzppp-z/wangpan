#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkReply>
#include <QObject>
#include <QString>

#include <functional>

class QNetworkAccessManager;
class QNetworkRequest;

// 与阶段 1 服务端 HTTP/JSON 接口对应的异步客户端。
// token 只保存在内存中，不写磁盘；密码不落盘、不进日志。
class ApiClient : public QObject {
    Q_OBJECT
public:
    explicit ApiClient(QObject* parent = nullptr);

    void setBaseUrl(const QString& baseUrl);
    QString baseUrl() const;

    // 会话 token；空串表示未登录。
    void setToken(const QString& token);
    QString token() const;

    // 目录列表 URL，供界面与测试直接复用。
    QString filesUrl(qlonglong parentId) const;

    // 供上传等长任务自行发起请求：复用同一个网络管理器与鉴权头。
    QNetworkAccessManager* manager() const;
    // path 以 "/" 开头；自动带上 Content-Type 与 Authorization: Bearer。
    QNetworkRequest requestFor(const QString& path,
                               const QString& contentType = QStringLiteral("application/json")) const;
    // 下载请求：offset > 0 时带 Range: bytes=<offset>-，并要求不压缩以保持字节对齐。
    QNetworkRequest requestForDownload(qlonglong fileId, qint64 offset) const;

    // 把 HTTP 状态 + 错误响应体映射为可直接展示的中文提示。
    static QString errorTextFor(int status, const QString& body, QNetworkReply::NetworkError err);

    void login(const QString& username, const QString& password);
    void registerUser(const QString& username, const QString& password);
    void logout();
    void listFiles(qlonglong parentId);
    void createFolder(qlonglong parentId, const QString& name);
    // 单条目重命名；服务端冲突时按 R2 自动改名，最终名称见 renamed()。
    void renameFile(qlonglong id, const QString& name);
    // 软删除（连同子项进回收站），不扣配额。
    void deleteFile(qlonglong id);
    void listTrash();
    void restoreTrash(qlonglong id);
    void purgeTrash(qlonglong id);
    void clearTrash();

signals:
    void loginSucceeded(qint64 userId, qint64 expiresAt);
    void registered(qint64 userId, qint64 expiresAt);
    void filesLoaded(const QJsonArray& items, qint64 usedBytes, qint64 quotaBytes);
    void folderCreated(qint64 id, const QString& finalName);
    void renamed(qint64 id, const QString& finalName);
    void deleted(qint64 id);
    void trashLoaded(const QJsonArray& items);
    void trashRestored(qint64 id, const QString& finalName, qint64 parentId);
    void trashPurged(qint64 id);
    void trashCleared();
    void loggedOut();
    // 可展示给用户的失败原因（服务端 message 或预设文案）。
    void failed(const QString& message);
    // 受保护接口返回 401：会话失效，需要重新登录。
    void unauthorized();

private:
    QNetworkRequest makeRequest(const QString& path) const;
    // 发出 GET / POST（无内容时也显式带空体，保证有 Content-Length）。
    void sendGet(const QNetworkRequest& req, bool sessionEndpoint,
                 const std::function<void(const QJsonObject&)>& onSuccess);
    void sendPost(const QNetworkRequest& req, const QByteArray& body, bool sessionEndpoint,
                  const std::function<void(const QJsonObject&)>& onSuccess);
    // PATCH / DELETE：显式带 body（可为空），保证请求有 Content-Length。
    void sendCustom(const QNetworkRequest& req, const QByteArray& verb, const QByteArray& body,
                    bool sessionEndpoint,
                    const std::function<void(const QJsonObject&)>& onSuccess);
    // 统一处理网络错误 / 401 / 失败响应；成功时回调已解析的 JSON。
    // sessionEndpoint 为 true 时，401 视为会话失效并发 unauthorized()。
    void finish(QNetworkReply* reply, bool sessionEndpoint,
                const std::function<void(const QJsonObject&)>& onSuccess);

    QNetworkAccessManager* nam_;
    QString baseUrl_;
    QString token_;
};
