#pragma once

#include <ctime>
#include <iostream>
#include <mutex>
#include <string>

namespace logging_detail {

// 生成本地时间戳，格式 "YYYY-MM-DD HH:MM:SS"。
inline std::string timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    char buf[32] = {0};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return std::string(buf);
}

// 统一日志格式：[时间][级别] 消息。调用方不得把密码、token、请求体传入。
inline void write(std::ostream& os, const char* level, const std::string& message) {
    static std::mutex mutex;  // 多线程下避免日志行互相穿插
    std::lock_guard<std::mutex> lock(mutex);
    os << "[" << timestamp() << "][" << level << "] " << message << std::endl;
}

}  // namespace logging_detail

// 普通信息输出到 stdout。
inline void log_info(const std::string& message) {
    logging_detail::write(std::cout, "INFO", message);
}

// 警告输出到 stderr。
inline void log_warn(const std::string& message) {
    logging_detail::write(std::cerr, "WARN", message);
}

// 错误输出到 stderr。
inline void log_error(const std::string& message) {
    logging_detail::write(std::cerr, "ERROR", message);
}
