#pragma once

#include <string>
#include <vector>

// 文件 / 文件夹元数据访问。parent_id = 0 表示根目录。
namespace db {
namespace files {

struct Row {
    long long id = 0;
    long long parent_id = 0;
    std::string name;
    std::string kind;  // "file" 或 "folder"
    long long size = 0;
    long long updated_at = 0;
};

// 列出某目录下的子项：文件夹在前，同类按名称升序（UTF-8 字节序）。
bool list_children(long long owner_id, long long parent_id, std::vector<Row>& out, std::string& err);

// 按 R2 生成不冲突的名称：name、name (1)、name (2)…；超过 255 字符时截断原名保留后缀。
std::string unique_name(long long owner_id, long long parent_id, const std::string& desired_name);

// 查询当前用户名下的文件夹；folder_id = 0 视为根目录，恒为 true。
bool find_folder(long long owner_id, long long folder_id, std::string& out_name);

// 新建文件夹。直接尝试插入并依赖唯一索引防并发重名，冲突时换下一个候选名重试。
// 成功后 out_name 为最终落地名称（客户端必须使用它）。
bool create_folder(long long owner_id, long long parent_id, const std::string& name, long long& out_id,
                   std::string& out_name, std::string& err);

}  // namespace files
}  // namespace db
