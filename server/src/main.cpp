#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>

#include "api/auth.h"
#include "api/download.h"
#include "api/files.h"
#include "api/manage.h"
#include "api/preview.h"
#include "api/respond.h"
#include "api/trash.h"
#include "api/upload.h"
#include "config.h"
#include "db/blobs.h"
#include "db/db.h"
#include "db/files.h"
#include "httplib.h"
#include "log.h"
#include "nlohmann/json.hpp"

namespace {

// 逐级创建目录；已存在且确实是目录时视为成功。
bool ensure_directory(const std::string& path, std::string& err) {
    if (path.empty()) {
        err = "数据目录不能为空";
        return false;
    }

    std::string current;
    std::size_t pos = 0;
    if (path.front() == '/') {
        current = "/";
        pos = 1;
    }

    while (pos <= path.size()) {
        const std::size_t slash = path.find('/', pos);
        const std::string part =
            (slash == std::string::npos) ? path.substr(pos) : path.substr(pos, slash - pos);

        if (!part.empty()) {
            if (current.empty()) {
                current = part;
            } else if (current == "/") {
                current += part;
            } else {
                current += "/" + part;
            }

            if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                err = "创建目录失败：" + current + "（" + std::strerror(errno) + "）";
                return false;
            }

            struct stat info;
            if (::stat(current.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
                err = "路径不是目录：" + current;
                return false;
            }
        }

        if (slash == std::string::npos) {
            break;
        }
        pos = slash + 1;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    std::string err;
    if (!parse_args(argc, argv, cfg, err)) {
        log_error(err);
        std::fprintf(stderr,
                     "用法：%s [--port 8080] [--bind 127.0.0.1] [--data-dir ./data]"
                     " [--quota-bytes 5368709120]\n",
                     argv[0]);
        return 2;
    }

    if (!ensure_directory(cfg.data_dir, err)) {
        log_error(err);
        return 1;
    }

    const std::string db_path = cfg.data_dir + "/wangpan.db";
    if (!db::open(db_path, err)) {
        log_error(err);
        return 1;
    }
    if (!db::init_schema(err)) {
        log_error("建表失败：" + err);
        db::close();
        return 1;
    }

    httplib::Server svr;
    // 分片固定 4 MiB，留一倍余量；再大的请求体由 httplib 直接拒绝（413）。
    svr.set_payload_max_length(8 * 1024 * 1024);
    svr.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        nlohmann::json body;
        body["ok"] = true;
        body["version"] = "0.1";
        api::json_ok(res, body);
    });

    auth::register_routes(svr, cfg.quota_bytes);
    api_files::register_routes(svr);
    api_manage::register_routes(svr);
    api_upload::register_routes(svr, cfg);
    api_download::register_routes(svr, cfg);
    api_preview::register_routes(svr, cfg);
    api_trash::register_routes(svr, cfg);

    // 启动清理：回收站里超过 30 天的内容 + 崩溃可能留下的零引用 blob。
    // 清理只动数据库事务，磁盘文件在事务提交后再删，避免回滚后文件先没了。
    {
        std::vector<std::string> shas;
        std::string cleanup_err;
        if (!db::files::purge_expired(db::now_seconds(), shas, cleanup_err)) {
            log_warn("启动清理回收站失败：" + cleanup_err);
        }
        std::vector<std::string> orphan_shas;
        if (!db::blobs::remove_zero_ref(orphan_shas, cleanup_err)) {
            log_warn("启动清理孤儿内容失败：" + cleanup_err);
        }
        shas.insert(shas.end(), orphan_shas.begin(), orphan_shas.end());
        api_trash::remove_blob_files(cfg, shas);
    }

    // 之后每 6 小时清一次过期回收站；进程退出即结束，不 join。
    std::thread([cfg]() {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::hours(6));
            std::vector<std::string> shas;
            std::string cleanup_err;
            if (!db::files::purge_expired(db::now_seconds(), shas, cleanup_err)) {
                log_warn("定时清理回收站失败：" + cleanup_err);
                continue;
            }
            api_trash::remove_blob_files(cfg, shas);
        }
    }).detach();

    // cpp-httplib 自带的 Range 处理会把"终点越界"判成 416，多区间还会拼 multipart，
    // 与下载接口的契约（越界截断、只接受单区间）不符。这里清掉框架的解析结果，
    // 由 download 自行解析原始 Range 头，避免同一区间被切片两次。
    svr.set_pre_routing_handler([](const httplib::Request& req, httplib::Response&) {
        if (req.has_header("Range")) {
            const_cast<httplib::Request&>(req).ranges.clear();
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    log_info("监听地址 http://" + cfg.bind_addr + ":" + std::to_string(cfg.port));
    log_info("数据目录 " + cfg.data_dir);

    if (!svr.listen(cfg.bind_addr, cfg.port)) {
        log_error("服务启动失败：" + cfg.bind_addr + ":" + std::to_string(cfg.port) +
                  "（端口可能已被占用）");
        db::close();
        return 1;
    }
    db::close();
    return 0;
}
