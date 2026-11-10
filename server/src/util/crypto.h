#pragma once

#include <cstddef>
#include <string>

// 密码学工具：全部基于 OpenSSL，失败时返回空串或 false，不抛异常。
namespace crypto {

// PBKDF2-HMAC-SHA256 迭代次数；调参时只改这里。
const int kPbkdf2Iterations = 100000;

// 生成 byte_len 字节的随机数并转成小写十六进制（长度为 byte_len * 2）。
std::string random_hex(std::size_t byte_len);

// 对任意字节串求 SHA-256，返回小写十六进制。
std::string sha256_hex(const std::string& data);

// 用十六进制 salt 对密码做 PBKDF2-HMAC-SHA256，返回小写十六进制。
std::string pbkdf2_sha256(const std::string& password, const std::string& salt_hex,
                          int iterations = kPbkdf2Iterations);

// 定长比较，避免按字节短路泄露信息。
bool constant_time_equal(const std::string& a, const std::string& b);

}  // namespace crypto
