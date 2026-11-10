#pragma once

#include <string>

#include "httplib.h"

// 目录浏览与新建文件夹。
namespace api_files {

// 名称规则（PRD R2）：1–255 字节，禁止 / \ : * ? " < > | 与控制字符，禁止 "." ".."。
bool valid_entry_name(const std::string& name);

void handle_list(const httplib::Request& req, httplib::Response& res);
void handle_create_folder(const httplib::Request& req, httplib::Response& res);

// 注册 GET /api/files、POST /api/folders。
void register_routes(httplib::Server& svr);

}  // namespace api_files
