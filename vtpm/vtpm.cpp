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
    std::printf("  %s pack [-o out.vtp] <vmake-project-dir>\n", p);
    std::printf("  %s list <pkg.vtp>\n", p);
    std::printf("  %s extract <pkg.vtp> [-o dir]\n", p);
    std::printf("Options:\n");
    std::printf("  --project [dir]   also pack every file referenced by the vmake project\n");
    std::printf("                    (default: the current directory, when it has VmakeLists.txt)\n");
    std::printf("  --no-project      pack the listed files only, ignoring an enclosing vmake project\n");
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

// ==================== vmake 工程扫描 ====================
// When a directory contains VmakeLists.txt it is a vmake project: pack every
// script (VmakeLists.txt / *.vmake) plus every file the scripts reference,
// keeping paths relative to the project root. This is a text scan on purpose,
// mirroring the extdef scanning above: vtpm stays independent of vmake.

static bool is_abs_path(const std::string& p) {
    if (p.size() >= 2 && p[1] == ':') return true;                 // Windows drive
    return !p.empty() && (p[0] == '/' || p[0] == '\\');
}

static std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty() || a == ".") return b;
    if (is_abs_path(b)) return b;
    return a + "/" + b;
}

static bool is_regular_file(const std::string& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

static bool is_directory(const std::string& p) {
    std::error_code ec;
    return std::filesystem::is_directory(p, ec);
}

static std::string normalize_rel(const std::string& p) {
    std::string s = p;
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.size() > 2 && s[0] == '.' && s[1] == '/') s = s.substr(2);
    return s;
}

