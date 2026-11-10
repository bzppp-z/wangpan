#pragma once

#include "httplib.h"

// 文件管理操作：重命名与删除到回收站。回收站本身见 api/trash.h。
namespace api_manage {

// PATCH /api/files/{id}，请求体 {"name":"新名称"}。
void handle_rename(const httplib::Request& req, httplib::Response& res);

// DELETE /api/files/{id}：软删除整棵子树，204。
void handle_delete(const httplib::Request& req, httplib::Response& res);

// 注册 PATCH /api/files/{id}、DELETE /api/files/{id}。
void register_routes(httplib::Server& svr);

}  // namespace api_manage
