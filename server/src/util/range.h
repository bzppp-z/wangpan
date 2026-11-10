#pragma once

#include <cstddef>
#include <string>

namespace util {

// 单区间 Range 解析结果。last 为闭区间终点（含）。
struct RangeSpec {
    bool valid = false;
    long long start = 0;
    long long last = 0;
};

namespace detail {

// 只接受纯数字且长度受限，避免溢出；空串返回 false。
inline bool parse_number(const std::string& text, long long& out) {
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

// 去掉首尾空白，HTTP 头两侧可能带空格。
inline std::string trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

}  // namespace detail

// 解析 `Range: bytes=<start>-[<end>]`，只接受单区间。
// size 为文件总长度：起点越界、终点小于起点、多区间、格式非法均返回 valid=false。
// 终点超过文件末尾时按末尾截断；`bytes=-N` 表示末尾 N 字节。
inline RangeSpec parse_range(const std::string& header, long long size) {
    RangeSpec out;
    if (size <= 0) {
        return out;
    }

    const std::string text = detail::trim(header);
    const std::string prefix = "bytes=";
    if (text.compare(0, prefix.size(), prefix) != 0) {
        return out;
    }
    const std::string spec = text.substr(prefix.size());
    if (spec.empty() || spec.find(',') != std::string::npos) {
        return out;  // 空区间或多区间都不支持
    }

    const std::size_t dash = spec.find('-');
    if (dash == std::string::npos || spec.find('-', dash + 1) != std::string::npos) {
        return out;
    }
    const std::string first = detail::trim(spec.substr(0, dash));
    const std::string second = detail::trim(spec.substr(dash + 1));

    if (first.empty()) {
        // 后缀形式：bytes=-N 表示最后 N 字节。
        long long suffix = 0;
        if (!detail::parse_number(second, suffix) || suffix <= 0) {
            return out;
        }
        out.start = suffix >= size ? 0 : size - suffix;
        out.last = size - 1;
        out.valid = true;
        return out;
    }

    long long start = 0;
    if (!detail::parse_number(first, start) || start >= size) {
        return out;
    }

    long long last = size - 1;
    if (!second.empty()) {
        if (!detail::parse_number(second, last) || last < start) {
            return out;
        }
        if (last >= size) {
            last = size - 1;
        }
    }

    out.start = start;
    out.last = last;
    out.valid = true;
    return out;
}

}  // namespace util
