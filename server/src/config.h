#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// 服务端运行配置：字段名与默认值后续任务直接复用，不要改名。
struct Config {
    int port = 8080;                                        // 监听端口
    std::string bind_addr = "127.0.0.1";                    // 绑定地址，默认只监听本机
    std::string data_dir = "./data";                        // 数据目录
    std::size_t chunk_size = 4 * 1024 * 1024;               // 分片大小，默认 4 MiB
    std::uint64_t quota_bytes = 5ULL * 1024 * 1024 * 1024;  // 单用户配额，默认 5 GiB
};

// 解析命令行参数，支持 --port、--bind、--data-dir、--quota-bytes。
// 成功返回 true；失败返回 false，并把中文原因写入 err。
bool parse_args(int argc, char** argv, Config& cfg, std::string& err);
