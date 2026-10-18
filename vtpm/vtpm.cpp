// ============================================================
// vtpm — Vortex 包管理器：把 .vt 打包成压缩的 .vtp（vortex package）
//
//   vtpm pack [-o out.vtp] file1.vt [file2.vt ...]   # 打包
//   vtpm list <pkg.vtp>                              # 列出条目
//   vtpm extract <pkg.vtp> [-o dir]                  # 解包还原
// ============================================================
#include "pack.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <set>
#include <sstream>
#include <algorithm>
#include <filesystem>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

static void usage(const char* p) {
    std::printf("Vortex Package Tool (LZMA2 压缩)\n");
    std::printf("Usage:\n");
    std::printf("  %s pack [-o out.vtp] <file.vt> [file2.vt ...]\n", p);
    std::printf("  %s list <pkg.vtp>\n", p);
    std::printf("  %s extract <pkg.vtp> [-o dir]\n", p);
}

// ==================== 依赖收集（Lib-O3） ====================
// vtpm 不链接前端/解释器，故依赖来源用轻量文本/PE 扫描：
//   * .vt  -> 扫描 extdef ... from "库文件" 子句
//   * .exe/.dll -> 用 objdump -p 读取导入表（递归展开）

static std::string basename_of(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

static std::string dir_of(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string(".") : p.substr(0, s);
}

static std::string lower_ext(const std::string& p) {
    size_t s = p.find_last_of('.');
    if (s == std::string::npos) return "";
    std::string e = p.substr(s);
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    return e;
}

static bool file_exists(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (f) { std::fclose(f); return true; }
    return false;
}

// 解析相对依赖路径：绝对路径原样；否则按 baseDir 优先，再按 cwd
static std::string resolve_dep(const std::string& lib, const std::string& baseDir) {
    if (lib.size() >= 2 && lib[1] == ':') return lib;      // Windows 绝对路径
    if (!lib.empty() && (lib[0] == '/' || lib[0] == '\\')) return lib;
    std::string cand = baseDir + "/" + lib;
    if (file_exists(cand)) return cand;
    if (file_exists(lib)) return lib;
    return cand;
}

// 扫描 .vt 源码里的 extdef ... from "库文件" 子句（逐行、忽略 # 注释）
static std::vector<std::string> scan_vt_extdef_libs(const std::string& src) {
    std::vector<std::string> out;
    std::istringstream iss(src);
    std::string line;
    while (std::getline(iss, line)) {
        size_t h = line.find('#');
        if (h != std::string::npos) line = line.substr(0, h);
        if (line.find("extdef") == std::string::npos) continue;
        size_t f = line.find("from");
        if (f == std::string::npos) continue;
        size_t q1 = line.find('"', f);
        if (q1 == std::string::npos) continue;
        size_t q2 = line.find('"', q1 + 1);
        if (q2 == std::string::npos) continue;
        out.push_back(line.substr(q1 + 1, q2 - q1 - 1));
    }
    return out;
}

#ifdef _WIN32
// 执行命令并捕获 stdout
static std::string run_capture(const std::string& cmd) {
    std::string out;
    // 额外包裹一层引号：cmd.exe /c 对以引号开头的命令行会剥掉首尾引号
    FILE* f = _popen(("\"" + cmd + "\"").c_str(), "r");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    _pclose(f);
    return out;
}

// 用 objdump -p 读取 PE 导入的 DLL 名列表
static std::vector<std::string> scan_pe_imports(const std::string& file) {
    std::vector<std::string> names;
    std::string tools = "D:/gcc/bin";
    if (const char* gp = std::getenv("VORTEX_GCC_DIR")) tools = std::string(gp) + "/bin";
    // 归一化为绝对路径（objdump 对 "./" 前缀的路径会报错）
    std::error_code ec;
    std::string abs = std::filesystem::absolute(file, ec).lexically_normal().string();
    if (ec) abs = file;
    std::string text = run_capture("\"" + tools + "/objdump.exe\" -p \"" + abs + "\"");
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        size_t p = line.find("DLL Name:");
        if (p == std::string::npos) continue;
        std::string nm = line.substr(p + 9);
        size_t a = nm.find_first_not_of(" \t\r\n");
        size_t b = nm.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        names.push_back(nm.substr(a, b - a + 1));
    }
    return names;
}
#else
static std::vector<std::string> scan_pe_imports(const std::string&) { return {}; }
#endif


