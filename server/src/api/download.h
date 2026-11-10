#pragma once

#include "config.h"
#include "httplib.h"

// 下载接口：整文件与单区间续传，响应流式发送，不把文件读进内存。
namespace api_download {

void register_routes(httplib::Server& svr, const Config& cfg);

}  // namespace api_download
