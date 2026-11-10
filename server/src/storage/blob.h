#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 分片落盘、扫描、合并与内容寻址存储。
// 所有路径都由服务端生成，客户端的任何字符串都不参与路径拼接。
namespace storage {

// 内容落盘路径：<data_dir>/blob/<sha[0:2]>/<sha[2:4]>/<sha256>。
std::string blob_path(const std::string& data_dir, const std::string& sha256);

// 把一片原始字节写入 <session_dir>/<index>.chunk。
// 先写 <index>.chunk.tmp，fsync 后 rename，避免半截文件被当成已收分片。
// bytes.size() 必须等于 expected_len，否则返回 false 且不留下文件。
bool write_chunk(const std::string& session_dir, int index, const std::string& bytes,
                 std::int64_t expected_len, std::string& err);

// 扫描会话目录，返回已收分片下标（升序）。
// 只读文件元信息：文件不存在或大小不等于该片应有长度，均视为未收。
bool scan_received(const std::string& session_dir, int chunk_count, std::int64_t chunk_size,
                   std::int64_t total_size, std::vector<int>& out_indices, std::string& err);

// 顺序读分片并写入 out_path，同时流式计算 SHA-256（小写十六进制）。
// 任何分片缺失或长度不符都返回 false，并删除写了一半的 out_path。
bool merge_and_hash(const std::string& session_dir, int chunk_count, std::int64_t chunk_size,
                    std::int64_t total_size, const std::string& out_path,
                    std::string& out_sha256, std::string& err);

// 把临时合并文件提交到内容寻址路径。
// 目标已存在且大小一致时只删除临时文件（同一内容只存一份）；否则 rename 落位。
bool commit_blob(const std::string& temp_path, const std::string& data_dir,
                 const std::string& sha256, std::string& err);

// 递归删除目录或文件；不存在视为成功。
bool remove_tree(const std::string& path);

}  // namespace storage
