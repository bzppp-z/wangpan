#include "util/crypto.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <vector>

namespace crypto {

namespace {

const char* kHexDigits = "0123456789abcdef";

std::string to_hex(const unsigned char* data, std::size_t len) {
    std::string out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(kHexDigits[data[i] >> 4]);
        out.push_back(kHexDigits[data[i] & 0x0F]);
    }
    return out;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool from_hex(const std::string& hex, std::vector<unsigned char>& out) {
    out.clear();
    if (hex.size() % 2 != 0) {
        return false;
    }
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int high = hex_value(hex[i]);
        const int low = hex_value(hex[i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out.push_back(static_cast<unsigned char>((high << 4) | low));
    }
    return true;
}

}  // namespace

std::string random_hex(std::size_t byte_len) {
    if (byte_len == 0) {
        return std::string();
    }
    std::vector<unsigned char> buffer(byte_len);
    if (RAND_bytes(buffer.data(), static_cast<int>(byte_len)) != 1) {
        return std::string();
    }
    return to_hex(buffer.data(), buffer.size());
}

std::string sha256_hex(const std::string& data) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    if (SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest) == nullptr) {
        return std::string();
    }
    return to_hex(digest, SHA256_DIGEST_LENGTH);
}

std::string pbkdf2_sha256(const std::string& password, const std::string& salt_hex, int iterations) {
    std::vector<unsigned char> salt;
    if (!from_hex(salt_hex, salt) || salt.empty()) {
        return std::string();
    }
    unsigned char derived[32];
    if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()), salt.data(),
                          static_cast<int>(salt.size()), iterations, EVP_sha256(),
                          static_cast<int>(sizeof(derived)), derived) != 1) {
        return std::string();
    }
    return to_hex(derived, sizeof(derived));
}

bool constant_time_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    if (a.empty()) {
        return true;
    }
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

}  // namespace crypto
