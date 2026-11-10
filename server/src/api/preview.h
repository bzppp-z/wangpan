#pragma once

#include "config.h"
#include "httplib.h"

// 在线预览：仅图片与文本白名单，文本最多 1 MiB，不落任何临时文件。
namespace api_preview {

void register_routes(httplib::Server& svr, const Config& cfg);

}  // namespace api_preview
