#include "api/preview.h"

#include <cstddef>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

#include "api/auth.h"
#include "api/respond.h"
#include "db/blobs.h"
#include "db/files.h"
#include "log.h"
#include "storage/blob.h"
#include "util/mime.h"

namespace api_preview {

namespace {

// 文本预览上限：超过就只发前 1 MiB，并带 X-Truncated 让客户端提示。
const long long kMaxTextBytes = 1024 * 1024;

bool parse_id(const std::string& text, long long& out) {
    if (text.empty() || text.size() > 18) {
        return false;
    }
    long long value = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

// 取最后一个点之后的扩展名并转小写；没有点或点后为空返回空串。
std::string extension_of(const std::string& name) {
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) {
        return std::string();
    }
    std::string ext = name.substr(dot + 1);
    for (std::size_t i = 0; i < ext.size(); ++i) {
        const char c = ext[i];
        if (c >= 'A' && c <= 'Z') {
            ext[i] = static_cast<char>(c - 'A' + 'a');
        }
    }
    return ext;
}

// 从 begin 开始发 len 字节；只需处理"从头开始"的预览，不涉及区间。
bool serve_file(const std::string& path, long long len, httplib::DataSink& sink,
                std::size_t offset) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) {
        return false;
    }
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!in) {
        return false;
    }

    std::vector<char> buffer(128 * 1024);
    long long remaining = len - static_cast<long long>(offset);
    while (remaining > 0) {
        const std::size_t want =
            remaining < static_cast<long long>(buffer.size()) ? static_cast<std::size_t>(remaining)
                                                              : buffer.size();
        in.read(&buffer[0], static_cast<std::streamsize>(want));
        const std::streamsize got = in.gcount();
        if (got <= 0) {
            return false;
        }
        if (!sink.write(buffer.data(), static_cast<std::size_t>(got))) {
            return false;
        }
        remaining -= static_cast<long long>(got);
    }
    return true;
}

void handle_preview(const httplib::Request& req, httplib::Response& res, const Config& cfg) {
    long long user_id = 0;
    if (!auth::resolve(req, user_id)) {
        api::json_error(res, 401, "unauthorized", "登录已过期，请重新登录");
        return;
    }

    long long file_id = 0;
    if (req.matches.size() < 2 || !parse_id(req.matches[1], file_id)) {
        api::json_error(res, 404, "not_found", "文件不存在");
        return;
    }

    db::files::Row row;
    if (!db::files::find_file(user_id, file_id, row)) {
        api::json_error(res, 404, "not_found", "文件不存在");
        return;
    }

    const std::string ext = extension_of(row.name);
    const char* mime = util::mime_for_extension(ext);
    if (mime == nullptr) {
        // 白名单之外一律拒绝：svg 可执行脚本，pdf/docx/mp4 交给客户端下载后查看。
        api::json_error(res, 415, "preview_unsupported", "暂不支持预览该类型，请下载后查看");
        return;
    }

    db::blobs::Info blob;
    if (!db::blobs::find_by_id(row.blob_id, blob)) {
        log_error("预览找不到内容记录：id=" + std::to_string(file_id));
        api::json_error(res, 404, "not_found", "文件内容不存在");
        return;
    }

    const std::string path = storage::blob_path(cfg.data_dir, blob.sha256);
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        log_error("预览内容文件缺失：" + blob.sha256);
        api::json_error(res, 404, "not_found", "文件内容不存在");
        return;
    }
    const long long size = static_cast<long long>(st.st_size);

    res.set_header("X-Content-Type-Options", "nosniff");
    res.set_header("X-Content-SHA256", blob.sha256);

    const bool is_text = util::is_text_extension(ext);
    const long long length = is_text && size > kMaxTextBytes ? kMaxTextBytes : size;
    if (is_text && size > kMaxTextBytes) {
        res.set_header("X-Truncated", "1");
    }

    res.set_content_provider(static_cast<std::size_t>(length), mime,
                             [path, length](std::size_t offset, std::size_t want,
                                            httplib::DataSink& sink) {
                                 (void)want;
                                 return serve_file(path, length, sink, offset);
                             });
}

}  // namespace

void register_routes(httplib::Server& svr, const Config& cfg) {
    svr.Get(R"(/api/files/([0-9]+)/preview)",
            [&cfg](const httplib::Request& req, httplib::Response& res) {
                handle_preview(req, res, cfg);
            });
}

}  // namespace api_preview
