// ============================================================
// pack.cpp — .vtp 打包/解包（liblzma LZMA2）+ 模块源码定位
// ============================================================
#include "pack.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#include <lzma.h>

namespace vortex {
namespace pack {

static const char kMagic[4] = {'V', 'T', 'P', 'D'};
static const uint32_t kVersion = 1;
static const uint32_t kCompStore = 0;
static const uint32_t kCompLzma = 1;

bool read_file(const std::string& path, std::vector<unsigned char>& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open '" + path + "'"; return false; }
    std::ostringstream oss; oss << f.rdbuf();
    const std::string& s = oss.str();
    out.assign(s.begin(), s.end());
    return true;
}

static bool write_all(const std::string& path, const std::vector<unsigned char>& data, std::string& err) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { err = "cannot write '" + path + "'"; return false; }
    f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
    f.flush();
    if (!f) { err = "write failed: '" + path + "'"; return false; }
    return true;
}

static void put_u32(std::vector<unsigned char>& d, uint32_t v) {
    d.push_back((unsigned char)(v & 0xff));
    d.push_back((unsigned char)((v >> 8) & 0xff));
    d.push_back((unsigned char)((v >> 16) & 0xff));
    d.push_back((unsigned char)((v >> 24) & 0xff));
}
static void put_u64(std::vector<unsigned char>& d, uint64_t v) {
    for (int i = 0; i < 8; ++i) d.push_back((unsigned char)((v >> (8 * i)) & 0xff));
}
static size_t rdpos = 0;
static bool get_u32(const std::vector<unsigned char>& d, uint32_t& v) {
    if (rdpos + 4 > d.size()) return false;
    v = (uint32_t)d[rdpos] | ((uint32_t)d[rdpos + 1] << 8) |
        ((uint32_t)d[rdpos + 2] << 16) | ((uint32_t)d[rdpos + 3] << 24);
    rdpos += 4; return true;
}
static bool get_u64(const std::vector<unsigned char>& d, uint64_t& v) {
    if (rdpos + 8 > d.size()) return false;
    v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)d[rdpos + i] << (8 * i);
    rdpos += 8; return true;
}

bool compress(const std::vector<unsigned char>& in, std::vector<unsigned char>& out, std::string& err) {
    if (in.empty()) { out.clear(); return true; }
    size_t bound = lzma_stream_buffer_bound(in.size());
    out.resize(bound);
    size_t out_pos = 0;
    lzma_ret r = lzma_easy_buffer_encode(6 /*preset*/, LZMA_CHECK_NONE, nullptr,
                                         in.data(), in.size(), out.data(), &out_pos, out.size());
    if (r != LZMA_OK) { err = "lzma compress failed (" + std::to_string((int)r) + ")"; return false; }
    out.resize(out_pos);
    return true;
}

bool decompress(const std::vector<unsigned char>& in, size_t raw_len,
                std::vector<unsigned char>& out, std::string& err) {
    if (in.empty()) { out.clear(); return true; }
    out.resize(raw_len);
    size_t in_pos = 0, out_pos = 0;
    uint64_t memlimit = UINT64_MAX;
    lzma_ret r = lzma_stream_buffer_decode(&memlimit, LZMA_TELL_NO_CHECK | LZMA_TELL_UNSUPPORTED_CHECK,
                                           nullptr, in.data(), &in_pos, in.size(),
                                           out.data(), &out_pos, out.size());
    if (r != LZMA_OK && r != LZMA_NO_CHECK) {
        err = "lzma decompress failed (" + std::to_string((int)r) + ")";
        return false;
    }
    out.resize(out_pos);
    return true;
}

bool write(const std::string& path, const std::vector<Entry>& entries, std::string& err) {
    std::vector<unsigned char> header;
    header.insert(header.end(), kMagic, kMagic + 4);
    put_u32(header, kVersion);
    put_u32(header, (uint32_t)entries.size());

    struct Meta { std::string name; uint32_t comp; uint64_t raw, stored, off; };
    std::vector<Meta> metas;
    std::vector<std::vector<unsigned char>> payloads;
    for (const auto& e : entries) {
        Meta m; m.name = e.name; m.raw = e.data.size();
        std::vector<unsigned char> stored;
        if (!compress(e.data, stored, err)) return false;
        m.comp = e.data.empty() || stored.size() >= e.data.size() ? kCompStore : kCompLzma;
        if (m.comp == kCompStore) stored = e.data;
        m.stored = stored.size();
        metas.push_back(m);
        payloads.push_back(std::move(stored));
    }

    // 数据区起始偏移
    uint64_t dataStart = 4 + 4 + 4;
    for (const auto& m : metas)
        dataStart += 4 + m.name.size() + 4 + 8 + 8 + 8;
    dataStart += entries.size() * 0; // header size already includes entry_count field

    uint64_t off = dataStart;
    for (auto& m : metas) { m.off = off; off += m.stored; }

    std::vector<unsigned char> body = header;
    for (const auto& m : metas) {
        put_u32(body, (uint32_t)m.name.size());
        body.insert(body.end(), m.name.begin(), m.name.end());
        put_u32(body, m.comp);
        put_u64(body, m.raw);
        put_u64(body, m.stored);
        put_u64(body, m.off);
    }
    for (size_t i = 0; i < metas.size(); ++i) {
        const auto& p = payloads[i];
        body.insert(body.end(), p.begin(), p.end());
    }
    return write_all(path, body, err);
}

