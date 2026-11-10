#pragma once

#include "httplib.h"

// 文件管理操作：重命名。删除与回收站见 api/trash.h。
namespace api_manage {

// PATCH /api/files/{id}，请求体 {"name":"新名称"}。
void handle_rename(const httplib::Request& req, httplib::Response& res);

// 注册 PATCH /api/files/{id}。
void register_routes(httplib::Server& svr);

}  // namespace api_manage
