// ============================================================
// CodeGen.h — vortex 原生编译器前端/LLVM 桥接接口
//
// 输入：已解析的 Program AST；输出：LLVM IR Module。
// 本头文件仅声明类型化 ABI；LLVM 类型细节在 CodeGen.cpp 内。
// ============================================================
#ifndef VORTEX_CODEGEN_H
#define VORTEX_CODEGEN_H

#include "ast.h"
#include <string>
#include <memory>

namespace vortex {

// 编译产物（对象/最终 exe 均由此管线生成）
struct BuildConfig {
    std::string output;        // 输出 exe 路径
    int opt_level = 2;         // 0..3
    bool emit_asm = false;     // 是否同时生成 .s 汇编
    bool debug = false;        // 是否生成 DWARF 调试信息（支持断点/变量查看）
    std::string triple;        // 空 = 宿主默认
};

// LLVM 后端命名空间（内部实现细节彼此独立）
namespace cg {

// 将 Program 编译为 LLVM IR 模块，返回错误消息（空=成功）。
// 失败时不产出 module；调用方可继续用解释器诊断信息。
std::string compile_program(const Program& prog, std::string& ir_text,
                            const BuildConfig& cfg);

} // namespace cg

// 顶层：把 .vt 源码编译为独立可执行文件。
// 返回 "" = 成功；否则返回错误描述。
std::string build_vortex_exe(const std::string& src, const BuildConfig& cfg);

} // namespace vortex

#endif // VORTEX_CODEGEN_H