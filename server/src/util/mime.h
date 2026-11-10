#pragma once

#include <string>

namespace util {

// 预览白名单：扩展名必须是已转小写、不含点的形式。
// 返回 nullptr 表示不支持预览；svg / pdf / docx 等一律不在白名单内。
inline const char* mime_for_extension(const std::string& ext) {
    if (ext == "png") {
        return "image/png";
    }
    if (ext == "jpg" || ext == "jpeg") {
        return "image/jpeg";
    }
    if (ext == "gif") {
        return "image/gif";
    }
    if (ext == "bmp") {
        return "image/bmp";
    }
    if (ext == "webp") {
        return "image/webp";
    }
    if (ext == "txt" || ext == "md" || ext == "log" || ext == "json" || ext == "xml" ||
        ext == "csv" || ext == "cpp" || ext == "h") {
        return "text/plain; charset=utf-8";
    }
    return nullptr;
}

// 文本类预览需要按 1 MiB 截断并加 X-Truncated。
inline bool is_text_extension(const std::string& ext) {
    const char* mime = mime_for_extension(ext);
    return mime != nullptr && std::string(mime).compare(0, 4, "text") == 0;
}

}  // namespace util