// Split one script line into tokens: quoted strings stay whole, '(' ')' ','
// and whitespace separate tokens, '#' starts a comment.
static std::vector<std::string> script_tokens(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool inQuote = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (inQuote) {
            if (c == '\\' && i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
            else if (c == '"') { inQuote = false; out.push_back(cur); cur.clear(); }
            else cur += c;
            continue;
        }
        if (c == '"') { inQuote = true; continue; }
        if (c == '#' && cur.empty()) break;                        // comment (outside quotes)
        if (c == '(' || c == ')' || c == ',' || c == ' ' || c == '\t' || c == '\r') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Collect the scripts and referenced files of a vmake project rooted at root.
// Each result is (absolute path, entry name relative to root).
static bool collect_vmake_project(const std::string& root,
                                  std::vector<std::pair<std::string, std::string> >& out,
                                  std::string& err) {
    std::set<std::string> visitedScripts;
    std::set<std::string> seenEntries;
    std::vector<std::string> queue;
    queue.push_back(join_path(root, "VmakeLists.txt"));

    while (!queue.empty()) {
        std::string script = queue.back();
        queue.pop_back();
        std::error_code ec;
        std::string canon = std::filesystem::weakly_canonical(script, ec).string();
        if (canon.empty()) canon = script;
        if (!visitedScripts.insert(canon).second) continue;

        std::vector<unsigned char> raw;
        if (!vortex::pack::read_file(script, raw, err)) return false;

        auto add_file = [&](const std::string& absPath) {
            std::error_code rec;
            std::string rel = std::filesystem::relative(absPath, root, rec).string();
            if (rec || rel.empty() || rel.find("..") == 0) rel = basename_of(absPath);
            rel = normalize_rel(rel);
            if (!seenEntries.insert(rel).second) return;
            out.push_back(std::make_pair(absPath, rel));
        };
        add_file(script);

        // Directory arguments (e.g. target_include_directories) contribute headers.
        auto add_dir_headers = [&](const std::string& dir) {
            std::error_code dec;
            for (std::filesystem::recursive_directory_iterator it(dir, dec), end;
                 it != end && !dec; it.increment(dec)) {
                if (!it->is_regular_file(dec)) continue;
                std::string ext = lower_ext(it->path().string());
                if (ext == ".h" || ext == ".hh" || ext == ".hpp" || ext == ".hxx")
                    add_file(it->path().string());
            }
        };

        const std::string scriptDir = dir_of(script);
        std::string src(raw.begin(), raw.end());
        std::istringstream iss(src);
        std::string line;
        while (std::getline(iss, line)) {
            std::vector<std::string> toks = script_tokens(line);
            if (toks.size() < 2) continue;
            const std::string cmd = toks[0];

            if (cmd == "add_subdirectory") {
                for (size_t i = 1; i < toks.size(); ++i) {
                    if (toks[i].empty() || toks[i][0] == '$') continue;
                    std::string sub = join_path(scriptDir, toks[i]);
                    std::string subScript = join_path(sub, "VmakeLists.txt");
                    if (is_regular_file(subScript)) { queue.push_back(subScript); break; }
                }
                continue;
            }
            if (cmd == "include") {
                for (size_t i = 1; i < toks.size(); ++i) {
                    if (toks[i].empty() || toks[i][0] == '$') continue;
                    std::string cand = join_path(scriptDir, toks[i]);
                    std::string withExt = cand + ".vmake";
                    if (is_regular_file(cand)) { queue.push_back(cand); break; }
                    if (is_regular_file(withExt)) { queue.push_back(withExt); break; }
                }
                continue;
            }
            for (size_t i = 1; i < toks.size(); ++i) {
                const std::string& t = toks[i];
                if (t.empty() || t[0] == '$' || t[0] == '-') continue;
                std::string cand = join_path(scriptDir, t);
                if (is_regular_file(cand)) { add_file(cand); continue; }
                if (is_directory(cand)) { add_dir_headers(cand); continue; }
                cand = join_path(root, t);                       // fall back to project root
                if (is_regular_file(cand)) { add_file(cand); continue; }
                if (is_directory(cand)) add_dir_headers(cand);
            }
        }
    }
    return true;
}


int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return 1; }
    std::string cmd = argv[1];
    using namespace vortex::pack;

    if (cmd == "pack") {
        std::string out = "out.vtp";
        std::vector<std::string> files;
        std::string projectDir;
        bool noProject = false;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "-o" && i + 1 < argc) { out = argv[++i]; }
            else if (a == "--project") {
                projectDir = ".";
                if (i + 1 < argc && argv[i + 1][0] != '-') projectDir = argv[++i];
            }
            else if (a == "--no-project") { noProject = true; }
            else files.push_back(a);
        }
        // A directory argument that holds a VmakeLists.txt is a vmake project root.
        std::vector<std::string> plain;
        for (size_t i = 0; i < files.size(); ++i) {
            if (is_directory(files[i]) && is_regular_file(join_path(files[i], "VmakeLists.txt")))
                projectDir = files[i];
            else
                plain.push_back(files[i]);
        }
        files = plain;
        // Default: pack the whole project when the current directory is one.
        if (!noProject && projectDir.empty() && is_regular_file("VmakeLists.txt")) projectDir = ".";
        if (files.empty() && projectDir.empty()) {
            std::fprintf(stderr, "no input files (pass a .vt file, a vmake project dir, or run inside one)\n");
            return 1;
        }
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

        // 1. vmake 工程：脚本与脚本引用到的全部文件（相对路径作为条目名）
        std::vector<std::pair<std::string, std::string> > projectFiles;
        if (!projectDir.empty()) {
            if (!collect_vmake_project(projectDir, projectFiles, err)) {
                std::fprintf(stderr, "error: %s\n", err.c_str()); return 1;
            }
            std::printf("vmake project: %s (%zu file(s))\n", projectDir.c_str(), projectFiles.size());
        }
        // 2. 显式输入的文件本体（条目名取 basename）
        for (auto& f : files)
            if (!add_entry(f, basename_of(f))) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
        for (size_t i = 0; i < projectFiles.size(); ++i)
            if (!add_entry(projectFiles[i].first, projectFiles[i].second)) {
                std::fprintf(stderr, "error: %s\n", err.c_str()); return 1;
            }
        // 3. 递归收集并打包依赖文件（.vt extdef / PE 导入表）
        for (auto& f : files) if (!collect_deps(f)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
        for (size_t i = 0; i < projectFiles.size(); ++i)
            if (!collect_deps(projectFiles[i].first)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
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
            std::error_code dec;
            std::filesystem::create_directories(outdir, dec);
            for (auto& e : entries) {
                // Entries may carry a relative path (vmake project mode); refuse
                // absolute paths and '..' so extraction cannot escape outdir.
                if (e.name.empty() || is_abs_path(e.name) || e.name.find("..") != std::string::npos) {
                    std::fprintf(stderr, "skipped unsafe entry: %s\n", e.name.c_str());
                    continue;
                }
                std::string path = join_path(outdir, e.name);
                std::filesystem::path pp(path);
                if (pp.has_parent_path()) std::filesystem::create_directories(pp.parent_path(), dec);
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