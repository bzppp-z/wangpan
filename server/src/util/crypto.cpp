#include "util/crypto.h"

namespace crypto {

std::string random_hex(std::size_t) {
    return std::string();  // 尚未实现
}

std::string sha256_hex(const std::string&) {
    return std::string();  // 尚未实现
}

std::string pbkdf2_sha256(const std::string&, const std::string&, int) {
    return std::string();  // 尚未实现
}

bool constant_time_equal(const std::string&, const std::string&) {
    return false;  // 尚未实现
}

}  // namespace crypto
