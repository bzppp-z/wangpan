#pragma once

#include <string>

#include "httplib.h"
#include "nlohmann/json.hpp"

namespace api {

// 成功响应：默认 200，响应体为 JSON。
inline void json_ok(httplib::Response& res, const nlohmann::json& body, int status = 200) {
    res.status = status;
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

// 失败响应：统一为 {"error":"<code>","message":"<中文提示>"}。
inline void json_error(httplib::Response& res, int status, const std::string& code,
                       const std::string& message) {
    nlohmann::json body;
    body["error"] = code;
    body["message"] = message;
    res.status = status;
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

}  // namespace api
