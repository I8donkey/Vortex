// ============================================================
// vortexcc main.cpp — vortex 原生编译器 CLI 入口
//
//   vortexcc build <file.vt> [-o out.exe] [-O0|-O1|-O2|-O3]
//   vortexcc --help
// ============================================================
#include "codegen/CodeGen.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace vortex;

static std::string read_file(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open file '" + path + "'"; return {}; }
    std::ostringstream oss; oss << f.rdbuf();
    return oss.str();
}

static void print_usage(const char* prog) {
    std::printf("Vortex Native Compiler (LLVM backend)\n");
    std::printf("Usage:\n");
    std::printf("  %s build <file.vt> [-o out.exe] [-O0..-O3] [--asm] [--debug]\n", prog);
    std::printf("  %s --help\n", prog);
}

int main(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "--help") == 0 ||
        std::strcmp(argv[1], "-h") == 0) {
        print_usage(argv[0]);
        return argc < 2 ? 1 : 0;
    }

    if (std::strcmp(argv[1], "build") != 0) {
        std::fprintf(stderr, "Unknown subcommand '%s'\n", argv[1]);
        return 1;
    }
    if (argc < 3) { std::fprintf(stderr, "Missing input file\n"); return 1; }

    std::string in = argv[2];
    BuildConfig cfg;
    cfg.output = in + ".exe";

    // 解析可选参数
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) cfg.output = argv[++i];
        else if (a == "--asm") cfg.emit_asm = true;
        else if (a == "--debug") cfg.debug = true;
        else if (a == "-O0") cfg.opt_level = 0;
        else if (a == "-O1") cfg.opt_level = 1;
        else if (a == "-O2") cfg.opt_level = 2;
        else if (a == "-O3") cfg.opt_level = 3;
        else {
            std::fprintf(stderr, "Unknown option '%s'\n", a.c_str());
            return 1;
        }
    }

    std::string err;
    std::string src = read_file(in, err);
    if (src.empty()) { std::fprintf(stderr, "Error: %s\n", err.c_str()); return 1; }

    std::string cerr = build_vortex_exe(src, cfg);
    if (!cerr.empty()) {
        std::fprintf(stderr, "Compile error: %s\n", cerr.c_str());
        return 1;
    }
    std::printf("OK: %s\n", cfg.output.c_str());
    return 0;
}