#pragma once

#include <string>
#include <vector>

#include "config.h"
#include "httplib.h"

// 回收站：列表、恢复、彻底删除、清空。
namespace api_trash {

// 事务提交后删除已归零 blob 的磁盘内容；单个文件失败只告警，不影响响应。
void remove_blob_files(const Config& cfg, const std::vector<std::string>& shas);

void handle_list(const httplib::Request& req, httplib::Response& res);
void handle_restore(const httplib::Request& req, httplib::Response& res);
void handle_purge(const httplib::Request& req, httplib::Response& res, const Config& cfg);
void handle_clear(const httplib::Request& req, httplib::Response& res, const Config& cfg);

// 注册 GET /api/trash、POST /api/trash/{id}/restore、DELETE /api/trash/{id}、
// DELETE /api/trash。
void register_routes(httplib::Server& svr, const Config& cfg);

}  // namespace api_trash
