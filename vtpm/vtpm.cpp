// ============================================================
// vtpm — Vortex 包管理器：把 .vt 打包成压缩的 .vtp（vortex package）
//
//   vtpm pack [-o out.vtp] file1.vt [file2.vt ...]   # 打包
//   vtpm list <pkg.vtp>                              # 列出条目
//   vtpm extract <pkg.vtp> [-o dir]                  # 解包还原
// ============================================================
#include "pack.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
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
        for (auto& f : files) {
            std::vector<unsigned char> data;
            if (!read_file(f, data, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
            std::string name = f;
            size_t slash = name.find_last_of("/\\");
            if (slash != std::string::npos) name = name.substr(slash + 1);
            entries.push_back(Entry{name, data});
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