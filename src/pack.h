// ============================================================
// pack.h — Vortex 打包 (.vtp) 容器读写 + 模块源码定位
//
// .vtp 格式（vortex package，小端）:
//   magic "VTPD"(4)  u32 version=1  u32 entry_count
//   每条目: u32 name_len; name[utf8]; u32 comp(0=store,1=lzma2)
//           u64 raw_len; u64 stored_len; u64 offset
//   之后数据区（每个条目 stored_len 字节，LZMA2 压缩或原文）
//
// 压缩用 liblzma(lzma_easy_buffer_encode, LZMA2)；不依赖外部 7z.exe。
// ============================================================
#ifndef VORTEX_PACK_H
#define VORTEX_PACK_H

#include <string>
#include <vector>

namespace vortex {
namespace pack {

// 一个包条目（data 为解压后的原始内容）
struct Entry {
    std::string name;
    std::vector<unsigned char> data;
};

// 从路径读取文件全部字节；失败返回 false 并填 err
bool read_file(const std::string& path, std::vector<unsigned char>& out, std::string& err);

// 把 entries 写成一个 .vtp 文件（每个条目压缩为 LZMA2）
bool write(const std::string& path, const std::vector<Entry>& entries, std::string& err);

// 打开 .vtp，读取全部条目（自动解压）；失败返回 false 并填 err
bool read_entries(const std::string& path, std::vector<Entry>& out, std::string& err);

// 从 .vtp 读取单个条目（名称精确匹配）；找不到返回 false，err 为空但 ok=false
bool read_entry(const std::string& path, const std::string& name,
                std::vector<unsigned char>& out, std::string& err);

// 压缩/解压单块数据（LZMA2）. 失败返回 false 并填 err
bool compress(const std::vector<unsigned char>& in, std::vector<unsigned char>& out, std::string& err);
bool decompress(const std::vector<unsigned char>& in, size_t raw_len,
                std::vector<unsigned char>& out, std::string& err);

// 定位并读取模块源码文本：
//   先在 base_dir 下找 <module>.vt，其次 <module>.vtp 里名为 "<module>.vt" 的条目。
// 返回 true 且 out=源码；否则 false，err 描述原因。
bool find_module_source(const std::string& module, const std::string& base_dir,
                        std::string& out, std::string& err);

} // namespace pack
} // namespace vortex

#endif // VORTEX_PACK_H