#include "api/auth.h"

namespace auth {

bool resolve(const httplib::Request&, long long& out_user_id) {
    out_user_id = 0;
    return false;  // 尚未实现
}

std::string bearer_token(const httplib::Request&) {
    return std::string();  // 尚未实现
}

void handle_register(const httplib::Request&, httplib::Response& res) {
    res.status = 501;
}

void handle_login(const httplib::Request&, httplib::Response& res) {
    res.status = 501;
}

void handle_logout(const httplib::Request&, httplib::Response& res) {
    res.status = 501;
}

void register_routes(httplib::Server&) {}

}  // namespace auth