bool read_entries(const std::string& path, std::vector<Entry>& out, std::string& err) {
    std::vector<unsigned char> file;
    if (!read_file(path, file, err)) return false;
    rdpos = 0;
    if (file.size() < 12 || memcmp(file.data(), kMagic, 4) != 0) {
        err = "not a vortex package: " + path; return false;
    }
    rdpos = 4;
    uint32_t ver, count;
    if (!get_u32(file, ver) || !get_u32(file, count)) { err = "bad header"; return false; }
    struct M { std::string name; uint32_t comp; uint64_t raw, stored, off; };
    std::vector<M> ms;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t nlen;
        if (!get_u32(file, nlen) || rdpos + nlen > file.size()) { err = "bad name"; return false; }
        M m; m.name.assign((const char*)file.data() + rdpos, nlen); rdpos += nlen;
        uint32_t comp; uint64_t raw, stored, off;
        if (!get_u32(file, comp) || !get_u64(file, raw) || !get_u64(file, stored) || !get_u64(file, off))
            { err = "bad meta"; return false; }
        m.comp = comp; m.raw = raw; m.stored = stored; m.off = off;
        ms.push_back(m);
    }
    for (const auto& m : ms) {
        if (m.off + m.stored > file.size()) { err = "meta out of range"; return false; }
        std::vector<unsigned char> stored(file.begin() + (ptrdiff_t)m.off,
                                          file.begin() + (ptrdiff_t)(m.off + m.stored));
        std::vector<unsigned char> raw;
        if (m.comp == kCompLzma) {
            if (!decompress(stored, (size_t)m.raw, raw, err)) return false;
        } else {
            raw = stored;
        }
        Entry e; e.name = m.name; e.data = std::move(raw);
        out.push_back(std::move(e));
    }
    return true;
}

bool read_entry(const std::string& path, const std::string& name,
                std::vector<unsigned char>& out, std::string& err) {
    std::vector<Entry> all;
    if (!read_entries(path, all, err)) return false;
    for (auto& e : all)
        if (e.name == name) { out = std::move(e.data); return true; }
    err.clear(); return false; // 条目不存在：不视为错误
}

bool find_module_source(const std::string& module, const std::string& base_dir,
                        std::string& out, std::string& err) {
    std::string mod = module;
    // 模块名里的点/斜杠 → 路径分隔
    for (auto& c : mod) if (c == '.') c = '/';
    std::string sep = "/";
    auto join = [&](const std::string& dir, const std::string& f) {
        if (dir.empty()) return f;
        if (dir.back() == '/' || dir.back() == '\\') return dir + f;
        return dir + sep + f;
    };
    std::vector<unsigned char> bytes;

    // 1) <module>.vt（脚本目录优先，其次 cwd）
    std::string vt = join(base_dir, mod + ".vt");
    if (read_file(vt, bytes, err)) { out.assign(bytes.begin(), bytes.end()); return true; }
    err.clear();
    vt = mod + ".vt";
    if (read_file(vt, bytes, err)) { out.assign(bytes.begin(), bytes.end()); return true; }
    err.clear();

    // 2) <module>.vtp 内名为 "<module>.vt" 的条目
    std::string vtp = join(base_dir, mod + ".vtp");
    std::vector<unsigned char> entry;
    if (read_entry(vtp, mod + ".vt", entry, err)) {
        out.assign(entry.begin(), entry.end());
        if (!out.empty() && out.back() != '\n') out.push_back('\n');
        return true;
    }
    err.clear();
    vtp = mod + ".vtp";
    if (read_entry(vtp, mod + ".vt", entry, err)) {
        out.assign(entry.begin(), entry.end());
        if (!out.empty() && out.back() != '\n') out.push_back('\n');
        return true;
    }
    err = "cannot find module '" + module + "' (looked for .vt and .vtp)";
    return false;
}

} // namespace pack
} // namespace vortex