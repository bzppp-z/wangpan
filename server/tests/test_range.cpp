#include <cstdio>
#include <string>

#include "util/range.h"

static int g_fail = 0;

static void expect(bool ok, const char* name) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        ++g_fail;
    }
}

static void expect_range(const std::string& header, long long size, long long start, long long last,
                         const char* name) {
    const util::RangeSpec spec = util::parse_range(header, size);
    const bool ok = spec.valid && spec.start == start && spec.last == last;
    if (!ok) {
        std::printf("  header=「%s」size=%lld 实得 valid=%d start=%lld last=%lld\n", header.c_str(),
                    size, spec.valid ? 1 : 0, spec.start, spec.last);
    }
    expect(ok, name);
}

static void expect_invalid(const std::string& header, long long size, const char* name) {
    const util::RangeSpec spec = util::parse_range(header, size);
    expect(!spec.valid, name);
}

int main() {
    expect_range("bytes=0-", 100, 0, 99, "从头到末尾");
    expect_range("bytes=40-59", 100, 40, 59, "闭区间");
    expect_range("bytes=90-200", 100, 90, 99, "终点越界按末尾截断");
    expect_range("bytes=-10", 100, 90, 99, "后缀形式");
    expect_range("bytes=-200", 100, 0, 99, "后缀超过文件长度");

    expect_invalid("bytes=100-", 100, "起点等于大小");
    expect_invalid("bytes=120-130", 100, "起点超过大小");
    expect_invalid("bytes=59-40", 100, "终点小于起点");
    expect_invalid("bytes=-0", 100, "后缀长度为 0");
    expect_invalid("bytes=-", 100, "缺少起止");
    expect_invalid("bytes=0-1,5-6", 100, "多区间不支持");
    expect_invalid("items=0-", 100, "单位不是 bytes");
    expect_invalid("bytes=a-b", 100, "非数字");
    expect_invalid("", 100, "缺少头");
    expect_invalid("bytes=0-", 0, "空文件无法取区间");

    // 大小 1 的边界：只有 bytes=0- 合法。
    expect_range("bytes=0-", 1, 0, 0, "单字节文件整取");
    expect_invalid("bytes=1-", 1, "单字节文件起点越界");

    std::printf("range: %s\n", g_fail == 0 ? "PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
