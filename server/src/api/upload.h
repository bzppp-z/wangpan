#pragma once

#include "config.h"
#include "httplib.h"

// 上传接口：init（秒传 / 续传 / 配额）以及后续任务的分片写入与合并。
namespace api_upload {

// 注册 /api/upload/* 路由。cfg 提供数据目录、分片大小等运行参数。
void register_routes(httplib::Server& svr, const Config& cfg);

}  // namespace api_upload
