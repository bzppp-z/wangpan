#include "config.h"

#include <cerrno>
#include <cstdlib>

namespace {

// 解析端口：必须是十进制整数，且落在 1-65535 之间。
bool parse_port(const std::string& text, int& out, std::string& err) {
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' || value < 1 || value > 65535) {
        err = "端口非法（应为 1-65535 的整数）：" + text;
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

// 解析字节数：十进制正整数，不允许符号与小数。
bool parse_bytes(const std::string& text, std::uint64_t& out, std::string& err) {
    if (text.empty() || text[0] < '0' || text[0] > '9') {
        err = "字节数非法（应为正整数）：" + text;
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' || value == 0) {
        err = "字节数非法（应为正整数）：" + text;
        return false;
    }
    out = static_cast<std::uint64_t>(value);
    return true;
}

}  // namespace

bool parse_args(int argc, char** argv, Config& cfg, std::string& err) {
    err.clear();
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg != "--port" && arg != "--bind" && arg != "--data-dir" && arg != "--quota-bytes") {
            err = "未知参数：" + arg;
            return false;
        }
        if (i + 1 >= argc) {
            err = "参数 " + arg + " 缺少取值";
            return false;
        }
        const std::string value = argv[++i];

        if (arg == "--port") {
            if (!parse_port(value, cfg.port, err)) {
                return false;
            }
        } else if (arg == "--bind") {
            if (value.empty()) {
                err = "绑定地址不能为空";
                return false;
            }
            cfg.bind_addr = value;
        } else if (arg == "--data-dir") {
            if (value.empty()) {
                err = "数据目录不能为空";
                return false;
            }
            cfg.data_dir = value;
        } else {
            if (!parse_bytes(value, cfg.quota_bytes, err)) {
                return false;
            }
        }
    }
    return true;
}