int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return 1; }
    std::string cmd = argv[1];
    using namespace vortex::pack;

    if (cmd == "pack") {
        std::string out = "out.vtp";
        std::vector<std::string> files;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "-o" && i + 1 < argc) { out = argv[++i]; }
            else files.push_back(a);
        }
        if (files.empty()) { std::fprintf(stderr, "no input files\n"); return 1; }
        std::vector<Entry> entries;
        std::string err;
        std::set<std::string> addedNames;   // 已加入条目的文件名（去重）
        auto add_entry = [&](const std::string& path, const std::string& name) -> bool {
            if (!addedNames.insert(name).second) return true;   // 同名条目已存在
            std::vector<unsigned char> data;
            if (!read_file(path, data, err)) return false;
            entries.push_back(Entry{name, data});
            return true;
        };
        // 收集某文件声明的依赖（.vt: extdef from；.exe/.dll: 导入表），结果入 depQueue
        std::vector<std::string> depQueue;
        auto collect_deps = [&](const std::string& file) -> bool {
            std::string ext = lower_ext(file);
            if (ext == ".vt") {
                std::vector<unsigned char> raw;
                if (!read_file(file, raw, err)) return false;
                std::string src(raw.begin(), raw.end());
                for (auto& lib : scan_vt_extdef_libs(src)) {
                    std::string dep = resolve_dep(lib, dir_of(file));
                    if (!file_exists(dep)) { err = "extdef library not found: " + lib; return false; }
                    depQueue.push_back(dep);
                }
            } else if (ext == ".exe" || ext == ".dll") {
                for (auto& nm : scan_pe_imports(file)) {
                    std::string cand = dir_of(file) + "/" + nm;
                    if (file_exists(cand)) depQueue.push_back(cand);   // 系统 DLL 找不到则跳过
                }
            }
            return true;
        };

        // 1. 输入文件本体
        for (auto& f : files)
            if (!add_entry(f, basename_of(f))) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
        // 2. 递归收集并打包依赖文件
        for (auto& f : files) if (!collect_deps(f)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
        std::set<std::string> depSeen;
        for (size_t i = 0; i < depQueue.size(); ++i) {
            std::string dep = depQueue[i];
            if (!depSeen.insert(dep).second) continue;
            if (!add_entry(dep, basename_of(dep))) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
            if (!collect_deps(dep)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
        }
        if (!write(out, entries, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
        std::printf("packed %zu file(s) -> %s\n", entries.size(), out.c_str());
        return 0;
    }

    if (cmd == "list" || cmd == "extract") {
        if (argc < 3) { usage(argv[0]); return 1; }
        std::string pkg = argv[2];
        std::string outdir = ".";
        if (cmd == "extract") {
            for (int i = 3; i < argc; ++i)
                if (std::string(argv[i]) == "-o" && i + 1 < argc) outdir = argv[++i];
        }
        std::vector<Entry> entries;
        std::string err;
        if (!read_entries(pkg, entries, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
        if (cmd == "list") {
            for (auto& e : entries) std::printf("%-30s %zu bytes\n", e.name.c_str(), e.data.size());
        } else {
#ifdef _WIN32
            _mkdir(outdir.c_str());
#else
            mkdir(outdir.c_str(), 0755);
#endif
            for (auto& e : entries) {
                std::string path = outdir + "/" + e.name;
                FILE* fo = std::fopen(path.c_str(), "wb");
                if (!fo) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
                std::fwrite(e.data.data(), 1, e.data.size(), fo);
                std::fclose(fo);
                std::printf("extracted %s\n", path.c_str());
            }
        }
        return 0;
    }

    usage(argv[0]);
    return 1;
}