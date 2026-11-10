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
    long long blob_id = 0;  // 仅 find_file 会填充；文件夹与列表结果为 0
};

// 列出某目录下的子项：文件夹在前，同类按名称升序（UTF-8 字节序）。
bool list_children(long long owner_id, long long parent_id, std::vector<Row>& out, std::string& err);

// 按 id 取当前账户下未删除的文件条目（kind='file'）；回收站内与不属于该账户的返回 false。
bool find_file(long long owner_id, long long file_id, Row& out);

// 按 R2 生成不冲突的名称：name、name (1)、name (2)…；超过 255 字符时截断原名保留后缀。
std::string unique_name(long long owner_id, long long parent_id, const std::string& desired_name);

// 重命名单个条目（文件或文件夹），保持原父目录不变。
// 仅允许当前账户下未删除的条目；不存在 / 越权 / 已在回收站时返回 false 且 err = "not_found"。
// 同名项已存在时按 R2 自动追加 (1)(2)…，最终名称通过 out_name 返回。
bool rename_file(long long owner_id, long long file_id, const std::string& desired_name,
                 std::string& out_name, std::string& err);

// 回收站条目：只包含子树根，parent_name 为父目录名（父目录已删除时也照常给出）。
struct TrashRow {
    long long id = 0;
    long long parent_id = 0;
    std::string name;
    std::string kind;
    long long size = 0;
    long long deleted_at = 0;
    long long expires_at = 0;  // deleted_at + 30 天
    std::string parent_name;
};

// 软删除整棵子树（统一 deleted_at），不扣配额。仅当前账户下未删除的条目可删。
bool soft_delete_subtree(long long owner_id, long long file_id, std::string& err);

// 回收站列表：只列"父节点未删除或本身就是根"的子树根，按删除时间倒序。
bool list_trash(long long owner_id, std::vector<TrashRow>& out, std::string& err);

// 恢复整棵子树到原目录；原父目录已删除/不存在时回到根目录，重名按 R2 自动改名。
bool restore_subtree(long long owner_id, long long file_id, std::string& out_name,
                     long long& out_parent_id, std::string& err);

// 彻底删除整棵子树：删除 files 行、blob 引用计数 -1，归零的 blob 记入 out_shas 供删磁盘。
bool purge_subtree(long long owner_id, long long file_id, std::vector<std::string>& out_shas,
                   std::string& err);

// 清理超过 30 天的回收站子树根；等价于对每个过期根执行 purge_subtree。
bool purge_expired(long long now, std::vector<std::string>& out_shas, std::string& err);

// 查询当前用户名下的文件夹；folder_id = 0 视为根目录，恒为 true。
bool find_folder(long long owner_id, long long folder_id, std::string& out_name);

// 新建文件夹。直接尝试插入并依赖唯一索引防并发重名，冲突时换下一个候选名重试。
// 成功后 out_name 为最终落地名称（客户端必须使用它）。
bool create_folder(long long owner_id, long long parent_id, const std::string& name, long long& out_id,
                   std::string& out_name, std::string& err);

// 该账户是否已拥有某 sha 的内容；回收站内条目也算（PRD R9/R10），用于秒传判定。
// 命中时把任意一条匹配的 files.id 写入 out_file_id；未命中返回 false 且 err 为空。
bool find_owned_by_sha(long long owner_id, const std::string& sha256, long long& out_file_id,
                       std::string& err);

// 为已有内容（blob_id）新建一个文件条目。名称按 R2 自动去重，blob 引用计数由调用方维护。
bool create_file_entry(long long owner_id, long long parent_id, const std::string& desired_name,
                       long long blob_id, long long size, long long& out_id,
                       std::string& out_name, std::string& err);

}  // namespace files
}  // namespace db
