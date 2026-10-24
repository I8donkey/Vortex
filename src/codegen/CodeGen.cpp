// ============================================================
// CodeGen.cpp — vortex LLVM 后端（P1 子集）
//
// 支持：int/float/bool/str 字面量与变量、算术/比较/逻辑运算、
//       if/while、用户 def 函数（int/double 参数）、print、math 初值。
// 标量直通原生寄存器；字符串经 VStr*（引用计数运行时）收发。
// ============================================================
#include "codegen/CodeGen.h"
#include "codegen/ModuleLoader.h"

#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "../suggest.h"

#include <limits>
#include <cmath>

// LLVM
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/Config/llvm-config.h>

// LLVM 大版本间的 API 差异（CI 上可能拿到 17，本地/macOS/Windows 为 23，需同时兼容）：
//   * 17 起 Host/Triple 头文件从 llvm/Support/ 迁到 llvm/TargetParser/
//   * 18 起 CodeGenOptLevel 取代 CodeGenOpt::Level（17 及更早没有该枚举）
//   * 18 起 CodeGenFileType 变为 enum class（成员名 ObjectFile；更早为 CGFT_ObjectFile）
//   * 较新版本（约 22 起）lookupTarget/createTargetMachine 首参改为 const llvm::Triple&
#if LLVM_VERSION_MAJOR >= 17
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#else
#include <llvm/Support/Host.h>
#include <llvm/Support/Triple.h>
#endif

#include <cstdio>
#include <memory>
#include <fstream>
#include <string>
#include <vector>
#include <optional>
#include <set>
#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace vortex {
namespace cg {

// 可靠地判断基本块是否已含终止指令（遍历，避免 getTerminator 在空块上的不可靠行为）
static bool block_has_term(llvm::BasicBlock* bb) {
    if (!bb) return false;
    for (auto& I : *bb)
        if (I.isTerminator()) return true;
    return false;
}

// ==================== operator 重载辅助 ====================
// 重载表 key（与解释器口径一致）：一元 "op#T"，二元 "op#T#U"
static std::string op_key(const std::string& op, const std::string& lt,
                          const std::string& rt, bool unary) {
    return unary ? (op + "#" + lt) : (op + "#" + lt + "#" + rt);
}

// 类型名规范化：把别名映射为解释器使用的基础类型名（class 名原样保留）
static std::string canon_type_name(const std::string& n) {
    if (n == "float" || n == "double" || n == "f32" || n == "f64") return "float";
    if (n == "str" || n == "string" || n == "unistr") return "str";
    if (n == "bool") return "bool";
    if (n == "int" || n == "long" || n == "uint" || n == "ulong") return "int";
    return n;
}

// 运算符助记名：生成唯一函数名，避免符号字符出现在 LLVM 标识符中
static std::string op_mnemonic(const std::string& op) {
    if (op == "+")  return "add";
    if (op == "-")  return "sub";
    if (op == "*")  return "mul";
    if (op == "/")  return "div";
    if (op == "//") return "idiv";
    if (op == "%")  return "mod";
    if (op == "**") return "pow";
    if (op == "&")  return "band";
    if (op == "|")  return "bor";
    if (op == "^")  return "bxor";
    if (op == "<<") return "shl";
    if (op == ">>") return "shr";
    if (op == "&&") return "land";
    if (op == "||") return "lor";
    if (op == "==") return "eq";
    if (op == "!=") return "ne";
    if (op == "<")  return "lt";
    if (op == ">")  return "gt";
    if (op == "<=") return "le";
    if (op == ">=") return "ge";
    if (op == "!")  return "not";
    return "unk";
}

// 内建错误类型名（raise Error("...") / catch (Type e)）
static bool is_builtin_error_type(const std::string& n) {
    static const char* k[] = {"Error", "TypeError", "ValueError", "IndexError", "KeyError",
                              "ZeroDivisionError", "NameError", "RuntimeError",
                              "AssertionError", "SintaxError", "SyntaxError"};
    for (const char* s : k) if (n == s) return true;
    return false;
}

// ==================== 静态类型 ====================
enum class VType { Int, Float, Bool, Str, List, StrList, StrListList, Dict, Set, Pair, Tuple, Object, Ref, Closure, ExtFloat, Void };

struct LVal {
    VType type;
    llvm::Value* val = nullptr;     // 值 (标量) 或指针 (Str)
    llvm::Value* slot = nullptr;    // 变量槽（alloca 或模块全局，用于读写）
    VType ref_pointee = VType::Void;  // type==Ref 时的被引用变量类型（Void 表未知/非引用）
    std::string obj_class;          // type==Object 时的静态类名（供字段/方法静态解析）
    std::string type_desc;          // type() 用的静态类型描述（元组等；空 = 由类型推导）
};

// ==================== 代码生成器 ====================
class Gen {
public:
    Gen(const Program& p, const BuildConfig& cfg)
        : prog_(p), cfg_(cfg) {
        fns_["print"] = std::make_pair(VType::Void, Builtin::Print);
        fns_["math.pow"] = std::make_pair(VType::Float, Builtin::Pow);
        fns_["math.sqrt"] = std::make_pair(VType::Float, Builtin::Sqrt);
        fns_["math.fabs"] = std::make_pair(VType::Float, Builtin::Fabs);
    }

    // 生成整个 module；返回错误（空=成功）
    std::string run(llvm::Module& mod);

private:
    enum class Builtin { Print, Pow, Sqrt, Fabs, None };
    const Program& prog_;
    const BuildConfig& cfg_;
    llvm::Module* mod_ = nullptr;
    llvm::IRBuilder<>* B = nullptr;

    // 当前函数的变量表: name -> (type, alloca 指针)
    std::vector<std::unordered_map<std::string, LVal>> scopes_;
    // 是否位于顶层(全局)作用域：为 true 时变量提升为模块全局，供函数跨作用域访问
    bool top_level_ = false;
    // 当前函数返回类型
    VType fn_ret_ = VType::Void;
    // 每函数推断出的返回类型（key=LLVM 函数名，如用户模块 "foo.add"）
    std::unordered_map<std::string, VType> fn_ret_map_;
    llvm::Function* cur_fn_ = nullptr;
    // lambda 序列号（生成唯一匿名函数名）
    int lam_seq_ = 0;
    // 内建函数表
    std::unordered_map<std::string, std::pair<VType, Builtin>> fns_;

    // ---- class 声明（单继承；运行时装箱 + 静态方法解析）----
    // 类表：类名 -> 父类 / 自有字段 / 方法（方法声明在 Program 内保活）
    struct ClassInfo {
        std::string name;
        std::string parent;                                       // 空 = 无父类
        std::vector<std::pair<std::string, std::string>> fields;  // (类型, 字段名)，仅自有
        std::vector<const FunctionDefStmt*> methods;
    };
    std::unordered_map<std::string, ClassInfo> classes_;
    // 预处理：扫描顶层 class 声明，构建 classes_
    void collect_classes();
    const ClassInfo* find_class(const std::string& n) const;
    // 沿父链查找方法（子类优先）；owner 回传定义所在类名
    const FunctionDefStmt* find_method(const std::string& cls, const std::string& name,
                                       std::string* owner = nullptr) const;
    // 取/建某 class 的运行时类对象全局 g_cls_<name>
    llvm::GlobalVariable* class_global(const std::string& name);
    // 常量 C 字符串（字段名/方法名/类名用）
    llvm::Value* cstr(const std::string& s);
    // 生成一个 class：运行时登记（类/父类/自有字段）+ 生成各方法函数
    void gen_class(const ClassDeclStmt* cd);
    // 生成一个方法函数：签名 (self, 形参...) → 推断返回类型
    void gen_method_fn(const std::string& cls, const FunctionDefStmt* fd);
    // 构造实例：vor_obj_new + 沿父链调用 init
    LVal gen_construct(const std::string& cls, const CallExpr* c);
    // 实例方法调用：静态解析目标 + 直接调用（自动前置 self）
    LVal gen_method_call(const LVal& obj, const std::string& name, const CallExpr* c);
    // 实例字段读取
    LVal gen_obj_field_read(const LVal& obj, const std::string& member);
    // 把实参规整为目标形参类型
    llvm::Value* coerce_arg(LVal v, VType target);
    // 把值规整为字符串值（raise 的消息 / 字符串拼接复用）
    llvm::Value* to_vstr(LVal v);
    // 常量 C 字符串 -> VStr*
    llvm::Value* vstr_const(const std::string& s);

    // ---- operator 重载（顶层声明；静态分派）----
    // 重载表：key = op_key(op, lt, rt, unary) -> 声明（声明在 Program 内保活）
    std::unordered_map<std::string, const OperatorDeclStmt*> op_overloads_;
    // 预处理：扫描顶层 operator 声明，构建 op_overloads_
    void collect_operators();
    // 类型名 -> VType（class 名 -> Object）
    VType type_from_name(const std::string& n) const;
    // LVal 的静态类型名（class 实例 -> 类名，其余 -> 基础类型名；未知 -> ""）
    std::string lval_type_name(const LVal& v) const;
    // operator 函数名（唯一、可打印）
    std::string op_fn_name(const OperatorDeclStmt* od) const;
    // 生成一个 operator 函数
    void gen_operator_fn(const OperatorDeclStmt* od);
    // 查找重载：精确匹配 (lt, rt)，再沿左/右操作数的类父链回退
    const OperatorDeclStmt* find_operator_decl(const std::string& op, const std::string& lt,
                                               const std::string& rt, bool unary) const;
    // 调用重载：构造实参并按声明返回类型收尾
    LVal call_operator_fn(const OperatorDeclStmt* od, const LVal& l, const LVal& r);

    // ---- 外部 C 函数（Lib-O3）----
    // extdef 表：名字 -> 声明（声明在 Program 内保活）
    std::unordered_map<std::string, const ExtDefStmt*> extdefs_;
    // 预处理：扫描顶层 extdef 声明，构建 extdefs_
    void collect_extdefs();
    // C 类型名 -> LLVM 类型（cint/cuint→i32、clong/culong→i64、cdouble→f64、cstr/cptr→ptr）
    llvm::Type* c_abi_type(const std::string& n) const;
    // 发射 extdef 的 LLVM 函数声明（外部链接）
    void gen_extdef(const ExtDefStmt* ed);
    // 外部函数调用：实参/返回值按 C ABI 转换
    LVal gen_ext_call(const ExtDefStmt* ed, const CallExpr* c);

    // ---- auto 解构绑定 ----
    // auto [a, b, c] = 表达式;  元素按位置绑定为 int（tuple/pair/list/实例字段）
    void gen_destructure(const VarDeclStmt* vd);

    // ---- type() 静态类型描述 ----
    // 表达式 -> 类型名（元组为 auto_generated_class(元素类型,...)，与解释器口径一致）
    std::string type_desc_of_expr(const Expr* e);
    // 静态类型 -> 类型名字符串（对齐解释器 Value::type_name）
    static std::string vtype_desc(VType t);

    // ---- 调试信息（P3，DIBuilder 发射 DWARF） ----
    llvm::DIBuilder* DB = nullptr;
    llvm::DICompileUnit* CU = nullptr;
    llvm::DIFile* DIF = nullptr;
    llvm::DISubprogram* cur_sp_ = nullptr;
    unsigned dbg_line_ = 0;
    bool debug_on() const { return cfg_.debug; }

    // 运行时函数声明缓存
    llvm::Function* declare_runtime(const char* name, llvm::Type* ret, std::vector<llvm::Type*> params, bool vararg=false);
    llvm::Value* decl_vor(const char* name, VType ret, std::vector<VType> params);

    // 表达式/语句生成
    LVal gen_expr(const Expr* e);
    void gen_stmt(const Stmt* s);
    void gen_block(const std::vector<StmtPtr>& body);
    // 闭包（P3）：生成 lambda 匿名函数并构造闭包值
    LVal gen_lambda(const LambdaExpr* le);
    void gen_return(const ReturnStmt* s);

    // 变量
    llvm::AllocaInst* make_alloca(VType t, llvm::Function* f, const char* nm);
    llvm::Type* llvm_type(VType t) const;
    bool lookup_var(const std::string& n, LVal& out) const;

    // 二进制运算
    LVal gen_binop(const BinaryOpExpr* b);
    // 精确十进制二元/比较运算（任一操作数为 extfloat）
    LVal gen_ext_binop(const std::string& op, LVal l, LVal r);

    // 内置函数调用
    LVal gen_call(const CallExpr* c);
    // 输出一个值到 stdout
    void emit_any(LVal v);
    // 把值规整为 i64 槽位（布尔零扩展、浮点按位拷贝）
    llvm::Value* to_i64(LVal v);
    // 用户函数推断返回类型（P1 简化：返回最后 return 的类型/当前记录）
    VType fn_ret_of(const std::string& fnname);
    // 静态推断一个表达式的大致类型（供函数返回类型推断；不发射代码）
    VType infer_expr_type(const Expr* e);
    // 推断函数的返回类型（遍历 return 语句），未知→Int
    VType infer_fn_ret(const FunctionDefStmt* fd);
    // 收集可用于"did you mean"的标识符候选（作用域 + 内建 + 常见模块名）
    std::vector<std::string> ident_candidates() const;

    // ---- 调试信息辅助（P3） ----
    void dbg_begin(llvm::Module& mod);                   // 创建 DIBuilder/CU/DIFile
    void dbg_end();                                      // finalize + 释放 DIBuilder
    llvm::DISubprogram* dbg_begin_func(llvm::Function* f, const char* nm);
    void dbg_step_line();                                // 推进近似行号并设置 DebugLoc
    void dbg_locate(unsigned line);                      // 以给定行号设置 DebugLoc(作用域=cur_sp_)
    void dbg_declare(const std::string& nm, VType t, llvm::Value* slot);
    llvm::DIType* dbg_dtype(VType t);

    // 控制流引用
    llvm::BasicBlock* cur_bb() const { return B->GetInsertBlock(); }
};

// ---------- LLVM 类型映射 ----------
llvm::Type* Gen::llvm_type(VType t) const {
    switch (t) {
        case VType::Int:   return B->getInt64Ty();
        case VType::Float: return B->getDoubleTy();
        case VType::Bool:  return B->getInt1Ty();
        case VType::Str:   return B->getPtrTy();          // VStr*
        case VType::List:  return B->getPtrTy();          // VList*
        case VType::StrList: return B->getPtrTy();        // VList*（字符串列表）
        case VType::StrListList: return B->getPtrTy();    // VList*（嵌套字符串列表）
        case VType::Dict:  return B->getPtrTy();          // VDict*
        case VType::Set:   return B->getPtrTy();          // VList* (集合复用)
        case VType::Pair:  return B->getPtrTy();          // VPair*
        case VType::Tuple: return B->getPtrTy();          // VTuple*
        case VType::Object:return B->getPtrTy();          // VObj*（class 实例）
        case VType::ExtFloat: return B->getPtrTy();       // VStr*（规范十进制文本）
        case VType::Closure: return B->getPtrTy();        // RT_Closure*
        case VType::Void:  return B->getVoidTy();
    }
    return B->getVoidTy();
}

// ---------- 调试信息（P3：DIBuilder 发射 DWARF） ----------
void Gen::dbg_begin(llvm::Module& mod) {
    DB = new llvm::DIBuilder(mod);
    dbg_line_ = 0;
    cur_sp_ = nullptr;
    // 伪源文件名：扩展名不影响 DWARF 使用（gdb 可据此定位）
    DIF = DB->createFile("vortex.vt", ".");
    CU = DB->createCompileUnit(llvm::dwarf::DW_LANG_C99, DIF, "vortexcc", false,
                               "", 0);
}

void Gen::dbg_end() {
    if (!DB) return;
    DB->finalize();
    delete DB;
    DB = nullptr;
}

// 函数级 DI：为每个函数创建 DISubprogram，并把 DebugLoc 作用域切到该函数
llvm::DISubprogram* Gen::dbg_begin_func(llvm::Function* f, const char* nm) {
    if (!DB) return nullptr;
    ++dbg_line_;
    llvm::DISubroutineType* st = DB->createSubroutineType(
        DB->getOrCreateTypeArray(llvm::ArrayRef<llvm::Metadata*>()));
    llvm::DISubprogram* sp = DB->createFunction(
        DIF, nm, f->getName(), DIF, dbg_line_,
        st, dbg_line_, llvm::DINode::FlagZero, llvm::DISubprogram::SPFlagDefinition);
    f->setSubprogram(sp);
    cur_sp_ = sp;
    // 函数入口基本块指令定位到本函数行
    if (B) B->SetCurrentDebugLocation(llvm::DILocation::get(B->getContext(), dbg_line_, 0, sp));
    return sp;
}

// 推进近似行号并设置 DebugLoc（AST 未携带行号，用单调计数器近似）
void Gen::dbg_step_line() {
    if (!DB) return;
    ++dbg_line_;
    dbg_locate(dbg_line_);
}

void Gen::dbg_locate(unsigned line) {
    if (!DB || !cur_sp_) { return; }
    B->SetCurrentDebugLocation(
        llvm::DILocation::get(B->getContext(), line, 0, cur_sp_));
}

llvm::DIType* Gen::dbg_dtype(VType t) {
    if (!DB) return nullptr;
    switch (t) {
        case VType::Int:   return DB->createBasicType("int", 64, llvm::dwarf::DW_ATE_signed);
        case VType::Float: return DB->createBasicType("float", 64, llvm::dwarf::DW_ATE_float);
        case VType::Bool:  return DB->createBasicType("bool", 1, llvm::dwarf::DW_ATE_boolean);
        case VType::Str:   return DB->createBasicType("str", 64, llvm::dwarf::DW_ATE_unsigned);
        case VType::Void:  return DB->createBasicType("void", 0, llvm::dwarf::DW_ATE_unsigned);
        default:           return DB->createBasicType("ptr", 64, llvm::dwarf::DW_ATE_unsigned);
    }
}

void Gen::dbg_declare(const std::string& nm, VType t, llvm::Value* slot) {
    // 注：本 LLVM 23 构建的 SelectionDAG 无法低调度旧式 llvm.dbg.declare 内建
    // （会走 visitTargetIntrinsic 崩溃），且新调试信息格式无经典开关。因此此处
    // 仅保留函数级 DISubprogram 与 DILocation（行号/断点/调用栈可用），逐变量
    // 声明由函数级信息替代，暂不发射 if it存在声明冲突。
    (void)nm; (void)t; (void)slot;
}

// ---------- 运行时函数 ----------
llvm::Function* Gen::declare_runtime(const char* name, llvm::Type* ret, std::vector<llvm::Type*> params, bool vararg) {
    // 复用已声明的同名函数，避免 LLVM 自动追加 .1/.2 编号导致符号名不匹配
    if (llvm::Function* existing = mod_->getFunction(name); existing)
        return existing;
    llvm::FunctionType* FT = llvm::FunctionType::get(ret, params, vararg);
    return llvm::Function::Create(FT, llvm::Function::ExternalLinkage, name, mod_);
}

llvm::Value* Gen::decl_vor(const char* name, VType ret, std::vector<VType> params) {
    std::vector<llvm::Type*> pts;
    for (auto& p : params) pts.push_back(llvm_type(p));
    return declare_runtime(name, llvm_type(ret), pts);
}

// ---------- 变量表 ----------
bool Gen::lookup_var(const std::string& n, LVal& out) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto f = it->find(n);
        if (f != it->end()) { out = f->second; return true; }
    }
    return false;
}

llvm::AllocaInst* Gen::make_alloca(VType t, llvm::Function* f, const char* nm) {
    llvm::IRBuilderBase::InsertPointGuard guard(*B);
    llvm::BasicBlock& entry = f->getEntryBlock();
    B->SetInsertPoint(&entry, entry.begin());
    return B->CreateAlloca(llvm_type(t), nullptr, nm);
}

// ==================== 表达式 ====================
LVal Gen::gen_expr(const Expr* e) {
    switch (e->kind) {
        case ExprKind::Literal: {
            auto* l = static_cast<const LiteralExpr*>(e);
            switch (l->lit_kind) {
                case LiteralExpr::LitKind::Int:   return {VType::Int,   B->getInt64(l->int_val)};
                case LiteralExpr::LitKind::Float: return {VType::Float, llvm::ConstantFP::get(B->getDoubleTy(), l->float_val)};
                case LiteralExpr::LitKind::ExtFloat: {
                    // 精确十进制字面量：按原始文本交给运行时解析（不经 double，避免精度损失）
                    llvm::Value* fn = decl_vor("vor_ext_from_cstr", VType::ExtFloat, {VType::Str});
                    return {VType::ExtFloat, B->CreateCall(llvm::cast<llvm::Function>(fn),
                            {B->CreateGlobalString(l->str_val, "ext")})};
                }
                case LiteralExpr::LitKind::Bool:  return {VType::Bool,  B->getInt1(l->bool_val)};
                case LiteralExpr::LitKind::String: {
                    llvm::Value* fn = decl_vor("vor_str_from_cstr", VType::Str, {VType::Str});
                    llvm::Value* cstr = B->CreateGlobalString(l->str_val, "str"); // LLVM 23: 原 CreateGlobalStringPtr
                    return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {cstr})};
                }
                default: break;
            }
            break;
        }
        case ExprKind::Identifier: {
            LVal v;
            const std::string iname = static_cast<const IdentifierExpr*>(e)->name;
            if (!lookup_var(iname, v)) {
                // 若是某用户模块对象的裸引用（存在 <iname>.<符号> 成员），不算未声明：
                // 编译端模块对象仅是占位（成员经 foo.x / foo.f 专有路径访问）
                std::string pref = iname + ".";
                bool isModule = false;
                for (const auto& frame : scopes_)
                    for (const auto& kv : frame)
                        if (kv.first.size() > iname.size() &&
                            kv.first.compare(0, pref.size(), pref) == 0) { isModule = true; break; }
                if (!isModule)
                    for (const llvm::Function& F : mod_->getFunctionList()) {
                        std::string s = F.getName().str();
                        if (s.size() > iname.size() && s.compare(0, pref.size(), pref) == 0) { isModule = true; break; }
                    }
                if (isModule) return {VType::Int, B->getInt64(0)};   // 模块对象占位（无实义值）
                // 真·未声明标识符：明确报错并给拼写建议
                throw std::runtime_error("'" + iname + "' was not declared in this scope" +
                                         vortex::suggest::did_you_mean(iname, ident_candidates()));
            }
            // 有 alloca 槽则每次重新 load，保证读到最新值
            if (v.slot) return {v.type, B->CreateLoad(llvm_type(v.type), v.slot), nullptr, VType::Void, v.obj_class};
            return v;
        }
        case ExprKind::Cast: {
            auto* ce = static_cast<const CastExpr*>(e);
            std::string t = ce->target_type;
            LVal sv = gen_expr(ce->value.get());
            // 精确十进制转换（与解释器口径一致）
            if (sv.type == VType::ExtFloat) {
                if (t == "extfloat") return sv;
                if (t == "int" || t == "long" || t == "uint" || t == "ulong" ||
                    t == "char" || t == "unichar")
                    return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(
                        decl_vor("vor_ext_to_i64", VType::Int, {VType::ExtFloat})), {sv.val}, "ext.i64")};
                if (t == "float" || t == "double")
                    return {VType::Float, B->CreateCall(llvm::cast<llvm::Function>(
                        decl_vor("vor_ext_to_f64", VType::Float, {VType::ExtFloat})), {sv.val}, "ext.f64")};
                if (t == "str" || t == "unistr") return {VType::Str, sv.val};
                if (t == "bool") {
                    llvm::Value* f = B->CreateCall(llvm::cast<llvm::Function>(
                        decl_vor("vor_ext_to_f64", VType::Float, {VType::ExtFloat})), {sv.val}, "ext.b");
                    return {VType::Bool, B->CreateFCmpUNE(f, llvm::ConstantFP::get(B->getDoubleTy(), 0.0), "ext.nz")};
                }
            }
            if (t == "extfloat") {
                // 从 int/float/bool/str 转精确十进制
                if (sv.type == VType::Str) {
                    llvm::Value* fn = decl_vor("vor_ext_from_vstr", VType::ExtFloat, {VType::Str});
                    return {VType::ExtFloat, B->CreateCall(llvm::cast<llvm::Function>(fn), {sv.val})};
                }
                if (sv.type == VType::Float) {
                    llvm::Value* fn = decl_vor("vor_ext_from_f64", VType::ExtFloat, {VType::Float});
                    return {VType::ExtFloat, B->CreateCall(llvm::cast<llvm::Function>(fn), {sv.val})};
                }
                llvm::Value* iv = (sv.type == VType::Bool)
                                      ? B->CreateZExt(sv.val, B->getInt64Ty(), "e.b2i") : to_i64(sv);
                llvm::Value* fn = decl_vor("vor_ext_from_i64", VType::ExtFloat, {VType::Int});
                return {VType::ExtFloat, B->CreateCall(llvm::cast<llvm::Function>(fn), {iv})};
            }
            if (t == "int" || t == "long") {
                if (sv.type == VType::Int) return sv;
                if (sv.type == VType::Bool) return {VType::Int, B->CreateZExt(sv.val, B->getInt64Ty())};
                if (sv.type == VType::Float) return {VType::Int, B->CreateFPToSI(sv.val, B->getInt64Ty(), "i.cast")};
                if (sv.type == VType::Str) {
                    llvm::Value* fn = decl_vor("vor_cast_i64", VType::Int, {VType::Str});
                    return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {sv.val})};
                }
                return {VType::Int, to_i64(sv)};
            }
            if (t == "float" || t == "double") {
                if (sv.type == VType::Float) return sv;
                if (sv.type == VType::Str) {
                    llvm::Value* fn = decl_vor("vor_cast_f64", VType::Float, {VType::Str});
                    return {VType::Float, B->CreateCall(llvm::cast<llvm::Function>(fn), {sv.val})};
                }
                if (sv.type == VType::Bool) return {VType::Float, B->CreateUIToFP(sv.val, B->getDoubleTy())};
                return {VType::Float, B->CreateSIToFP(to_i64(sv), B->getDoubleTy())};
            }
            if (t == "bool") return {VType::Bool, B->CreateICmpNE(to_i64(sv), B->getInt64(0))};
            if (t == "str") {
                if (sv.type == VType::Str) return sv;
                llvm::Value* fn = nullptr;
                if (sv.type == VType::Int)
                    fn = decl_vor("vor_i64_to_str", VType::Str, {VType::Int});
                else if (sv.type == VType::Float)
                    fn = decl_vor("vor_double_to_str", VType::Str, {VType::Float});
                else
                    fn = decl_vor("vor_bool_to_str", VType::Str, {VType::Int});
                return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn),
                    {sv.type == VType::Float ? sv.val : to_i64(sv)})};
            }
            return {VType::Int, to_i64(sv)};
        }
        case ExprKind::BinaryOp:
            return gen_binop(static_cast<const BinaryOpExpr*>(e));
        case ExprKind::UnaryOp: {
            auto* u = static_cast<const UnaryOpExpr*>(e);
            if (u->op == "@") {
                // @x：取变量槽地址，返回 Ref
                if (u->operand->kind != ExprKind::Identifier) break;
                std::string anm = static_cast<const IdentifierExpr*>(u->operand.get())->name;
                LVal t;
                if (lookup_var(anm, t) && t.slot) {
                    LVal ref; ref.type = VType::Ref; ref.val = t.slot; ref.ref_pointee = t.type;
                    return ref;
                }
                break;
            }
            if (u->op == "~") {
                // ~：对 Ref 解引用读取；否则按位取反
                LVal v = gen_expr(u->operand.get());
                if (v.type == VType::Ref && v.ref_pointee != VType::Void)
                    return {v.ref_pointee, B->CreateLoad(llvm_type(v.ref_pointee), v.val)};
                if (v.type == VType::Int)
                    return {VType::Int, B->CreateXor(v.val, B->getInt64(-1))};
                break;
            }
            LVal v = gen_expr(u->operand.get());
            // 用户自定义一元运算符优先（任意允许的运算符可作前缀一元，如 `>x`）
            if (const OperatorDeclStmt* od =
                    find_operator_decl(u->op, lval_type_name(v), "", true))
                return call_operator_fn(od, v, LVal{VType::Void, nullptr});
            if (v.type == VType::ExtFloat) {
                if (u->op == "+") return v;
                if (u->op == "-")
                    return {VType::ExtFloat, B->CreateCall(llvm::cast<llvm::Function>(
                        decl_vor("vor_ext_neg", VType::ExtFloat, {VType::ExtFloat})), {v.val}, "ext.neg")};
                if (u->op == "!") {
                    llvm::Value* f = B->CreateCall(llvm::cast<llvm::Function>(
                        decl_vor("vor_ext_to_f64", VType::Float, {VType::ExtFloat})), {v.val}, "ext.t");
                    llvm::Value* nz = B->CreateFCmpUNE(f, llvm::ConstantFP::get(B->getDoubleTy(), 0.0), "ext.nz");
                    return {VType::Bool, B->CreateNot(nz)};
                }
            }
            if (u->op == "!") return {VType::Bool, B->CreateNot(v.val)};
            if (u->op == "-") {
                if (v.type == VType::Int)  return {VType::Int,   B->CreateNeg(v.val)};
                if (v.type == VType::Float)return {VType::Float, B->CreateFNeg(v.val)};
            }
            break;
        }
        case ExprKind::AssignOp: {
            auto* a = static_cast<const AssignOpExpr*>(e);
            // 实例字段赋值 p.x = v
            if (a->target->kind == ExprKind::MemberAccess && a->op == "=") {
                auto* m = static_cast<const MemberAccessExpr*>(a->target.get());
                LVal obj = gen_expr(m->object.get());
                if (obj.type == VType::Object) {
                    LVal v = gen_expr(a->value.get());
                    llvm::Value* iv = (v.type == VType::Float)
                                          ? B->CreateFPToSI(v.val, B->getInt64Ty(), "f2i") : v.val;
                    llvm::Function* setFn = llvm::cast<llvm::Function>(declare_runtime(
                        "vor_obj_set", B->getVoidTy(), {B->getPtrTy(), B->getPtrTy(), B->getInt64Ty()}));
                    B->CreateCall(setFn, {obj.val, cstr(m->member), iv});
                    return v;
                }
            }
            // dict 下标赋值 d[k] = v
            if (a->target->kind == ExprKind::Subscript && a->op == "=") {
                auto* sub = static_cast<const SubscriptExpr*>(a->target.get());
                LVal obj = gen_expr(sub->object.get());
                if (obj.type == VType::Dict) {
                    LVal idx = gen_expr(sub->index.get());
                    LVal v = gen_expr(a->value.get());
                    const bool ks = (idx.type == VType::Str);
                    if (v.type == VType::Str) {
                        llvm::Value* fn = decl_vor(ks ? "vor_dict_set_str_vstr" : "vor_dict_set_int_vstr",
                            VType::Void, ks ? std::vector<VType>{VType::Dict, VType::Str, VType::Str}
                                            : std::vector<VType>{VType::Dict, VType::Int, VType::Str});
                        B->CreateCall(llvm::cast<llvm::Function>(fn), {obj.val, idx.val, v.val});
                    } else {
                        llvm::Value* fn = decl_vor(ks ? "vor_dict_set_str" : "vor_dict_set_int",
                            VType::Void, ks ? std::vector<VType>{VType::Dict, VType::Str, VType::Int}
                                            : std::vector<VType>{VType::Dict, VType::Int, VType::Int});
                        B->CreateCall(llvm::cast<llvm::Function>(fn), {obj.val, idx.val, to_i64(v)});
                    }
                    return {VType::Void, nullptr};
                }
                break;
            }
            llvm::Value* storePtr = nullptr;
            VType ptrType = VType::Void;
            if (a->target->kind == ExprKind::Identifier) {
                LVal target;
                std::string nm = static_cast<const IdentifierExpr*>(a->target.get())->name;
                if (!lookup_var(nm, target) || !target.slot) break;
                storePtr = target.slot; ptrType = target.type;
            } else if (a->target->kind == ExprKind::UnaryOp) {
                auto* tu = static_cast<const UnaryOpExpr*>(a->target.get());
                if (tu->op != "~") break;
                LVal r = gen_expr(tu->operand.get());       // ~@x = v / ~r = v
                if (r.type != VType::Ref || r.ref_pointee == VType::Void) break;
                storePtr = r.val; ptrType = r.ref_pointee;
            } else break;

            LVal rvrhs = gen_expr(a->value.get());
            llvm::Value* rhs = rvrhs.val;                       // RHS（内联对旧值的引用在本 store 前求值）
            bool isFloat = (ptrType == VType::Float);
            if (isFloat && rvrhs.type == VType::Int)
                rhs = B->CreateSIToFP(rvrhs.val, B->getDoubleTy(), "r.f");
            llvm::Value* toStore = rhs;

            if (a->op == "=") {
                toStore = rhs;
            } else {
                llvm::Value* old = B->CreateLoad(llvm_type(ptrType), storePtr);
                if (a->op == "+=") toStore = isFloat ? B->CreateFAdd(old, rhs) : B->CreateAdd(old, rhs);
                else if (a->op == "-=") toStore = isFloat ? B->CreateFSub(old, rhs) : B->CreateSub(old, rhs);
                else if (a->op == "*=") toStore = isFloat ? B->CreateFMul(old, rhs) : B->CreateMul(old, rhs);
                else if (a->op == "/=") toStore = isFloat ? B->CreateFDiv(old, rhs) : B->CreateSDiv(old, rhs);
                else toStore = rhs;
            }
            B->CreateStore(toStore, storePtr);
            return {ptrType, toStore};
        }
        case ExprKind::Call:
            return gen_call(static_cast<const CallExpr*>(e));
        case ExprKind::ListInit: {
            // [e1,e2,...] -> VList（元素按 i64 槽存储）
            llvm::Value* ln = decl_vor("vor_list_new", VType::List, {});
            llvm::Value* lst = B->CreateCall(llvm::cast<llvm::Function>(ln), {});
            auto* li = static_cast<const ListInitExpr*>(e);
            for (auto& el : li->elements) {
                LVal ev = gen_expr(el.get());
                llvm::Value* box = to_i64(ev);
                B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_list_push", VType::Void, {VType::List, VType::Int})),
                              {lst, box});
            }
            return {VType::List, lst};
        }
        case ExprKind::DictInit: {
            // {k:v,...} -> VDict（key/value 各支持 int 或 str）
            llvm::Value* dn = decl_vor("vor_dict_new", VType::Dict, {});
            llvm::Value* d = B->CreateCall(llvm::cast<llvm::Function>(dn), {});
            auto* di = static_cast<const DictInitExpr*>(e);
            for (auto& kv : di->pairs) {
                LVal k = gen_expr(kv.key.get());
                LVal v = gen_expr(kv.value.get());
                const bool ks = (k.type == VType::Str);
                if (v.type == VType::Str) {
                    const char* f = ks ? "vor_dict_set_str_vstr" : "vor_dict_set_int_vstr";
                    llvm::Value* fn = decl_vor(f, VType::Void, ks
                        ? std::vector<VType>{VType::Dict, VType::Str, VType::Str}
                        : std::vector<VType>{VType::Dict, VType::Int, VType::Str});
                    B->CreateCall(llvm::cast<llvm::Function>(fn), {d, k.val, v.val});
                } else {
                    const char* f = ks ? "vor_dict_set_str" : "vor_dict_set_int";
                    llvm::Value* fn = decl_vor(f, VType::Void, ks
                        ? std::vector<VType>{VType::Dict, VType::Str, VType::Int}
                        : std::vector<VType>{VType::Dict, VType::Int, VType::Int});
                    B->CreateCall(llvm::cast<llvm::Function>(fn), {d, k.val, to_i64(v)});
                }
            }
            return {VType::Dict, d};
        }
        case ExprKind::ListComp: {
            auto* lc = static_cast<const ListCompExpr*>(e);
            // 仅支持 int 元素容器（list/dict-int-key），与编译器 for-in 能力一致
            LVal cont = gen_expr(lc->container.get());
            const bool isDict = (cont.type == VType::Dict);
            const bool isStr = (cont.type == VType::StrList);
            if (cont.type != VType::List && !isDict && cont.type != VType::Set
                && cont.type != VType::Tuple && !isStr)
                throw std::runtime_error("comprehension: unsupported container type (VType=" + std::to_string((int)cont.type) + ")");
            llvm::Function* F = cur_bb()->getParent();
            llvm::Value* res = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_list_new", VType::List, {})), {});
            llvm::BasicBlock* condBB = llvm::BasicBlock::Create(B->getContext(), "lc.cond", F);
            llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(B->getContext(), "lc.body", F);
            llvm::BasicBlock* updBB  = llvm::BasicBlock::Create(B->getContext(), "lc.upd", F);
            llvm::BasicBlock* endBB  = llvm::BasicBlock::Create(B->getContext(), "lc.end", F);
            llvm::AllocaInst* idxSlot = make_alloca(VType::Int, F, "lc.idx");
            B->CreateStore(B->getInt64(0), idxSlot);
            B->CreateBr(condBB);
            // upd
            B->SetInsertPoint(updBB);
            llvm::Value* i1 = B->CreateLoad(B->getInt64Ty(), idxSlot);
            B->CreateStore(B->CreateAdd(i1, B->getInt64(1)), idxSlot);
            B->CreateBr(condBB);
            // cond
            B->SetInsertPoint(condBB);
            llvm::Value* len = isDict
                ? B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_dict_int_key_count", VType::Int, {VType::Dict})), {cont.val})
                : (isStr ? B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_list_len", VType::Int, {VType::List})), {cont.val})
                         : B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_list_len", VType::Int, {VType::List})), {cont.val}));
            llvm::Value* ic = B->CreateLoad(B->getInt64Ty(), idxSlot);
            B->CreateCondBr(B->CreateICmpSLT(ic, len), bodyBB, endBB);
            // body: bind var
            B->SetInsertPoint(bodyBB);
            const VType elType = isStr ? VType::Str : VType::Int;
            llvm::AllocaInst* varSlot = make_alloca(elType, F, "lc.var");
            llvm::Value* idxv = B->CreateLoad(B->getInt64Ty(), idxSlot);
            llvm::Value* elem;
            if (isDict)
                elem = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_dict_int_key_at", VType::Int, {VType::Dict, VType::Int})), {cont.val, idxv});
            else if (isStr)
                elem = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_list_get_str", VType::Str, {VType::StrList, VType::Int})), {cont.val, idxv});
            else
                elem = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_list_get", VType::Int, {VType::List, VType::Int})), {cont.val, idxv});
            B->CreateStore(elem, varSlot);
            scopes_.emplace_back();
            scopes_.back()[lc->var] = LVal{elType, B->CreateLoad(llvm_type(elType), varSlot), varSlot};
            // cond 过滤
            if (lc->cond) {
                llvm::BasicBlock* filtBB = llvm::BasicBlock::Create(B->getContext(), "lc.filt", F);
                llvm::BasicBlock* pushBB = llvm::BasicBlock::Create(B->getContext(), "lc.push", F);
                LVal cf = gen_expr(lc->cond.get());
                B->CreateCondBr(cf.val, pushBB, updBB);
                B->SetInsertPoint(pushBB);
                LVal ev = gen_expr(lc->element.get());
                B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_list_push", VType::Void, {VType::List, VType::Int})), {res, to_i64(ev)});
                scopes_.pop_back();
                B->CreateBr(updBB);
                B->SetInsertPoint(filtBB);
                B->CreateBr(updBB);
            } else {
                LVal ev = gen_expr(lc->element.get());
                B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_list_push", VType::Void, {VType::List, VType::Int})), {res, to_i64(ev)});
                scopes_.pop_back();
                B->CreateBr(updBB);
            }
            B->SetInsertPoint(endBB);
            return {VType::List, res};
        }
        case ExprKind::DictComp: {
            auto* dc = static_cast<const DictCompExpr*>(e);
            LVal cont = gen_expr(dc->container.get());
            const bool isDict = (cont.type == VType::Dict);
            if (cont.type != VType::List && !isDict && cont.type != VType::Set
                && cont.type != VType::Tuple)
                throw std::runtime_error("dict comprehension: unsupported container type");
            llvm::Function* F = cur_bb()->getParent();
            llvm::Value* res = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_dict_new", VType::Dict, {})), {});
            llvm::BasicBlock* condBB = llvm::BasicBlock::Create(B->getContext(), "dcc.cond", F);
            llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(B->getContext(), "dcc.body", F);
            llvm::BasicBlock* updBB  = llvm::BasicBlock::Create(B->getContext(), "dcc.upd", F);
            llvm::BasicBlock* endBB  = llvm::BasicBlock::Create(B->getContext(), "dcc.end", F);
            llvm::AllocaInst* idxSlot = make_alloca(VType::Int, F, "dcc.idx");
            B->CreateStore(B->getInt64(0), idxSlot);
            B->CreateBr(condBB);
            B->SetInsertPoint(updBB);
            llvm::Value* i1 = B->CreateLoad(B->getInt64Ty(), idxSlot);
            B->CreateStore(B->CreateAdd(i1, B->getInt64(1)), idxSlot);
            B->CreateBr(condBB);
            B->SetInsertPoint(condBB);
            llvm::Value* len = isDict
                ? B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_dict_int_key_count", VType::Int, {VType::Dict})), {cont.val})
                : B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_list_len", VType::Int, {VType::List})), {cont.val});
            llvm::Value* ic = B->CreateLoad(B->getInt64Ty(), idxSlot);
            B->CreateCondBr(B->CreateICmpSLT(ic, len), bodyBB, endBB);
            B->SetInsertPoint(bodyBB);
            llvm::AllocaInst* varSlot = make_alloca(VType::Int, F, "dcc.var");
            llvm::Value* idxv = B->CreateLoad(B->getInt64Ty(), idxSlot);
            llvm::Value* elem = isDict
                ? B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_dict_int_key_at", VType::Int, {VType::Dict, VType::Int})), {cont.val, idxv})
                : B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_list_get", VType::Int, {VType::List, VType::Int})), {cont.val, idxv});
            B->CreateStore(elem, varSlot);
            scopes_.emplace_back();
            scopes_.back()[dc->var] = LVal{VType::Int, B->CreateLoad(B->getInt64Ty(), varSlot), varSlot};
            if (dc->cond) {
                llvm::BasicBlock* filtBB = llvm::BasicBlock::Create(B->getContext(), "dcc.filt", F);
                llvm::BasicBlock* pushBB = llvm::BasicBlock::Create(B->getContext(), "dcc.push", F);
                LVal cf = gen_expr(dc->cond.get());
                B->CreateCondBr(cf.val, pushBB, updBB);
                B->SetInsertPoint(pushBB);
                LVal k = gen_expr(dc->key.get());
                LVal v = gen_expr(dc->value.get());
                const bool ks = (k.type == VType::Str);
                if (v.type == VType::Str)
                    B->CreateCall(llvm::cast<llvm::Function>(decl_vor(ks ? "vor_dict_set_str_vstr" : "vor_dict_set_int_vstr", VType::Void, ks ? std::vector<VType>{VType::Dict, VType::Str, VType::Str} : std::vector<VType>{VType::Dict, VType::Int, VType::Str})), {res, k.val, v.val});
                else
                    B->CreateCall(llvm::cast<llvm::Function>(decl_vor(ks ? "vor_dict_set_str" : "vor_dict_set_int", VType::Void, ks ? std::vector<VType>{VType::Dict, VType::Str, VType::Int} : std::vector<VType>{VType::Dict, VType::Int, VType::Int})), {res, k.val, to_i64(v)});
                scopes_.pop_back();
                B->CreateBr(updBB);
                B->SetInsertPoint(filtBB);
                B->CreateBr(updBB);
            } else {
                LVal k = gen_expr(dc->key.get());
                LVal v = gen_expr(dc->value.get());
                const bool ks = (k.type == VType::Str);
                if (v.type == VType::Str)
                    B->CreateCall(llvm::cast<llvm::Function>(decl_vor(ks ? "vor_dict_set_str_vstr" : "vor_dict_set_int_vstr", VType::Void, ks ? std::vector<VType>{VType::Dict, VType::Str, VType::Str} : std::vector<VType>{VType::Dict, VType::Int, VType::Str})), {res, k.val, v.val});
                else
                    B->CreateCall(llvm::cast<llvm::Function>(decl_vor(ks ? "vor_dict_set_str" : "vor_dict_set_int", VType::Void, ks ? std::vector<VType>{VType::Dict, VType::Str, VType::Int} : std::vector<VType>{VType::Dict, VType::Int, VType::Int})), {res, k.val, to_i64(v)});
                scopes_.pop_back();
                B->CreateBr(updBB);
            }
            B->SetInsertPoint(endBB);
            return {VType::Dict, res};
        }
        case ExprKind::Subscript: {
            auto* sub = static_cast<const SubscriptExpr*>(e);
            LVal obj = gen_expr(sub->object.get());
            LVal idx = gen_expr(sub->index.get());
            if (obj.type == VType::List) {
                llvm::Value* g = decl_vor("vor_list_get", VType::Int, {VType::List, VType::Int});
                return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(g),
                                                  {obj.val, B->CreateSExt(idx.val, B->getInt64Ty())})};
            }
            if (obj.type == VType::StrList) {
                llvm::Value* g = decl_vor("vor_list_get_str", VType::Str, {VType::StrList, VType::Int});
                return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(g),
                                                  {obj.val, B->CreateSExt(idx.val, B->getInt64Ty())})};
            }
            if (obj.type == VType::StrListList) {
                // 嵌套字符串列表：取第 idx 行（内层字符串列表）
                llvm::Value* g = decl_vor("vor_list_get_strlist", VType::StrList, {VType::StrListList, VType::Int});
                return {VType::StrList, B->CreateCall(llvm::cast<llvm::Function>(g),
                                                     {obj.val, B->CreateSExt(idx.val, B->getInt64Ty())})};
            }
            if (obj.type == VType::Tuple) {
                llvm::Value* g = decl_vor("vor_tuple_at", VType::Int, {VType::Tuple, VType::Int});
                return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(g), {obj.val, idx.val})};
            }
            if (obj.type == VType::Dict) {
                // d[int] / d[str] -> int 值
                if (idx.type == VType::Str) {
                    llvm::Value* g = decl_vor("vor_dict_get_str", VType::Int, {VType::Dict, VType::Str});
                    return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(g), {obj.val, idx.val})};
                }
                llvm::Value* g = decl_vor("vor_dict_get_int", VType::Int, {VType::Dict, VType::Int});
                return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(g), {obj.val, idx.val})};
            }
            break;
        }
        case ExprKind::MemberAccess: {
            // math 模块常量（pi/e/tau/phi/sqrt2/ln2/inf/nan）
            auto* mc = static_cast<const MemberAccessExpr*>(e);
            if (mc->object->kind == ExprKind::Identifier) {
                const std::string objName =
                    static_cast<const IdentifierExpr*>(mc->object.get())->name;
                if (objName == "math") {
                    const std::string& cn = mc->member;
                    double cv = 0; bool found = true;
                    if      (cn == "pi")     cv = 3.141592653589793238462643383279502884197169399375105820974944;
                    else if (cn == "e")      cv = 2.718281828459045235360287471352662497757247093699959574966967;
                    else if (cn == "tau")    cv = 6.283185307179586476925286766559005768394338798750211641949888;
                    else if (cn == "phi")    cv = 1.618033988749894848204586834365638117720309179805762862135448;
                    else if (cn == "sqrt2")  cv = 1.414213562373095048801688724209698078569671875376948073176680;
                    else if (cn == "ln2")    cv = 0.693147180559945309417232121458176568075500134360255254120680;
                    else if (cn == "inf")    cv = std::numeric_limits<double>::infinity();
                    else if (cn == "nan")    cv = std::numeric_limits<double>::quiet_NaN();
                    else found = false;
                    if (found)
                        return {VType::Float, llvm::ConstantFP::get(B->getDoubleTy(), cv)};
                }
                // os 模块字符串常量（SEP / LINE_END）
                if (objName == "os") {
                    const std::string& cn = mc->member;
                    const char* cstr = nullptr;
                    if (cn == "SEP") cstr = "/";
                    else if (cn == "LINE_END") cstr = "\n";
                    if (cstr) {
                        llvm::Value* fn = decl_vor("vor_str_from_cstr", VType::Str, {VType::Str});
                        llvm::Value* g = B->CreateGlobalString(cstr, "osc");
                        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {g})};
                    }
                }
            }
            // 用户模块全局/常量：读模块顶层全局 "g_<obj>.<member>"
            if (mc->object->kind == ExprKind::Identifier) {
                const std::string objN = static_cast<const IdentifierExpr*>(mc->object.get())->name;
                if (llvm::GlobalVariable* gv = mod_->getNamedGlobal("g_" + objN + "." + mc->member)) {
                    llvm::Type* gt = gv->getValueType();
                    if (gt->isDoubleTy()) return {VType::Float, B->CreateLoad(gt, gv)};
                    if (gt->isPointerTy()) return {VType::Str, B->CreateLoad(gt, gv)};
                    return {VType::Int, B->CreateLoad(B->getInt64Ty(), gv)};
                }
            }
            // pair.first / pair.second（作为值表达式，非调用）
            auto* m = static_cast<const MemberAccessExpr*>(e);
            LVal obj = gen_expr(m->object.get());
            // class 实例：字段按名读取
            if (obj.type == VType::Object) return gen_obj_field_read(obj, m->member);
            if (obj.type == VType::Pair && m->member == "first") {
                llvm::Value* fn = decl_vor("vor_pair_first", VType::Int, {VType::Pair});
                return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {obj.val})};
            }
            if (obj.type == VType::Pair && m->member == "second") {
                llvm::Value* fn = decl_vor("vor_pair_second", VType::Int, {VType::Pair});
                return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {obj.val})};
            }
            // xxx.length / xxx.len（list / 字符串列表 / 嵌套列表 / 字符串）
            if ((obj.type == VType::List || obj.type == VType::StrList
                 || obj.type == VType::StrListList)
                && (m->member == "len" || m->member == "length")) {
                llvm::Value* fn = decl_vor("vor_list_len", VType::Int, {VType::List});
                return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {obj.val})};
            }
            if (obj.type == VType::Str && (m->member == "len" || m->member == "length")) {
                llvm::Value* fn = decl_vor("vor_str_len", VType::Int, {VType::Str});
                return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {obj.val})};
            }
            break;
        }
        case ExprKind::Lambda:
            return gen_lambda(static_cast<const LambdaExpr*>(e));
        default:
            break;
    }
    return {VType::Int, B->getInt64(0)};
}

// ---------- 二进制运算 ----------
LVal Gen::gen_binop(const BinaryOpExpr* b) {
    LVal l = gen_expr(b->left.get());
    LVal r = gen_expr(b->right.get());
    const std::string& op = b->op;

    // 精确十进制：任一操作数为 extfloat（+ 的文本拼接场景除外）时走精确十进制运算
    // 必须在浮点提升之前，避免把 extfloat 指针当作整数转成 double
    if ((l.type == VType::ExtFloat || r.type == VType::ExtFloat) &&
        !(op == "+" && (l.type == VType::Str || r.type == VType::Str)))
        return gen_ext_binop(op, l, r);

    bool isFloat = (l.type == VType::Float || r.type == VType::Float);
    // 浮点提升：任一为 double，则整型操作数转 double，避免 fadd 操作数类型不一致
    llvm::Value* lf = l.val;
    llvm::Value* rf = r.val;
    if (isFloat) {
        if (l.type != VType::Float) lf = B->CreateSIToFP(l.val, B->getDoubleTy(), "l.f");
        if (r.type != VType::Float) rf = B->CreateSIToFP(r.val, B->getDoubleTy(), "r.f");
    }

    // 逻辑短路：&& || 用基本块
    if (op == "&&") {
        llvm::Function* F = cur_bb()->getParent();
        llvm::BasicBlock* rhs = llvm::BasicBlock::Create(B->getContext(), "and.rhs", F);
        llvm::BasicBlock* merge = llvm::BasicBlock::Create(B->getContext(), "and.merge", F);
        llvm::BasicBlock* from = B->GetInsertBlock();   // 分支前的块
        llvm::Value* lv = B->CreateTrunc(l.val, B->getInt1Ty(), "and.l");
        B->CreateCondBr(lv, rhs, merge);                 // lhs 为假直接 phi=false(from)
        B->SetInsertPoint(rhs);
        llvm::Value* rv = B->CreateTrunc(r.val, B->getInt1Ty(), "and.r");
        B->CreateBr(merge);
        B->SetInsertPoint(merge);
        llvm::PHINode* phi = B->CreatePHI(B->getInt1Ty(), 2, "and");
        phi->addIncoming(B->getFalse(), from);
        phi->addIncoming(rv, rhs);
        return {VType::Bool, phi};
    }
    if (op == "||") {
        llvm::Function* F = cur_bb()->getParent();
        llvm::BasicBlock* rhs = llvm::BasicBlock::Create(B->getContext(), "or.rhs", F);
        llvm::BasicBlock* merge = llvm::BasicBlock::Create(B->getContext(), "or.merge", F);
        llvm::BasicBlock* from = B->GetInsertBlock();
        llvm::Value* lv = B->CreateTrunc(l.val, B->getInt1Ty(), "or.l");
        B->CreateCondBr(lv, merge, rhs);                 // lhs 为真直接 phi=true(from)
        B->SetInsertPoint(rhs);
        llvm::Value* rv = B->CreateTrunc(r.val, B->getInt1Ty(), "or.r");
        B->CreateBr(merge);
        B->SetInsertPoint(merge);
        llvm::PHINode* phi = B->CreatePHI(B->getInt1Ty(), 2, "or");
        phi->addIncoming(B->getTrue(), from);
        phi->addIncoming(rv, rhs);
        return {VType::Bool, phi};
    }

    // 用户自定义二元运算符优先（精确匹配，再父链回退；无匹配则走内建语义）
    if (const OperatorDeclStmt* od =
            find_operator_decl(b->op, lval_type_name(l), lval_type_name(r), false))
        return call_operator_fn(od, l, r);

    // 算术
    llvm::Value* res = nullptr;
    if (op == "+") {
        // Text concatenation: if either side is str/extfloat, stringify the other side too
        // (e.g. "x" + 5 -> "x5"), matching the interpreter.
        const bool ls = (l.type == VType::Str || l.type == VType::ExtFloat);
        const bool rs = (r.type == VType::Str || r.type == VType::ExtFloat);
        if (ls || rs) {
            llvm::Value* lv = ls ? l.val : to_vstr(l);
            llvm::Value* rv = rs ? r.val : to_vstr(r);
            llvm::Value* fn = decl_vor("vor_str_concat", VType::Str, {VType::Str, VType::Str});
            return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {lv, rv})};
        }
        res = isFloat ? B->CreateFAdd(lf, rf, "add") : B->CreateAdd(l.val, r.val, "add");
        return {isFloat ? VType::Float : VType::Int, res};
    }
    if (op == "-") res = isFloat ? B->CreateFSub(lf, rf, "sub") : B->CreateSub(l.val, r.val, "sub");
    if (op == "*") res = isFloat ? B->CreateFMul(lf, rf, "mul") : B->CreateMul(l.val, r.val, "mul");
    if (op == "/") { // vortex 的 "/" 一律浮点除法
            res = B->CreateFDiv(isFloat ? lf : B->CreateSIToFP(l.val, B->getDoubleTy()),
                                isFloat ? rf : B->CreateSIToFP(r.val, B->getDoubleTy()), "fdiv");
            return {VType::Float, res};
        }
    if (op == "//") { // 整数则整除；浮点则 floor(l/r)
        if (isFloat) {
            llvm::Value* div = B->CreateFDiv(lf, rf, "fdiv");
            llvm::Value* fn = decl_vor("vor_floor", VType::Int, {VType::Float});
            llvm::Value* fl = B->CreateCall(llvm::cast<llvm::Function>(fn), {div});
            res = B->CreateSIToFP(fl, B->getDoubleTy());
            return {VType::Float, res};
        }
        // Integer floor division (Python semantics), matching the interpreter.
        llvm::Value* fn = decl_vor("vor_floordiv_i64", VType::Int, {VType::Int, VType::Int});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {l.val, r.val}, "idiv")};
    }
    if (op == "%") {
        if (isFloat) { res = B->CreateFRem(lf, rf, "frem"); }
        else {
            // Integer modulo with the sign of the divisor (Python semantics).
            llvm::Value* fn = decl_vor("vor_mod_i64", VType::Int, {VType::Int, VType::Int});
            return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {l.val, r.val}, "rem")};
        }
    }
    if (op == "**") {
        llvm::Value* fn = decl_vor("vor_pow", VType::Float, {VType::Float, VType::Float});
        llvm::Value* f1 = isFloat ? lf : B->CreateSIToFP(l.val, B->getDoubleTy());
        llvm::Value* f2 = isFloat ? rf : B->CreateSIToFP(r.val, B->getDoubleTy());
        res = B->CreateCall(llvm::cast<llvm::Function>(fn), {f1, f2});
        return {VType::Float, res};
    }
    if (res) {
        if (res->getType()->isDoubleTy()) return {VType::Float, res};
        return {isFloat ? VType::Float : VType::Int, res};
    }

    // 位运算
    if (op == "&") { res = B->CreateAnd(l.val, r.val, "and"); }
    if (op == "|") { res = B->CreateOr(l.val, r.val, "or"); }
    if (op == "^") { res = B->CreateXor(l.val, r.val, "xor"); }
    if (res) return {VType::Int, res};

    // 比较 -> bool
    const bool strOp = (l.type == VType::Str || r.type == VType::Str);
    if (strOp && (op == "==" || op == "!=" || op == "<" || op == ">" ||
                  op == "<=" || op == ">=")) {
        // 字符串比较：对内容做 strcmp（避免指针地址比较恒 false）
        // 非 str 一侧按解释器语义转为字符串。
        auto to_strv = [&](LVal v, llvm::Value* val) -> llvm::Value* {
            if (v.type == VType::Str) return val;
            if (v.type == VType::Int) {
                llvm::Value* f = decl_vor("vor_i64_to_str", VType::Str, {VType::Int});
                return B->CreateCall(llvm::cast<llvm::Function>(f), {val});
            }
            if (v.type == VType::Float) {
                llvm::Value* f = decl_vor("vor_double_to_str", VType::Str, {VType::Float});
                return B->CreateCall(llvm::cast<llvm::Function>(f), {val});
            }
            if (v.type == VType::Bool) {
                llvm::Value* t = B->CreateGlobalString("true", "bstr");
                llvm::Value* f = B->CreateGlobalString("false", "bstr");
                llvm::Value* sel = B->CreateSelect(val, t, f);
                llvm::Value* fn = decl_vor("vor_str_from_cstr", VType::Str, {VType::Str});
                return B->CreateCall(llvm::cast<llvm::Function>(fn), {sel});
            }
            llvm::Value* fn = decl_vor("vor_str_from_cstr", VType::Str, {VType::Str});
            return B->CreateCall(llvm::cast<llvm::Function>(fn), {B->CreateGlobalString("", "estr")});
        };
        llvm::Value* ls = to_strv(l, l.val);
        llvm::Value* rs = to_strv(r, r.val);
        llvm::Value* fn = decl_vor("vor_str_cmp", VType::Int, {VType::Str, VType::Str});
        llvm::Value* c = B->CreateCall(llvm::cast<llvm::Function>(fn), {ls, rs});
        llvm::Value* zero = B->getInt64(0);
        llvm::Value* cmp = nullptr;
        if (op == "==") cmp = B->CreateICmpEQ(c, zero, "cmpeq");
        else if (op == "!=") cmp = B->CreateICmpNE(c, zero, "cmpne");
        else if (op == "<")  cmp = B->CreateICmpSLT(c, zero, "cmplt");
        else if (op == ">")  cmp = B->CreateICmpSGT(c, zero, "cmpgt");
        else if (op == "<=") cmp = B->CreateICmpSLE(c, zero, "cmple");
        else if (op == ">=") cmp = B->CreateICmpSGE(c, zero, "cmpge");
        return {VType::Bool, cmp};
    }

    llvm::CmpInst::Predicate pred;
    if      (op == "==") pred = isFloat ? llvm::CmpInst::FCMP_OEQ : llvm::CmpInst::ICMP_EQ;
    else if (op == "!=") pred = isFloat ? llvm::CmpInst::FCMP_ONE : llvm::CmpInst::ICMP_NE;
    else if (op == "<")  pred = isFloat ? llvm::CmpInst::FCMP_OLT : llvm::CmpInst::ICMP_SLT;
    else if (op == ">")  pred = isFloat ? llvm::CmpInst::FCMP_OGT : llvm::CmpInst::ICMP_SGT;
    else if (op == "<=") pred = isFloat ? llvm::CmpInst::FCMP_OLE : llvm::CmpInst::ICMP_SLE;
    else if (op == ">=") pred = isFloat ? llvm::CmpInst::FCMP_OGE : llvm::CmpInst::ICMP_SGE;
    else return {VType::Int, l.val};

    res = isFloat ? B->CreateFCmp(pred, lf, rf, "cmp") : B->CreateICmp(pred, l.val, r.val, "cmp");
    return {VType::Bool, res};
}

// ---------- 精确十进制运算（extfloat） ----------
// 任一操作数为 extfloat 时：另一侧按 float/int 提升，运算全部交给运行时十进制例程
LVal Gen::gen_ext_binop(const std::string& op, LVal l, LVal r) {
    auto toext = [&](const LVal& v) -> llvm::Value* {
        if (v.type == VType::ExtFloat) return v.val;
        if (v.type == VType::Float)
            return B->CreateCall(llvm::cast<llvm::Function>(
                decl_vor("vor_ext_from_f64", VType::ExtFloat, {VType::Float})), {v.val}, "e.f");
        llvm::Value* iv = (v.type == VType::Bool)
                              ? B->CreateZExt(v.val, B->getInt64Ty(), "e.b2i") : v.val;
        return B->CreateCall(llvm::cast<llvm::Function>(
            decl_vor("vor_ext_from_i64", VType::ExtFloat, {VType::Int})), {iv}, "e.i");
    };
    // 比较 -> bool
    if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=") {
        llvm::Value* c = B->CreateCall(llvm::cast<llvm::Function>(
            decl_vor("vor_ext_cmp", VType::Int, {VType::ExtFloat, VType::ExtFloat})),
            {toext(l), toext(r)}, "extcmp");
        llvm::Value* zero = B->getInt64(0);
        llvm::Value* pred = nullptr;
        if      (op == "==") pred = B->CreateICmpEQ(c, zero, "ext.eq");
        else if (op == "!=") pred = B->CreateICmpNE(c, zero, "ext.ne");
        else if (op == "<")  pred = B->CreateICmpSLT(c, zero, "ext.lt");
        else if (op == ">")  pred = B->CreateICmpSGT(c, zero, "ext.gt");
        else if (op == "<=") pred = B->CreateICmpSLE(c, zero, "ext.le");
        else                 pred = B->CreateICmpSGE(c, zero, "ext.ge");
        return {VType::Bool, pred};
    }
    const char* fn = nullptr;
    if      (op == "+")  fn = "vor_ext_add";
    else if (op == "-")  fn = "vor_ext_sub";
    else if (op == "*")  fn = "vor_ext_mul";
    else if (op == "/")  fn = "vor_ext_div";
    else if (op == "//") fn = "vor_ext_floordiv";
    else if (op == "%")  fn = "vor_ext_mod";
    else if (op == "**") fn = "vor_ext_pow";
    if (!fn) throw std::runtime_error("operator '" + op + "' is not supported for extfloat");
    llvm::Value* res = B->CreateCall(llvm::cast<llvm::Function>(
        decl_vor(fn, VType::ExtFloat, {VType::ExtFloat, VType::ExtFloat})),
        {toext(l), toext(r)}, "extop");
    return {VType::ExtFloat, res};
}

// ---------- 函数调用 ----------
LVal Gen::gen_call(const CallExpr* c) {
    // class 构造：Point(...) / Point3(...)
    if (c->callee->kind == ExprKind::Identifier) {
        const std::string& cn = static_cast<const IdentifierExpr*>(c->callee.get())->name;
        if (classes_.count(cn)) return gen_construct(cn, c);
        // 外部 C 函数调用（Lib-O3）
        auto eit = extdefs_.find(cn);
        if (eit != extdefs_.end()) return gen_ext_call(eit->second, c);
    }
    // 实例方法调用：obj.method(...)（仅当接收者是已声明的实例变量，避免误伤模块调用）
    if (c->callee->kind == ExprKind::MemberAccess) {
        auto* mm = static_cast<const MemberAccessExpr*>(c->callee.get());
        if (mm->object->kind == ExprKind::Identifier) {
            const std::string& rn = static_cast<const IdentifierExpr*>(mm->object.get())->name;
            LVal probe;
            if (lookup_var(rn, probe) && probe.type == VType::Object) {
                LVal recv = gen_expr(mm->object.get());
                if (recv.type == VType::Object) return gen_method_call(recv, mm->member, c);
            }
        }
    }
    // 成员访问：math.pow 等
    std::string fname;
    if (c->callee->kind == ExprKind::Identifier) {
        fname = static_cast<const IdentifierExpr*>(c->callee.get())->name;
    } else if (c->callee->kind == ExprKind::MemberAccess) {
        auto* m = static_cast<const MemberAccessExpr*>(c->callee.get());
        if (m->object->kind == ExprKind::Identifier) {
            fname = static_cast<const IdentifierExpr*>(m->object.get())->name + "." + m->member;
        }
    }

    // ---- map / filter / reduce（一等函数传参，编译器端支持）----
    // 需在 args 求值前拦截，因为回调（def 名）不是普通表达式
    if (fname == "map" || fname == "filter" || fname == "reduce") {
        if (c->args.size() < 2)
            throw std::runtime_error("'" + fname + "' expects a function and an iterable");
        // 生成回调闭包：lambda 已是 Closure；def 名则用 vor_closure_new 包装函数指针
        auto gen_cb = [&](const ExprPtr& fnexpr) -> llvm::Value* {
            if (fnexpr->kind == ExprKind::Identifier) {
                const std::string& nm = static_cast<const IdentifierExpr*>(fnexpr.get())->name;
                llvm::Function* fn = mod_->getFunction(nm);
                if (fn) {
                    llvm::Value* alloc = B->CreateCall(
                        llvm::cast<llvm::Function>(declare_runtime("vor_closure_new", B->getPtrTy(), {B->getInt64Ty()})),
                        {B->getInt64(0)});
                    llvm::Value* cl = B->CreateBitCast(alloc, B->getInt64Ty()->getPointerTo(), "cb.cl");
                    B->CreateStore(B->CreatePtrToInt(fn, B->getInt64Ty()),
                                   B->CreateGEP(B->getInt64Ty(), cl, B->getInt64(0)));
                    return alloc;
                }
            }
            return gen_expr(fnexpr.get()).val; // lambda → Closure 指针
        };
        // 闭包间接调用：i64 fn(ptr closure, i64...)
        auto call_cb = [&](llvm::Value* closure, std::vector<llvm::Value*> a) -> llvm::Value* {
            llvm::Value* clI64 = B->CreateBitCast(closure, B->getInt64Ty()->getPointerTo());
            llvm::Value* fnraw = B->CreateLoad(B->getInt64Ty(), B->CreateGEP(B->getInt64Ty(), clI64, B->getInt64(0)));
            std::vector<llvm::Type*> ft;
            ft.push_back(B->getPtrTy());
            for (auto& aa : a) ft.push_back(B->getInt64Ty());
            llvm::FunctionType* Ftype = llvm::FunctionType::get(B->getInt64Ty(), ft, false);
            llvm::Value* fn = B->CreateIntToPtr(fnraw, Ftype->getPointerTo());
            std::vector<llvm::Value*> ca; ca.push_back(closure);
            for (auto& aa : a) ca.push_back(aa);
            return B->CreateCall(Ftype, fn, ca, "cb.call");
        };
        llvm::Value* cb = gen_cb(c->args[0]);
        LVal cont = gen_expr(c->args[1].get());
        if (cont.type != VType::List)
            throw std::runtime_error("'" + fname + "': expected a list");
        llvm::Function* F = cur_bb()->getParent();
        llvm::Value* len = B->CreateCall(llvm::cast<llvm::Function>(
            decl_vor("vor_list_len", VType::Int, {VType::List})), {cont.val});

        if (fname == "map") {
            llvm::Value* out = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_list_new", VType::List, {})), {});
            llvm::BasicBlock* cBB = llvm::BasicBlock::Create(B->getContext(), "map.cond", F);
            llvm::BasicBlock* bBB = llvm::BasicBlock::Create(B->getContext(), "map.body", F);
            llvm::BasicBlock* uBB = llvm::BasicBlock::Create(B->getContext(), "map.upd", F);
            llvm::BasicBlock* eBB = llvm::BasicBlock::Create(B->getContext(), "map.end", F);
            llvm::AllocaInst* idx = make_alloca(VType::Int, F, "map.i");
            B->CreateStore(B->getInt64(0), idx);
            B->CreateBr(cBB);
            B->SetInsertPoint(uBB);
            B->CreateStore(B->CreateAdd(B->CreateLoad(B->getInt64Ty(), idx), B->getInt64(1)), idx);
            B->CreateBr(cBB);
            B->SetInsertPoint(cBB);
            B->CreateCondBr(B->CreateICmpSLT(B->CreateLoad(B->getInt64Ty(), idx), len), bBB, eBB);
            B->SetInsertPoint(bBB);
            llvm::Value* el = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_list_get", VType::Int, {VType::List, VType::Int})), {cont.val, B->CreateLoad(B->getInt64Ty(), idx)});
            llvm::Value* r = call_cb(cb, {el});
            B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_list_push", VType::Void, {VType::List, VType::Int})), {out, r});
            B->CreateBr(uBB);
            B->SetInsertPoint(eBB);
            return {VType::List, out};
        }
        if (fname == "filter") {
            llvm::Value* out = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_list_new", VType::List, {})), {});
            llvm::BasicBlock* cBB = llvm::BasicBlock::Create(B->getContext(), "fil.cond", F);
            llvm::BasicBlock* bBB = llvm::BasicBlock::Create(B->getContext(), "fil.body", F);
            llvm::BasicBlock* uBB = llvm::BasicBlock::Create(B->getContext(), "fil.upd", F);
            llvm::BasicBlock* eBB = llvm::BasicBlock::Create(B->getContext(), "fil.end", F);
            llvm::AllocaInst* idx = make_alloca(VType::Int, F, "fil.i");
            B->CreateStore(B->getInt64(0), idx);
            B->CreateBr(cBB);
            B->SetInsertPoint(uBB);
            B->CreateStore(B->CreateAdd(B->CreateLoad(B->getInt64Ty(), idx), B->getInt64(1)), idx);
            B->CreateBr(cBB);
            B->SetInsertPoint(cBB);
            B->CreateCondBr(B->CreateICmpSLT(B->CreateLoad(B->getInt64Ty(), idx), len), bBB, eBB);
            B->SetInsertPoint(bBB);
            llvm::Value* el = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_list_get", VType::Int, {VType::List, VType::Int})), {cont.val, B->CreateLoad(B->getInt64Ty(), idx)});
            llvm::Value* ok = call_cb(cb, {el});
            llvm::BasicBlock* pushBB = llvm::BasicBlock::Create(B->getContext(), "fil.push", F);
            llvm::BasicBlock* skipBB = llvm::BasicBlock::Create(B->getContext(), "fil.skip", F);
            B->CreateCondBr(B->CreateICmpNE(ok, B->getInt64(0)), pushBB, skipBB);
            B->SetInsertPoint(pushBB);
            B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_list_push", VType::Void, {VType::List, VType::Int})), {out, el});
            B->CreateBr(uBB);
            B->SetInsertPoint(skipBB);
            B->CreateBr(uBB);
            B->SetInsertPoint(eBB);
            return {VType::List, out};
        }
        // reduce
        llvm::AllocaInst* acc = make_alloca(VType::Int, F, "acc");
        llvm::Value* startIdx = B->getInt64(1);
        if (c->args.size() >= 3) {
            LVal init = gen_expr(c->args[2].get());
            B->CreateStore(to_i64(init), acc);
            startIdx = B->getInt64(0);
        } else {
            llvm::Value* first = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_list_get", VType::Int, {VType::List, VType::Int})), {cont.val, B->getInt64(0)});
            B->CreateStore(first, acc);
        }
        llvm::BasicBlock* cBB = llvm::BasicBlock::Create(B->getContext(), "red.cond", F);
        llvm::BasicBlock* bBB = llvm::BasicBlock::Create(B->getContext(), "red.body", F);
        llvm::BasicBlock* uBB = llvm::BasicBlock::Create(B->getContext(), "red.upd", F);
        llvm::BasicBlock* eBB = llvm::BasicBlock::Create(B->getContext(), "red.end", F);
        llvm::AllocaInst* idx = make_alloca(VType::Int, F, "red.i");
        B->CreateStore(startIdx, idx);
        B->CreateBr(cBB);
        B->SetInsertPoint(uBB);
        B->CreateStore(B->CreateAdd(B->CreateLoad(B->getInt64Ty(), idx), B->getInt64(1)), idx);
        B->CreateBr(cBB);
        B->SetInsertPoint(cBB);
        B->CreateCondBr(B->CreateICmpSLT(B->CreateLoad(B->getInt64Ty(), idx), len), bBB, eBB);
        B->SetInsertPoint(bBB);
        llvm::Value* el = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_list_get", VType::Int, {VType::List, VType::Int})), {cont.val, B->CreateLoad(B->getInt64Ty(), idx)});
        llvm::Value* nr = call_cb(cb, {B->CreateLoad(B->getInt64Ty(), acc), el});
        B->CreateStore(nr, acc);
        B->CreateBr(uBB);
        B->SetInsertPoint(eBB);
        return {VType::Int, B->CreateLoad(B->getInt64Ty(), acc)};
    }

    // ---- all / any / sorted（无回调，纯遍历/排序）----
    if (fname == "all" || fname == "any" || fname == "sorted") {
        if (c->args.empty()) throw std::runtime_error("'" + fname + "' expects an iterable");
        LVal cont = gen_expr(c->args[0].get());
        if (cont.type != VType::List) throw std::runtime_error("'" + fname + "': expected a list");
        llvm::Function* F = cur_bb()->getParent();
        llvm::Value* len = B->CreateCall(llvm::cast<llvm::Function>(
            decl_vor("vor_list_len", VType::Int, {VType::List})), {cont.val});
        if (fname == "sorted") {
            llvm::Value* out = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_list_new", VType::List, {})), {});
            llvm::BasicBlock* cBB = llvm::BasicBlock::Create(B->getContext(), "srt.cond", F);
            llvm::BasicBlock* bBB = llvm::BasicBlock::Create(B->getContext(), "srt.body", F);
            llvm::BasicBlock* uBB = llvm::BasicBlock::Create(B->getContext(), "srt.upd", F);
            llvm::BasicBlock* eBB = llvm::BasicBlock::Create(B->getContext(), "srt.end", F);
            llvm::AllocaInst* idx = make_alloca(VType::Int, F, "srt.i");
            B->CreateStore(B->getInt64(0), idx); B->CreateBr(cBB);
            B->SetInsertPoint(uBB);
            B->CreateStore(B->CreateAdd(B->CreateLoad(B->getInt64Ty(), idx), B->getInt64(1)), idx);
            B->CreateBr(cBB);
            B->SetInsertPoint(cBB);
            B->CreateCondBr(B->CreateICmpSLT(B->CreateLoad(B->getInt64Ty(), idx), len), bBB, eBB);
            B->SetInsertPoint(bBB);
            llvm::Value* el = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_list_get", VType::Int, {VType::List, VType::Int})), {cont.val, B->CreateLoad(B->getInt64Ty(), idx)});
            B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_list_push", VType::Void, {VType::List, VType::Int})), {out, el});
            B->CreateBr(uBB);
            B->SetInsertPoint(eBB);
            llvm::Value* rev = c->args.size() >= 2 ? to_i64(gen_expr(c->args[1].get())) : B->getInt64(0);
            B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_list_sort", VType::Void, {VType::List, VType::Int})), {out, rev});
            return {VType::List, out};
        }
        // all / any
        llvm::AllocaInst* flag = make_alloca(VType::Bool, F, fname == "all" ? "all.f" : "any.f");
        B->CreateStore(fname == "all" ? B->getInt1(1) : B->getInt1(0), flag);
        llvm::BasicBlock* cBB = llvm::BasicBlock::Create(B->getContext(), (fname+"c").c_str(), F);
        llvm::BasicBlock* bBB = llvm::BasicBlock::Create(B->getContext(), (fname+"b").c_str(), F);
        llvm::BasicBlock* uBB = llvm::BasicBlock::Create(B->getContext(), (fname+"u").c_str(), F);
        llvm::BasicBlock* eBB = llvm::BasicBlock::Create(B->getContext(), (fname+"e").c_str(), F);
        llvm::AllocaInst* idx = make_alloca(VType::Int, F, (fname+"i").c_str());
        B->CreateStore(B->getInt64(0), idx); B->CreateBr(cBB);
        B->SetInsertPoint(uBB);
        B->CreateStore(B->CreateAdd(B->CreateLoad(B->getInt64Ty(), idx), B->getInt64(1)), idx);
        B->CreateBr(cBB);
        B->SetInsertPoint(cBB);
        B->CreateCondBr(B->CreateICmpSLT(B->CreateLoad(B->getInt64Ty(), idx), len), bBB, eBB);
        B->SetInsertPoint(bBB);
        llvm::Value* el = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_list_get", VType::Int, {VType::List, VType::Int})), {cont.val, B->CreateLoad(B->getInt64Ty(), idx)});
        if (fname == "all") {
            llvm::BasicBlock* failBB = llvm::BasicBlock::Create(B->getContext(), "all.fail", F);
            B->CreateCondBr(B->CreateICmpEQ(el, B->getInt64(0)), failBB, uBB);
            B->SetInsertPoint(failBB);
            B->CreateStore(B->getInt1(0), flag);
            B->CreateBr(eBB); // 遇 0 即短路 false
        } else {
            llvm::BasicBlock* hitBB = llvm::BasicBlock::Create(B->getContext(), "any.hit", F);
            B->CreateCondBr(B->CreateICmpNE(el, B->getInt64(0)), hitBB, uBB);
            B->SetInsertPoint(hitBB);
            B->CreateStore(B->getInt1(1), flag);
            B->CreateBr(eBB); // 提前退出
        }
        B->SetInsertPoint(eBB);
        return {VType::Bool, B->CreateLoad(B->getInt1Ty(), flag)};
    }

    auto it = fns_.find(fname);
    if (it != fns_.end()) {
        // print / math 内置
        if (it->second.second == Builtin::Print) {
            // 多参 print 以单个空格分隔，与解释器对齐
            llvm::Value* comma = nullptr;
            bool first = true;
            for (auto& a : c->args) {
                if (!first) {
                    if (!comma)
                        comma = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_str_from_cstr", VType::Str, {VType::Str})),
                                              {B->CreateGlobalString(" ", "sep")});
                    B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_print_str", VType::Void, {VType::Str})), {comma});
                }
                first = false;
                LVal v = gen_expr(a.get());
                emit_any(v);
            }
            B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_print_newline", VType::Void, {})), {});
            return {VType::Void, nullptr};
        }
        if (it->second.second == Builtin::Pow) {
            LVal a0 = gen_expr(c->args[0].get());
            LVal a1 = gen_expr(c->args[1].get());
            llvm::Value* fn = decl_vor("vor_pow", VType::Float, {VType::Float, VType::Float});
            llvm::Value* f = B->CreateSIToFP(a0.val, B->getDoubleTy());
            llvm::Value* g = B->CreateSIToFP(a1.val, B->getDoubleTy());
            return {VType::Float, B->CreateCall(llvm::cast<llvm::Function>(fn), {f, g})};
        }
        // sqrt/fabs 单位参数
        if (it->second.second == Builtin::Sqrt || it->second.second == Builtin::Fabs) {
            const char* nm = it->second.second == Builtin::Sqrt ? "vor_sqrt" : "vor_fabs";
            LVal a0 = gen_expr(c->args[0].get());
            llvm::Value* fn = decl_vor(nm, VType::Float, {VType::Float});
            llvm::Value* f = B->CreateSIToFP(a0.val, B->getDoubleTy());
            return {VType::Float, B->CreateCall(llvm::cast<llvm::Function>(fn), {f})};
        }
    }

    // ---- 扩展模块转发：time./random./log.（P4 批A） ----
    // 标量/str 参数与返回 float/int/str；参数 int->float 自动提升。
    auto to_fp = [&](LVal v) -> llvm::Value* {
        if (v.type == VType::Float) return v.val;
        return B->CreateSIToFP(v.val, B->getDoubleTy(), "a.f");
    };
    auto nothing = [&]() { return LVal{VType::Void, nullptr}; };

    // ---- 扩展模块转发：math.*（与解释器 math 模块对齐） ----
    // 一元 float->float
    auto mf1 = [&](const char* rf) -> LVal {
        LVal a0 = gen_expr(c->args[0].get());
        llvm::Value* fn = decl_vor(rf, VType::Float, {VType::Float});
        return {VType::Float, B->CreateCall(llvm::cast<llvm::Function>(fn), {to_fp(a0)})};
    };
    // 二元 float->float
    auto mf2 = [&](const char* rf) -> LVal {
        LVal a0 = gen_expr(c->args[0].get());
        LVal a1 = gen_expr(c->args[1].get());
        llvm::Value* fn = decl_vor(rf, VType::Float, {VType::Float, VType::Float});
        return {VType::Float, B->CreateCall(llvm::cast<llvm::Function>(fn), {to_fp(a0), to_fp(a1)})};
    };
    // 一元 double->int（floor/ceil/trunc）
    auto mi1 = [&](const char* rf) -> LVal {
        LVal a0 = gen_expr(c->args[0].get());
        llvm::Value* fn = decl_vor(rf, VType::Int, {VType::Float});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {to_fp(a0)})};
    };
    if (fname == "math.cbrt")   return mf1("vor_cbrt");
    if (fname == "math.exp")    return mf1("vor_exp");
    if (fname == "math.log") {
        if (c->args.size() >= 2) return mf2("vor_logbase"); // 指定底
        return mf1("vor_log");
    }
    if (fname == "math.log2")   return mf1("vor_log2");
    if (fname == "math.log10")  return mf1("vor_log10");
    if (fname == "math.log1p")  return mf1("vor_log1p");
    if (fname == "math.expm1")  return mf1("vor_expm1");
    if (fname == "math.erf")    return mf1("vor_erf");
    if (fname == "math.gamma")  return mf1("vor_tgamma");
    if (fname == "math.lgamma") return mf1("vor_lgamma");
    if (fname == "math.sin")    return mf1("vor_sin");
    if (fname == "math.cos")    return mf1("vor_cos");
    if (fname == "math.tan")    return mf1("vor_tan");
    if (fname == "math.asin")   return mf1("vor_asin");
    if (fname == "math.acos")   return mf1("vor_acos");
    if (fname == "math.atan")   return mf1("vor_atan");
    if (fname == "math.atan2")  return mf2("vor_atan2");
    if (fname == "math.sinh")   return mf1("vor_sinh");
    if (fname == "math.cosh")   return mf1("vor_cosh");
    if (fname == "math.tanh")   return mf1("vor_tanh");
    if (fname == "math.asinh")  return mf1("vor_asinh");
    if (fname == "math.acosh")  return mf1("vor_acosh");
    if (fname == "math.atanh")  return mf1("vor_atanh");
    if (fname == "math.hypot")  return mf2("vor_hypot");
    if (fname == "math.radians") return mf1("vor_radians");
    if (fname == "math.degrees") return mf1("vor_degrees");
    if (fname == "math.copysign") return mf2("vor_copysign");
    if (fname == "math.remainder") return mf2("vor_remainder");
    if (fname == "math.floor")  return mi1("vor_floor");
    if (fname == "math.ceil")   return mi1("vor_ceil");
    if (fname == "math.trunc")  return mi1("vor_trunc");
    if (fname == "math.fmod")   return mf2("vor_fmod");
    if (fname == "math.fmin")   return mf2("vor_fmin");
    if (fname == "math.fmax")   return mf2("vor_fmax");
    if (fname == "math.round") {
        LVal a0 = gen_expr(c->args[0].get());
        llvm::Value* fn = decl_vor("vor_roundn", VType::Float, {VType::Float, VType::Int});
        llvm::Value* nd = c->args.size() >= 2 ? to_i64(gen_expr(c->args[1].get())) : B->getInt64(0);
        return {VType::Float, B->CreateCall(llvm::cast<llvm::Function>(fn), {to_fp(a0), nd})};
    }
    if (fname == "math.gcd") {
        llvm::Value* fn = decl_vor("vor_gcd", VType::Int, {VType::Int, VType::Int});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn),
            {to_i64(gen_expr(c->args[0].get())), to_i64(gen_expr(c->args[1].get()))})};
    }
    if (fname == "math.ipow") {
        llvm::Value* fn = decl_vor("vor_ipow", VType::Int, {VType::Int, VType::Int});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn),
            {to_i64(gen_expr(c->args[0].get())), to_i64(gen_expr(c->args[1].get()))})};
    }
    if (fname == "math.comb" || fname == "math.perm") {
        const char* rf = fname == "math.comb" ? "vor_comb" : "vor_perm";
        llvm::Value* fn = decl_vor(rf, VType::Int, {VType::Int, VType::Int});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn),
            {to_i64(gen_expr(c->args[0].get())), to_i64(gen_expr(c->args[1].get()))})};
    }
    if (fname == "math.factorial") {
        llvm::Value* fn = decl_vor("vor_factorial", VType::Int, {VType::Int});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn),
            {to_i64(gen_expr(c->args[0].get()))})};
    }
    if (fname == "math.isqrt") {
        llvm::Value* fn = decl_vor("vor_isqrt", VType::Int, {VType::Int});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn),
            {to_i64(gen_expr(c->args[0].get()))})};
    }
    if (fname == "math.lcm") {
        llvm::Value* fn = decl_vor("vor_lcm", VType::Int, {VType::Int, VType::Int});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn),
            {to_i64(gen_expr(c->args[0].get())), to_i64(gen_expr(c->args[1].get()))})};
    }
    if (fname == "math.isinf" || fname == "math.isnan" || fname == "math.isfinite") {
        const char* rf = fname == "math.isinf" ? "vor_isinf"
                       : fname == "math.isnan" ? "vor_isnan" : "vor_isfinite";
        LVal a0 = gen_expr(c->args[0].get());
        llvm::Value* fn = decl_vor(rf, VType::Int, {VType::Float});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {to_fp(a0)})};
    }
    if (fname == "math.isclose") {
        LVal a0 = gen_expr(c->args[0].get());
        LVal a1 = gen_expr(c->args[1].get());
        llvm::Value* rt = c->args.size() >= 3 ? to_fp(gen_expr(c->args[2].get())) : llvm::ConstantFP::get(B->getDoubleTy(), 1e-9);
        llvm::Value* at = c->args.size() >= 4 ? to_fp(gen_expr(c->args[3].get())) : llvm::ConstantFP::get(B->getDoubleTy(), 0.0);
        llvm::Value* fn = decl_vor("vor_isclose", VType::Int, {VType::Float, VType::Float, VType::Float, VType::Float});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {to_fp(a0), to_fp(a1), rt, at})};
    }
    // abs：按实参类型选择 int/float 路径
    if (fname == "math.abs") {
        LVal a0 = gen_expr(c->args[0].get());
        if (a0.type == VType::Float) {
            llvm::Value* fn = decl_vor("vor_fabs", VType::Float, {VType::Float});
            return {VType::Float, B->CreateCall(llvm::cast<llvm::Function>(fn), {a0.val})};
        }
        llvm::Value* fn = decl_vor("vor_iabs", VType::Int, {VType::Int});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {to_i64(a0)})};
    }
    // min/max：任一是 float 则取浮点，否则整数
    if (fname == "math.min" || fname == "math.max") {
        LVal a0 = gen_expr(c->args[0].get());
        LVal a1 = gen_expr(c->args[1].get());
        const bool f = (a0.type == VType::Float || a1.type == VType::Float);
        if (f) {
            const char* rf = fname == "math.min" ? "vor_fmin" : "vor_fmax";
            llvm::Value* fn = decl_vor(rf, VType::Float, {VType::Float, VType::Float});
            return {VType::Float, B->CreateCall(llvm::cast<llvm::Function>(fn), {to_fp(a0), to_fp(a1)})};
        }
        const char* rf = fname == "math.min" ? "vor_imin" : "vor_imax";
        llvm::Value* fn = decl_vor(rf, VType::Int, {VType::Int, VType::Int});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {to_i64(a0), to_i64(a1)})};
    }
    if (fname == "math.fabs") { // 与内置一致
        LVal a0 = gen_expr(c->args[0].get());
        llvm::Value* fn = decl_vor("vor_fabs", VType::Float, {VType::Float});
        return {VType::Float, B->CreateCall(llvm::cast<llvm::Function>(fn), {to_fp(a0)})};
    }

    // type(x)：返回类型名字符串（编译期常量；元组为 auto_generated_class(...)）
    if (fname == "type") {
        if (c->args.empty()) throw std::runtime_error("type() expects one argument");
        std::string desc = type_desc_of_expr(c->args[0].get());
        gen_expr(c->args[0].get());   // 仍然求值实参，保留副作用/报错行为
        llvm::Value* fn = decl_vor("vor_str_from_cstr", VType::Str, {VType::Str});
        llvm::Value* g = B->CreateGlobalString(desc, "tyname");
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {g})};
    }

    // len(String) / len(List) / len(Dict)：转发运行时长度
    if (fname == "len") {
        LVal v = gen_expr(c->args[0].get());
        if (v.type == VType::Str) {
            llvm::Value* fn = decl_vor("vor_str_len", VType::Int, {VType::Str});
            return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {v.val})};
        }
        if (v.type == VType::List || v.type == VType::StrList) {
            llvm::Value* fn = decl_vor("vor_list_len", VType::Int, {VType::List});
            return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {v.val})};
        }
        if (v.type == VType::Dict) {
            llvm::Value* fn = decl_vor("vor_dict_len", VType::Int, {VType::Dict});
            return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {v.val})};
        }
        if (v.type == VType::Tuple) {
            llvm::Value* fn = decl_vor("vor_tuple_len", VType::Int, {VType::Tuple});
            return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {v.val})};
        }
    }
    // sum(List)/prod(List)：数值列表求和/求积
    if (fname == "sum" || fname == "prod") {
        LVal v = gen_expr(c->args[0].get());
        if (v.type == VType::List) {
            const char* rf = fname == "sum" ? "vor_list_sum" : "vor_list_prod";
            llvm::Value* fn = decl_vor(rf, VType::Int, {VType::List});
            return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {v.val})};
        }
    }

    if (fname == "time.time") {
        llvm::Value* fn = decl_vor("vor_time_now", VType::Float, {});
        return {VType::Float, B->CreateCall(llvm::cast<llvm::Function>(fn), {})};
    }
    if (fname == "time.sleep") {
        LVal a0 = gen_expr(c->args[0].get());
        llvm::Value* fn = decl_vor("vor_sleep", VType::Void, {VType::Float});
        B->CreateCall(llvm::cast<llvm::Function>(fn), {to_fp(a0)});
        return nothing();
    }
    if (fname == "time.counter") {
        llvm::Value* fn = decl_vor("vor_counter", VType::Float, {});
        return {VType::Float, B->CreateCall(llvm::cast<llvm::Function>(fn), {})};
    }
    if (fname == "time.reset_counter") {
        llvm::Value* fn = decl_vor("vor_counter_reset", VType::Void, {});
        B->CreateCall(llvm::cast<llvm::Function>(fn), {});
        return nothing();
    }
    if (fname == "time.process_time") {
        llvm::Value* fn = decl_vor("vor_process_time", VType::Float, {});
        return {VType::Float, B->CreateCall(llvm::cast<llvm::Function>(fn), {})};
    }

    if (fname == "random.seed") {
        LVal a0 = gen_expr(c->args[0].get());
        llvm::Value* fn = decl_vor("vor_rng_seed", VType::Void, {VType::Int});
        B->CreateCall(llvm::cast<llvm::Function>(fn), {to_i64(a0)});
        return nothing();
    }
    if (fname == "random.random") {
        llvm::Value* fn = decl_vor("vor_rng_random", VType::Float, {});
        return {VType::Float, B->CreateCall(llvm::cast<llvm::Function>(fn), {})};
    }
    if (fname == "random.uniform") {
        LVal a0 = gen_expr(c->args[0].get()); LVal a1 = gen_expr(c->args[1].get());
        llvm::Value* fn = decl_vor("vor_rng_uniform", VType::Float, {VType::Float, VType::Float});
        return {VType::Float, B->CreateCall(llvm::cast<llvm::Function>(fn), {to_fp(a0), to_fp(a1)})};
    }
    if (fname == "random.randint") {
        LVal a0 = gen_expr(c->args[0].get()); LVal a1 = gen_expr(c->args[1].get());
        llvm::Value* fn = decl_vor("vor_rng_randint", VType::Int, {VType::Int, VType::Int});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {to_i64(a0), to_i64(a1)})};
    }
    if (fname == "random.getrandbits") {
        LVal a0 = gen_expr(c->args[0].get());
        llvm::Value* fn = decl_vor("vor_rng_getrandbits", VType::Int, {VType::Int});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {to_i64(a0)})};
    }
    if (fname == "random.randrange") {
        LVal a0 = gen_expr(c->args[0].get()); LVal a1 = gen_expr(c->args[1].get());
        llvm::Value* step = c->args.size() >= 3 ? to_i64(gen_expr(c->args[2].get())) : B->getInt64(1);
        llvm::Value* fn = decl_vor("vor_rng_randrange", VType::Int, {VType::Int, VType::Int, VType::Int});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {to_i64(a0), to_i64(a1), step})};
    }
    if (fname == "random.gauss" || fname == "random.normalvariate") {
        LVal a0 = gen_expr(c->args[0].get()); LVal a1 = gen_expr(c->args[1].get());
        llvm::Value* fn = decl_vor("vor_rng_gauss", VType::Float, {VType::Float, VType::Float});
        return {VType::Float, B->CreateCall(llvm::cast<llvm::Function>(fn), {to_fp(a0), to_fp(a1)})};
    }
    if (fname == "random.expovariate") {
        LVal a0 = gen_expr(c->args[0].get());
        llvm::Value* fn = decl_vor("vor_rng_expovariate", VType::Float, {VType::Float});
        return {VType::Float, B->CreateCall(llvm::cast<llvm::Function>(fn), {to_fp(a0)})};
    }
    if (fname == "random.triangular") {
        LVal a0 = gen_expr(c->args[0].get()); LVal a1 = gen_expr(c->args[1].get());
        llvm::Value* mode = c->args.size() >= 3
            ? to_fp(gen_expr(c->args[2].get()))
            : B->CreateFMul(B->CreateFAdd(to_fp(a0), to_fp(a1)), llvm::ConstantFP::get(B->getDoubleTy(), 0.5), "mid");
        llvm::Value* fn = decl_vor("vor_rng_triangular", VType::Float, {VType::Float, VType::Float, VType::Float});
        return {VType::Float, B->CreateCall(llvm::cast<llvm::Function>(fn), {to_fp(a0), to_fp(a1), mode})};
    }
    if (fname == "random.getstate") {
        llvm::Value* fn = decl_vor("vor_rng_getstate", VType::Str, {});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {})};
    }
    if (fname == "random.setstate") {
        LVal a0 = gen_expr(c->args[0].get());
        llvm::Value* fn = decl_vor("vor_rng_setstate", VType::Void, {VType::Str});
        B->CreateCall(llvm::cast<llvm::Function>(fn), {a0.val});
        return nothing();
    }

    // log 发射：可标量/str 参数，空格拼接
    auto val_to_str = [&](LVal v) -> llvm::Value* {
        if (v.type == VType::Str) return v.val;
        if (v.type == VType::Int) {
            llvm::Value* fn = decl_vor("vor_i64_to_str", VType::Str, {VType::Int});
            return B->CreateCall(llvm::cast<llvm::Function>(fn), {v.val});
        }
        if (v.type == VType::Float) {
            llvm::Value* fn = decl_vor("vor_double_to_str", VType::Str, {VType::Float});
            return B->CreateCall(llvm::cast<llvm::Function>(fn), {v.val});
        }
        if (v.type == VType::Bool) {
            llvm::Value* tstr = B->CreateGlobalString("true", "bstr");
            llvm::Value* fstr = B->CreateGlobalString("false", "bstr");
            llvm::Value* sel = B->CreateSelect(v.val, tstr, fstr);
            llvm::Value* fn = decl_vor("vor_str_from_cstr", VType::Str, {VType::Str});
            return B->CreateCall(llvm::cast<llvm::Function>(fn), {sel});
        }
        return B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_str_from_cstr", VType::Str, {VType::Str})),
                             {B->CreateGlobalString("", "estr")});
    };
    auto join_log = [&](std::vector<llvm::Value*>& parts) -> llvm::Value* {
        llvm::Value* acc = parts[0];
        for (size_t i = 1; i < parts.size(); ++i) {
            llvm::Value* sp = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_str_from_cstr", VType::Str, {VType::Str})),
                                            {B->CreateGlobalString(" ", "sp")});
            llvm::Value* t1 = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_str_concat", VType::Str, {VType::Str, VType::Str})), {acc, sp});
            llvm::Value* t2 = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_str_concat", VType::Str, {VType::Str, VType::Str})), {t1, parts[i]});
            acc = t2;
        }
        return acc;
    };
    static const int LOG_LEVELS[5] = {10, 20, 30, 40, 50};
    const char* log_emit_names[5] = {"log.debug", "log.info", "log.warn", "log.error", "log.fatal"};
    for (int li = 0; li < 5; ++li) {
        if (fname == log_emit_names[li]) {
            std::vector<llvm::Value*> parts;
            for (auto& a : c->args) parts.push_back(val_to_str(gen_expr(a.get())));
            llvm::Value* msg = join_log(parts);
            llvm::Value* fn = decl_vor("vor_log_emit", VType::Void, {VType::Int, VType::Str});
            B->CreateCall(llvm::cast<llvm::Function>(fn), {B->getInt64(LOG_LEVELS[li]), msg});
            return nothing();
        }
    }
    if (fname == "log.level") {
        LVal a0 = gen_expr(c->args[0].get());
        llvm::Value* fn = decl_vor("vor_log_level", VType::Void, {VType::Str});
        B->CreateCall(llvm::cast<llvm::Function>(fn), {val_to_str(a0)});
        return nothing();
    }
    if (fname == "log.get_level") {
        llvm::Value* fn = decl_vor("vor_log_get_level", VType::Str, {});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {})};
    }
    if (fname == "log.format") {
        LVal a0 = gen_expr(c->args[0].get());
        llvm::Value* fn = decl_vor("vor_log_format", VType::Void, {VType::Str});
        B->CreateCall(llvm::cast<llvm::Function>(fn), {val_to_str(a0)});
        return nothing();
    }
    if (fname == "log.file") {
        LVal a0 = gen_expr(c->args[0].get());
        llvm::Value* fn = decl_vor("vor_log_file", VType::Void, {VType::Str});
        B->CreateCall(llvm::cast<llvm::Function>(fn), {val_to_str(a0)});
        return nothing();
    }
    if (fname == "log.console") {
        LVal a0 = gen_expr(c->args[0].get());
        llvm::Value* fn = decl_vor("vor_log_console", VType::Void, {VType::Int});
        B->CreateCall(llvm::cast<llvm::Function>(fn), {to_i64(a0)});
        return nothing();
    }

    // ---- 扩展模块转发：file.（读/写/追加/存在/删除/改名/大小/目录判断/建目录） ----
    // 字符串参数统一取 cstr/as_cstr；返回 str / bool / int / strlist。
    if (fname == "file.readlines") {
        llvm::Value* p = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_file_readlines", VType::StrList, {VType::Str});
        return {VType::StrList, B->CreateCall(llvm::cast<llvm::Function>(fn), {p})};
    }
    if (fname == "file.listdir") {
        llvm::Value* p = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_file_listdir", VType::StrList, {VType::Str});
        return {VType::StrList, B->CreateCall(llvm::cast<llvm::Function>(fn), {p})};
    }
    if (fname == "file.read") {
        llvm::Value* p = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_file_read", VType::Str, {VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {p})};
    }
    if (fname == "file.write" || fname == "file.append") {
        llvm::Value* p = val_to_str(gen_expr(c->args[0].get()));
        LVal v = gen_expr(c->args[1].get());
        llvm::Value* fn = decl_vor(fname == "file.write" ? "vor_file_write" : "vor_file_append",
                                  VType::Void, {VType::Str, VType::Str});
        B->CreateCall(llvm::cast<llvm::Function>(fn), {p, val_to_str(v)});
        return nothing();
    }
    if (fname == "file.exists") {
        llvm::Value* p = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_file_exists", VType::Int, {VType::Str});
        return {VType::Bool, B->CreateCall(llvm::cast<llvm::Function>(fn), {p})};
    }
    if (fname == "file.remove") {
        llvm::Value* p = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_file_remove", VType::Int, {VType::Str});
        return {VType::Bool, B->CreateCall(llvm::cast<llvm::Function>(fn), {p})};
    }
    if (fname == "file.rename") {
        llvm::Value* p0 = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* p1 = val_to_str(gen_expr(c->args[1].get()));
        llvm::Value* fn = decl_vor("vor_file_rename", VType::Int, {VType::Str, VType::Str});
        return {VType::Bool, B->CreateCall(llvm::cast<llvm::Function>(fn), {p0, p1})};
    }
    if (fname == "file.size") {
        llvm::Value* p = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_file_size", VType::Int, {VType::Str});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {p})};
    }
    if (fname == "file.isdir") {
        llvm::Value* p = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_file_isdir", VType::Int, {VType::Str});
        return {VType::Bool, B->CreateCall(llvm::cast<llvm::Function>(fn), {p})};
    }
    if (fname == "file.isfile") {
        llvm::Value* p = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_file_isfile", VType::Int, {VType::Str});
        return {VType::Bool, B->CreateCall(llvm::cast<llvm::Function>(fn), {p})};
    }
    if (fname == "file.mkdir") {
        llvm::Value* p = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_file_mkdir", VType::Int, {VType::Str});
        return {VType::Bool, B->CreateCall(llvm::cast<llvm::Function>(fn), {p})};
    }
    if (fname == "file.rmdir") {
        llvm::Value* p = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_file_rmdir", VType::Int, {VType::Str});
        return {VType::Bool, B->CreateCall(llvm::cast<llvm::Function>(fn), {p})};
    }

    // ---- 扩展模块转发：zip.（add / extract / count / has） ----
    if (fname == "zip.add") {
        llvm::Value* zp = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* nm = val_to_str(gen_expr(c->args[1].get()));
        llvm::Value* dt = val_to_str(gen_expr(c->args[2].get()));
        llvm::Value* fn = decl_vor("vor_zip_add", VType::Void, {VType::Str, VType::Str, VType::Str});
        B->CreateCall(llvm::cast<llvm::Function>(fn), {zp, nm, dt});
        return nothing();
    }
    if (fname == "zip.extract") {
        llvm::Value* zp = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* nm = val_to_str(gen_expr(c->args[1].get()));
        llvm::Value* fn = decl_vor("vor_zip_extract", VType::Str, {VType::Str, VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {zp, nm})};
    }
    if (fname == "zip.count") {
        llvm::Value* zp = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_zip_count", VType::Int, {VType::Str});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {zp})};
    }
    if (fname == "zip.names") {
        llvm::Value* zp = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_zip_names", VType::StrList, {VType::Str});
        return {VType::StrList, B->CreateCall(llvm::cast<llvm::Function>(fn), {zp})};
    }
    if (fname == "zip.has") {
        llvm::Value* zp = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* nm = val_to_str(gen_expr(c->args[1].get()));
        llvm::Value* fn = decl_vor("vor_zip_has", VType::Int, {VType::Str, VType::Str});
        return {VType::Bool, B->CreateCall(llvm::cast<llvm::Function>(fn), {zp, nm})};
    }

    // ---- 扩展模块转发：xml.（escape / unescape / parse_text） ----
    if (fname == "xml.escape") {
        llvm::Value* s = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_xml_escape", VType::Str, {VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {s})};
    }
    if (fname == "xml.unescape") {
        llvm::Value* s = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_xml_unescape", VType::Str, {VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {s})};
    }
    if (fname == "xml.parse_text") {
        llvm::Value* x = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* t = val_to_str(gen_expr(c->args[1].get()));
        llvm::Value* fn = decl_vor("vor_xml_parse_text", VType::Str, {VType::Str, VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {x, t})};
    }

    // ---- 扩展模块转发：html.（escape / unescape / strip_tags） ----
    if (fname == "html.escape") {
        llvm::Value* s = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_html_escape", VType::Str, {VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {s})};
    }
    if (fname == "html.unescape") {
        llvm::Value* s = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_html_unescape", VType::Str, {VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {s})};
    }
    if (fname == "html.strip_tags") {
        llvm::Value* s = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_html_strip_tags", VType::Str, {VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {s})};
    }

    // ---- 扩展模块转发：sql.（open / close / execute / table_exists） ----
    // db 句柄为不透明 void*（VType::Str=ptr 槽）传递。
    if (fname == "sql.open") {
        llvm::Value* p = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_sql_open", VType::Str, {VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {p})};
    }
    if (fname == "sql.close") {
        llvm::Value* h = gen_expr(c->args[0].get()).val;
        llvm::Value* fn = decl_vor("vor_sql_close", VType::Void, {VType::Str});
        B->CreateCall(llvm::cast<llvm::Function>(fn), {h});
        return nothing();
    }
    if (fname == "sql.execute") {
        llvm::Value* h = gen_expr(c->args[0].get()).val;
        llvm::Value* s = val_to_str(gen_expr(c->args[1].get()));
        llvm::Value* fn = decl_vor("vor_sql_execute", VType::Int, {VType::Str, VType::Str});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {h, s})};
    }
    if (fname == "sql.table_exists") {
        llvm::Value* h = gen_expr(c->args[0].get()).val;
        llvm::Value* n = val_to_str(gen_expr(c->args[1].get()));
        llvm::Value* fn = decl_vor("vor_sql_table_exists", VType::Int, {VType::Str, VType::Str});
        return {VType::Bool, B->CreateCall(llvm::cast<llvm::Function>(fn), {h, n})};
    }
    if (fname == "sql.query_one") {
        llvm::Value* h = gen_expr(c->args[0].get()).val;
        llvm::Value* s = val_to_str(gen_expr(c->args[1].get()));
        llvm::Value* fn = decl_vor("vor_sql_query_one", VType::Str, {VType::Str, VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {h, s})};
    }
    if (fname == "sql.query") {
        llvm::Value* h = gen_expr(c->args[0].get()).val;
        llvm::Value* s = val_to_str(gen_expr(c->args[1].get()));
        llvm::Value* fn = decl_vor("vor_sql_query", VType::StrListList, {VType::Str, VType::Str});
        return {VType::StrListList, B->CreateCall(llvm::cast<llvm::Function>(fn), {h, s})};
    }

    // ---- 扩展模块转发：os.（getenv/setenv/cwd/pid/platform/home/tempdir/path_join） ----
    if (fname == "os.getenv") {
        llvm::Value* n = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_os_getenv", VType::Str, {VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {n})};
    }
    if (fname == "os.hasenv") {
        llvm::Value* n = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_os_hasenv", VType::Int, {VType::Str});
        return {VType::Bool, B->CreateCall(llvm::cast<llvm::Function>(fn), {n})};
    }
    if (fname == "os.setenv") {
        llvm::Value* n = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* v = val_to_str(gen_expr(c->args[1].get()));
        llvm::Value* fn = decl_vor("vor_os_setenv", VType::Int, {VType::Str, VType::Str});
        return {VType::Bool, B->CreateCall(llvm::cast<llvm::Function>(fn), {n, v})};
    }
    if (fname == "os.unsetenv") {
        llvm::Value* n = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_os_unsetenv", VType::Int, {VType::Str});
        return {VType::Bool, B->CreateCall(llvm::cast<llvm::Function>(fn), {n})};
    }
    if (fname == "os.cwd") {
        llvm::Value* fn = decl_vor("vor_os_cwd", VType::Str, {});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {})};
    }
    if (fname == "os.chdir") {
        llvm::Value* p = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_os_chdir", VType::Int, {VType::Str});
        return {VType::Bool, B->CreateCall(llvm::cast<llvm::Function>(fn), {p})};
    }
    if (fname == "os.pid") {
        llvm::Value* fn = decl_vor("vor_os_pid", VType::Int, {});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {})};
    }
    if (fname == "os.platform") {
        llvm::Value* fn = decl_vor("vor_os_platform", VType::Str, {});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {})};
    }
    if (fname == "os.home") {
        llvm::Value* fn = decl_vor("vor_os_home", VType::Str, {});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {})};
    }
    if (fname == "os.tempdir") {
        llvm::Value* fn = decl_vor("vor_os_tempdir", VType::Str, {});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {})};
    }
    if (fname == "os.path_join") {
        llvm::Value* a = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* b = val_to_str(gen_expr(c->args[1].get()));
        llvm::Value* fn = decl_vor("vor_os_path_join", VType::Str, {VType::Str, VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {a, b})};
    }
    if (fname == "os.basename" || fname == "os.dirname" || fname == "os.extname") {
        const char* rf = fname == "os.basename" ? "vor_os_basename"
                       : fname == "os.dirname" ? "vor_os_dirname" : "vor_os_extname";
        llvm::Value* a = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor(rf, VType::Str, {VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {a})};
    }
    if (fname == "os.argc") {
        llvm::Value* fn = decl_vor("vor_os_argc", VType::Int, {});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {})};
    }
    if (fname == "os.arg") {
        llvm::Value* i = to_i64(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_os_arg", VType::Str, {VType::Int});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {i})};
    }
    if (fname == "os.listdir") {
        llvm::Value* p = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_os_listdir", VType::StrList, {VType::Str});
        return {VType::StrList, B->CreateCall(llvm::cast<llvm::Function>(fn), {p})};
    }

    // ---- 扩展模块转发：regex.（valid/match/search/find/find_all/replace/count/escape） ----
    if (fname == "regex.escape") {
        llvm::Value* s = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_regex_escape", VType::Str, {VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {s})};
    }
    if (fname == "regex.split") {
        llvm::Value* p = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* s = val_to_str(gen_expr(c->args[1].get()));
        llvm::Value* fn = decl_vor("vor_regex_split", VType::StrList, {VType::Str, VType::Str});
        return {VType::StrList, B->CreateCall(llvm::cast<llvm::Function>(fn), {p, s})};
    }
    if (fname == "regex.valid" || fname == "regex.match" || fname == "regex.search") {
        const char* rf = fname == "regex.valid" ? "vor_regex_valid"
                       : fname == "regex.match" ? "vor_regex_match" : "vor_regex_search";
        llvm::Value* p = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* s = fname == "regex.valid" ? nullptr : val_to_str(gen_expr(c->args[1].get()));
        llvm::Value* fn = decl_vor(rf, VType::Int, fname == "regex.valid"
                                   ? std::vector<VType>{VType::Str} : std::vector<VType>{VType::Str, VType::Str});
        llvm::Value* r = fname == "regex.valid"
            ? B->CreateCall(llvm::cast<llvm::Function>(fn), {p})
            : B->CreateCall(llvm::cast<llvm::Function>(fn), {p, s});
        return {VType::Bool, r};
    }
    if (fname == "regex.find" || fname == "regex.find_all") {
        const char* rf = fname == "regex.find" ? "vor_regex_find" : "vor_regex_find_all";
        llvm::Value* p = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* s = val_to_str(gen_expr(c->args[1].get()));
        llvm::Value* fn = decl_vor(rf, VType::Str, {VType::Str, VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {p, s})};
    }
    if (fname == "regex.replace") {
        llvm::Value* p = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* s = val_to_str(gen_expr(c->args[1].get()));
        llvm::Value* r = val_to_str(gen_expr(c->args[2].get()));
        llvm::Value* fn = decl_vor("vor_regex_replace", VType::Str, {VType::Str, VType::Str, VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {p, s, r})};
    }
    if (fname == "regex.count") {
        llvm::Value* p = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* s = val_to_str(gen_expr(c->args[1].get()));
        llvm::Value* fn = decl_vor("vor_regex_count", VType::Int, {VType::Str, VType::Str});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {p, s})};
    }

    // ---- 扩展模块转发：json.（valid/parse_str/parse_int/parse_float/parse_bool/stringify_*） ----
    if (fname == "json.valid") {
        llvm::Value* s = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_json_valid", VType::Int, {VType::Str});
        return {VType::Bool, B->CreateCall(llvm::cast<llvm::Function>(fn), {s})};
    }
    if (fname == "json.parse_array") {
        llvm::Value* s = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_json_parse_array", VType::StrList, {VType::Str});
        return {VType::StrList, B->CreateCall(llvm::cast<llvm::Function>(fn), {s})};
    }
    if (fname == "json.get") {
        llvm::Value* s = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* k = val_to_str(gen_expr(c->args[1].get()));
        llvm::Value* fn = decl_vor("vor_json_get", VType::Str, {VType::Str, VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {s, k})};
    }
    if (fname == "json.parse_str") {
        llvm::Value* s = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_json_parse_str", VType::Str, {VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {s})};
    }
    if (fname == "json.parse_int") {
        llvm::Value* s = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_json_parse_int", VType::Int, {VType::Str});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {s})};
    }
    if (fname == "json.parse_float") {
        llvm::Value* s = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_json_parse_float", VType::Float, {VType::Str});
        return {VType::Float, B->CreateCall(llvm::cast<llvm::Function>(fn), {s})};
    }
    if (fname == "json.parse_bool") {
        llvm::Value* s = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_json_parse_bool", VType::Int, {VType::Str});
        return {VType::Bool, B->CreateCall(llvm::cast<llvm::Function>(fn), {s})};
    }
    if (fname == "json.stringify_str") {
        llvm::Value* v = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_json_stringify_str", VType::Str, {VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {v})};
    }
    if (fname == "json.stringify_int") {
        llvm::Value* v = to_i64(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_json_stringify_int", VType::Str, {VType::Int});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {v})};
    }
    if (fname == "json.stringify_float") {
        llvm::Value* v = to_fp(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_json_stringify_float", VType::Str, {VType::Float});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {v})};
    }
    if (fname == "json.stringify_bool") {
        llvm::Value* v = to_i64(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_json_stringify_bool", VType::Str, {VType::Int});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {v})};
    }

    // ---- 扩展模块转发：base64.（encode / decode） ----
    if (fname == "base64.encode") {
        llvm::Value* s = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_base64_encode", VType::Str, {VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {s})};
    }
    if (fname == "base64.decode") {
        llvm::Value* s = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_base64_decode", VType::Str, {VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {s})};
    }

    // ---- 扩展模块转发：datetime.（ymd/to_iso/from_iso/today/add_days/days_between） ----
    if (fname == "datetime.ymd") {
        llvm::Value* y = to_i64(gen_expr(c->args[0].get()));
        llvm::Value* mo = to_i64(gen_expr(c->args[1].get()));
        llvm::Value* d = to_i64(gen_expr(c->args[2].get()));
        llvm::Value* fn = decl_vor("vor_datetime_ymd", VType::Str, {VType::Int, VType::Int, VType::Int});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {y, mo, d})};
    }
    if (fname == "datetime.to_iso") {
        llvm::Value* e = to_i64(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_datetime_to_iso", VType::Str, {VType::Int});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {e})};
    }
    if (fname == "datetime.from_iso") {
        llvm::Value* s = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_datetime_from_iso", VType::Int, {VType::Str});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {s})};
    }
    if (fname == "datetime.today") {
        llvm::Value* fn = decl_vor("vor_datetime_today", VType::Str, {});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {})};
    }
    if (fname == "datetime.add_days") {
        llvm::Value* s = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* n = to_i64(gen_expr(c->args[1].get()));
        llvm::Value* fn = decl_vor("vor_datetime_add_days", VType::Str, {VType::Str, VType::Int});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {s, n})};
    }
    if (fname == "datetime.days_between") {
        llvm::Value* a = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* b = val_to_str(gen_expr(c->args[1].get()));
        llvm::Value* fn = decl_vor("vor_datetime_days_between", VType::Int, {VType::Str, VType::Str});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {a, b})};
    }

    // 把任意 LVal 字符串化为 VStr*
    auto toStre = [&](LVal v) -> llvm::Value* {
        if (v.type == VType::Str) return v.val;
        if (v.type == VType::Float) {
            llvm::Value* f = decl_vor("vor_double_to_str", VType::Str, {VType::Float});
            return B->CreateCall(llvm::cast<llvm::Function>(f), {v.val});
        }
        if (v.type == VType::Bool) {
            llvm::Value* f = decl_vor("vor_bool_to_str", VType::Str, {VType::Int});
            return B->CreateCall(llvm::cast<llvm::Function>(f), {to_i64(v)});
        }
        llvm::Value* f = decl_vor("vor_i64_to_str", VType::Str, {VType::Int});
        return B->CreateCall(llvm::cast<llvm::Function>(f), {to_i64(v)});
    };

    // ---- 扩展模块转发：csv.（quote/count_fields/field_at/to_line） ----
    // sep：VStr*（可为空）→ 运行时取首字节，空则默认逗号
    llvm::PointerType* csSepPtr = llvm::cast<llvm::PointerType>(llvm_type(VType::Str));
    auto csSepOrNull = [&](size_t pos) -> llvm::Value* {
        if (c->args.size() > pos) return val_to_str(gen_expr(c->args[pos].get()));
        return llvm::ConstantPointerNull::get(csSepPtr);
    };
    if (fname == "csv.quote") {
        llvm::Value* fv = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_csv_quote", VType::Str, {VType::Str, VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {fv, csSepOrNull(1)})};
    }
    if (fname == "csv.count_fields") {
        llvm::Value* ln = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_csv_count_fields", VType::Int, {VType::Str, VType::Str});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {ln, csSepOrNull(1)})};
    }
    if (fname == "csv.field_at") {
        llvm::Value* ln = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* idx = to_i64(gen_expr(c->args[1].get()));
        llvm::Value* fn = decl_vor("vor_csv_field_at", VType::Str, {VType::Str, VType::Int, VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {ln, idx, csSepOrNull(2)})};
    }
    if (fname == "csv.row") {
        llvm::Value* ln = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_csv_parse_row", VType::StrList, {VType::Str, VType::Str});
        return {VType::StrList, B->CreateCall(llvm::cast<llvm::Function>(fn), {ln, csSepOrNull(1)})};
    }
    if (fname == "csv.to_line") {
        // vararg：对每个参数 quote 后以逗号连接，结果字符串
        llvm::Function* qf = llvm::cast<llvm::Function>(decl_vor("vor_csv_quote", VType::Str, {VType::Str, VType::Str}));
        llvm::Function* cc = llvm::cast<llvm::Function>(decl_vor("vor_str_concat", VType::Str, {VType::Str, VType::Str}));
        llvm::Function* cstr = llvm::cast<llvm::Function>(decl_vor("vor_str_from_cstr", VType::Str, {VType::Str}));
        llvm::Value* comma = B->CreateCall(cstr, {B->CreateGlobalString(",", "csvsep")});
        llvm::Value* res = nullptr;
        bool first = true;
        for (auto& a : c->args) {
            llvm::Value* part = B->CreateCall(qf, {toStre(gen_expr(a.get())), llvm::ConstantPointerNull::get(csSepPtr)});
            if (!first) res = B->CreateCall(cc, {res, comma});
            res = res ? B->CreateCall(cc, {res, part}) : part;
            first = false;
        }
        return {VType::Str, res};
    }
    // ---- 扩展模块转发：hash.（md5/sha1/sha256），输出小写十六进制 str ----
    if (fname == "hash.md5" || fname == "hash.sha1" || fname == "hash.sha256") {
        const char* rf = fname == "hash.md5" ? "vor_hash_md5"
                       : fname == "hash.sha1" ? "vor_hash_sha1" : "vor_hash_sha256";
        llvm::Value* s = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor(rf, VType::Str, {VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {s})};
    }

    // ---- 扩展模块转发：sys.（系统/运行时，纯标量返回） ----
    if (fname == "sys.version") {
        llvm::Value* fn = decl_vor("vor_sys_version", VType::Str, {});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {})};
    }
    if (fname == "sys.platform") {
        llvm::Value* p = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_os_platform", VType::Str, {})), {});
        return {VType::Str, p};
    }
    if (fname == "sys.time_ms") {
        llvm::Value* fn = decl_vor("vor_sys_time_ms", VType::Int, {});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {})};
    }
    if (fname == "sys.clock") {
        llvm::Value* fn = decl_vor("vor_sys_clock", VType::Float, {});
        return {VType::Float, B->CreateCall(llvm::cast<llvm::Function>(fn), {})};
    }
    if (fname == "sys.sleep") {
        llvm::Value* fn = decl_vor("vor_sys_sleep", VType::Int, {VType::Int});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {to_i64(gen_expr(c->args[0].get()))})};
    }
    if (fname == "sys.exit") {
        llvm::Value* code = c->args.empty() ? B->getInt64(0) : to_i64(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_sys_exit", VType::Void, {VType::Int});
        B->CreateCall(llvm::cast<llvm::Function>(fn), {code});
        return nothing();
    }

    // ---- 扩展模块转发：str.（字符串工具，纯标量/字符串返回） ----
    {
        auto S1 = [&]() { return val_to_str(gen_expr(c->args[0].get())); };
        auto S2 = [&]() { return val_to_str(gen_expr(c->args[1].get())); };
        auto I1 = [&]() { return to_i64(gen_expr(c->args[1].get())); };
        if (fname == "text.upper") {
            llvm::Value* fn = decl_vor("vor_str_upper", VType::Str, {VType::Str});
            return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {S1()})};
        }
        if (fname == "text.lower") {
            llvm::Value* fn = decl_vor("vor_str_lower", VType::Str, {VType::Str});
            return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {S1()})};
        }
        if (fname == "text.title") {
            llvm::Value* fn = decl_vor("vor_str_title", VType::Str, {VType::Str});
            return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {S1()})};
        }
        if (fname == "text.swapcase") {
            llvm::Value* fn = decl_vor("vor_str_swapcase", VType::Str, {VType::Str});
            return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {S1()})};
        }
        if (fname == "text.capitalize") {
            llvm::Value* fn = decl_vor("vor_str_capitalize", VType::Str, {VType::Str});
            return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {S1()})};
        }
        auto TRIM = [&](const char* rf) -> LVal {
            llvm::Value* fn = decl_vor(rf, VType::Str, {VType::Str});
            return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {S1()})};
        };
        if (fname == "text.trim")  return TRIM("vor_str_trim");
        if (fname == "text.ltrim") return TRIM("vor_str_ltrim");
        if (fname == "text.rtrim") return TRIM("vor_str_rtrim");
        if (fname == "text.strip")  return TRIM("vor_str_trim");
        if (fname == "text.lstrip") return TRIM("vor_str_ltrim");
        if (fname == "text.rstrip") return TRIM("vor_str_rtrim");
        if (fname == "text.len") {
            llvm::Value* fn = decl_vor("vor_str_len", VType::Int, {VType::Str});
            return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {S1()})};
        }
        auto INT2 = [&](const char* rf) -> LVal {
            llvm::Value* fn = decl_vor(rf, VType::Int, {VType::Str, VType::Str});
            return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {S1(), S2()})};
        };
        if (fname == "text.contains")    return INT2("vor_str_contains");
        if (fname == "text.starts_with") return INT2("vor_str_starts_with");
        if (fname == "text.ends_with")   return INT2("vor_str_ends_with");
        if (fname == "text.startswith")  return INT2("vor_str_starts_with");
        if (fname == "text.endswith")    return INT2("vor_str_ends_with");
        if (fname == "text.removeprefix") {
            llvm::Value* fn = decl_vor("vor_str_removeprefix", VType::Str, {VType::Str, VType::Str});
            return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {S1(), S2()})};
        }
        if (fname == "text.removesuffix") {
            llvm::Value* fn = decl_vor("vor_str_removesuffix", VType::Str, {VType::Str, VType::Str});
            return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {S1(), S2()})};
        }
        if (fname == "text.find")        return INT2("vor_str_find");
        if (fname == "text.rfind")       return INT2("vor_str_rfind");
        if (fname == "text.count")       return INT2("vor_str_count");
        auto PRED = [&](const char* rf) -> LVal {
            llvm::Value* fn = decl_vor(rf, VType::Int, {VType::Str});
            return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {S1()})};
        };
        if (fname == "text.isalpha") return PRED("vor_str_isalpha");
        if (fname == "text.isdigit") return PRED("vor_str_isdigit");
        if (fname == "text.isalnum") return PRED("vor_str_isalnum");
        if (fname == "text.isspace") return PRED("vor_str_isspace");
        if (fname == "text.isupper") return PRED("vor_str_isupper");
        if (fname == "text.islower") return PRED("vor_str_islower");
        if (fname == "text.replace") {
            llvm::Value* b = val_to_str(gen_expr(c->args[2].get()));
            llvm::Value* fn = decl_vor("vor_str_replace", VType::Str, {VType::Str, VType::Str, VType::Str});
            return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {S1(), S2(), b})};
        }
        if (fname == "text.slice") {
            llvm::Value* en = c->args.size() >= 3 ? to_i64(gen_expr(c->args[2].get())) : B->getInt64(-1);
            llvm::Value* fn = decl_vor("vor_str_slice", VType::Str, {VType::Str, VType::Int, VType::Int});
            return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {S1(), I1(), en})};
        }
        if (fname == "text.char_at") {
            llvm::Value* fn = decl_vor("vor_str_char_at", VType::Int, {VType::Str, VType::Int});
            return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {S1(), I1()})};
        }
        if (fname == "text.ord") {
            llvm::Value* fn = decl_vor("vor_str_ord", VType::Int, {VType::Str});
            return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {S1()})};
        }
        if (fname == "text.chr") {
            llvm::Value* fn = decl_vor("vor_str_chr", VType::Str, {VType::Int});
            return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn),
                {to_i64(gen_expr(c->args[0].get()))})};
        }
        if (fname == "text.zfill") {
            llvm::Value* fn = decl_vor("vor_str_zfill", VType::Str, {VType::Str, VType::Int});
            return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {S1(), I1()})};
        }
        if (fname == "text.ljust" || fname == "text.rjust") {
            const char* rf = fname == "text.ljust" ? "vor_str_pad_right" : "vor_str_pad_left";
            llvm::Function* fb = llvm::cast<llvm::Function>(decl_vor("vor_str_first_byte", VType::Int, {VType::Str, VType::Int}));
            llvm::Value* pad;
            if (c->args.size() >= 3)
                pad = B->CreateCall(fb, {val_to_str(gen_expr(c->args[2].get())), B->getInt64(' ')});
            else
                pad = B->getInt64(' ');
            llvm::Value* fn = decl_vor(rf, VType::Str, {VType::Str, VType::Int, VType::Int});
            return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {S1(), I1(), pad})};
        }
        if (fname == "text.center") {
            llvm::Function* fb = llvm::cast<llvm::Function>(decl_vor("vor_str_first_byte", VType::Int, {VType::Str, VType::Int}));
            llvm::Value* pad;
            if (c->args.size() >= 3)
                pad = B->CreateCall(fb, {val_to_str(gen_expr(c->args[2].get())), B->getInt64(' ')});
            else
                pad = B->getInt64(' ');
            llvm::Value* fn = decl_vor("vor_str_center", VType::Str, {VType::Str, VType::Int, VType::Int});
            return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {S1(), I1(), pad})};
        }
        if (fname == "text.repeat") {
            llvm::Value* fn = decl_vor("vor_str_repeat", VType::Str, {VType::Str, VType::Int});
            return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {S1(), I1()})};
        }
        if (fname == "text.pad_left" || fname == "text.pad_right") {
            const char* rf = fname == "text.pad_left" ? "vor_str_pad_left" : "vor_str_pad_right";
            llvm::Function* fb = llvm::cast<llvm::Function>(decl_vor("vor_str_first_byte", VType::Int, {VType::Str, VType::Int}));
            llvm::Value* pad;
            if (c->args.size() >= 3)
                pad = B->CreateCall(fb, {val_to_str(gen_expr(c->args[2].get())), B->getInt64(' ')});
            else
                pad = B->getInt64(' ');
            llvm::Value* fn = decl_vor(rf, VType::Str, {VType::Str, VType::Int, VType::Int});
            return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {S1(), I1(), pad})};
        }
        if (fname == "text.format") {
            // vararg：把参数指针收集进栈数组，交给 vor_str_format 依次替换 "{}"
            llvm::Value* fmt = S1();
            const size_t n = c->args.size() - 1;
            const size_t arrSize = n > 0 ? n : 1;
            llvm::Type* strPtr = llvm_type(VType::Str);
            llvm::ArrayType* arrTy = llvm::ArrayType::get(strPtr, arrSize);
            llvm::AllocaInst* arr = B->CreateAlloca(arrTy, nullptr, "fmta");
            for (size_t i = 0; i < n; ++i) {
                llvm::Value* arg = val_to_str(gen_expr(c->args[i + 1].get()));
                llvm::Value* idx[2] = { B->getInt64(0), B->getInt64((unsigned long long)i) };
                llvm::Value* slot = B->CreateInBoundsGEP(arrTy, arr, idx, "fmtslot");
                B->CreateStore(arg, slot);
            }
            llvm::Function* fn = declare_runtime("vor_str_format", strPtr,
                {strPtr, strPtr->getPointerTo(), B->getInt64Ty()});
            llvm::Value* r = B->CreateCall(fn, {fmt, arr, B->getInt64((unsigned long long)n)});
            return {VType::Str, r};
        }
        if (fname == "text.split") {
            llvm::Value* fn = decl_vor("vor_str_split", VType::StrList, {VType::Str, VType::Str});
            return {VType::StrList, B->CreateCall(llvm::cast<llvm::Function>(fn), {S1(), S2()})};
        }
        if (fname == "text.join") {
            // vararg：arg0 为分隔符，其余作为待拼接部分
            llvm::Value* sep = S1();
            const size_t n = c->args.size() - 1;
            const size_t arrSize = n > 0 ? n : 1;
            llvm::Type* strPtr = llvm_type(VType::Str);
            llvm::ArrayType* arrTy = llvm::ArrayType::get(strPtr, arrSize);
            llvm::AllocaInst* arr = B->CreateAlloca(arrTy, nullptr, "jparts");
            for (size_t i = 0; i < n; ++i) {
                llvm::Value* arg = val_to_str(gen_expr(c->args[i + 1].get()));
                llvm::Value* idx[2] = { B->getInt64(0), B->getInt64((unsigned long long)i) };
                llvm::Value* slot = B->CreateInBoundsGEP(arrTy, arr, idx, "jslot");
                B->CreateStore(arg, slot);
            }
            llvm::Function* fn = declare_runtime("vor_str_join", strPtr,
                {strPtr, strPtr->getPointerTo(), B->getInt64Ty()});
            llvm::Value* r = B->CreateCall(fn, {sep, arr, B->getInt64((unsigned long long)n)});
            return {VType::Str, r};
        }
    }
    // ---- 扩展模块转发：net.（url_encode/decode + http_get/post） ----
    if (fname == "net.url_encode") {
        llvm::Value* s = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_net_url_encode", VType::Str, {VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {s})};
    }
    if (fname == "net.url_decode") {
        llvm::Value* s = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_net_url_decode", VType::Str, {VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {s})};
    }
    if (fname == "net.http_get") {
        llvm::Value* s = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* fn = decl_vor("vor_net_http_get", VType::Str, {VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {s})};
    }
    if (fname == "net.http_post") {
        llvm::Value* s = val_to_str(gen_expr(c->args[0].get()));
        llvm::Value* b = val_to_str(gen_expr(c->args[1].get()));
        llvm::Value* fn = decl_vor("vor_net_http_post", VType::Str, {VType::Str, VType::Str});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {s, b})};
    }
    // log 输出函数（debug/info/warn/error/fatal）：vararg 空格拼接后记录
    auto log_emit = [&](int lv) {
        llvm::Value* msg = nullptr;
        llvm::Value* space = nullptr;
        bool first = true;
        bool any = false;
        for (auto& a : c->args) {
            if (first) first = false;
            else {
                if (!space)
                    space = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_str_from_cstr", VType::Str, {VType::Str})),
                                          {B->CreateGlobalString(" ", "lsep")});
                llvm::Value* c1 = decl_vor("vor_str_concat", VType::Str, {VType::Str, VType::Str});
                msg = B->CreateCall(llvm::cast<llvm::Function>(c1), {msg, space});
            }
            any = true;
            llvm::Value* part = toStre(gen_expr(a.get()));
            if (msg)
                msg = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_str_concat", VType::Str, {VType::Str, VType::Str})), {msg, part});
            else
                msg = part;
        }
        if (!any)
            msg = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_str_from_cstr", VType::Str, {VType::Str})), {B->CreateGlobalString("", "lempty")});
        B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_log_emit", VType::Void, {VType::Int, VType::Str})), {B->getInt64(lv), msg});
        return nothing();
    };
    if (fname == "log.debug") return log_emit(10);
    if (fname == "log.info")  return log_emit(20);
    if (fname == "log.warn")  return log_emit(30);
    if (fname == "log.error") return log_emit(40);
    if (fname == "log.fatal") return log_emit(50);

    // ---- 扩展模块转发批B：time tuple / random list 交互 ----
    if (fname == "time.gmtime" || fname == "time.localtime") {
        LVal ts = c->args.empty() ? LVal{} : gen_expr(c->args[0].get());
        llvm::Value* tnow = ts.val ? to_fp(ts) : B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_time_now", VType::Float, {})), {});
        const char* nm = fname == "time.gmtime" ? "vor_time_gmtime" : "vor_time_localtime";
        llvm::Value* fn = decl_vor(nm, VType::Tuple, {VType::Float});
        return {VType::Tuple, B->CreateCall(llvm::cast<llvm::Function>(fn), {tnow})};
    }
    if (fname == "time.mktime") {
        LVal a0 = gen_expr(c->args[0].get());
        llvm::Value* fn = decl_vor("vor_time_mktime", VType::Float, {VType::Tuple});
        return {VType::Float, B->CreateCall(llvm::cast<llvm::Function>(fn), {a0.val})};
    }
    if (fname == "time.strftime") {
        LVal a0 = gen_expr(c->args[0].get()); LVal a1 = gen_expr(c->args[1].get());
        llvm::Value* fn = decl_vor("vor_time_strftime", VType::Str, {VType::Str, VType::Tuple});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {a0.val, a1.val})};
    }
    if (fname == "random.choice") {
        LVal a0 = gen_expr(c->args[0].get());
        llvm::Value* fn = decl_vor("vor_rng_choice", VType::Int, {VType::List});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {a0.val})};
    }
    if (fname == "random.choices") {
        LVal pop = gen_expr(c->args[0].get());
        llvm::Value* w = (c->args.size() >= 2)
            ? gen_expr(c->args[1].get()).val
            : llvm::Constant::getNullValue(B->getPtrTy());
        llvm::Value* k = c->args.size() >= 3 ? to_i64(gen_expr(c->args[2].get())) : B->getInt64(1);
        llvm::Value* fn = decl_vor("vor_rng_choices", VType::List, {VType::List, VType::List, VType::Int});
        return {VType::List, B->CreateCall(llvm::cast<llvm::Function>(fn), {pop.val, w, k})};
    }
    if (fname == "random.shuffle") {
        LVal a0 = gen_expr(c->args[0].get());
        llvm::Value* fn = decl_vor("vor_rng_shuffle", VType::Void, {VType::List});
        B->CreateCall(llvm::cast<llvm::Function>(fn), {a0.val});
        return nothing();
    }
    if (fname == "random.sample") {
        LVal a0 = gen_expr(c->args[0].get()); LVal a1 = gen_expr(c->args[1].get());
        llvm::Value* fn = decl_vor("vor_rng_sample", VType::List, {VType::List, VType::Int});
        return {VType::List, B->CreateCall(llvm::cast<llvm::Function>(fn), {a0.val, to_i64(a1)})};
    }

    // ---- 扩展模块转发：thread.（P4） ----
    // 不透明句柄统一复用 Str(指针) 槽传递。
    if (fname == "thread.run") {
        // arg0 必须是已知用户函数名（静态分派，函数编译为原生 i64(void)）
        auto* id = dynamic_cast<const IdentifierExpr*>(c->args[0].get());
        llvm::Function* ufn = id ? mod_->getFunction(id->name) : nullptr;
        if (!ufn) return {VType::Void, nullptr};
        llvm::FunctionType* vft = llvm::FunctionType::get(B->getVoidTy(), {}, false);
        llvm::Value* fn = declare_runtime("vor_thread_run", B->getPtrTy(), {vft->getPointerTo()});
        llvm::Value* fp = B->CreateBitCast(ufn, vft->getPointerTo());
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {fp})};
    }
    if (fname == "thread.join") {
        LVal a0 = gen_expr(c->args[0].get());
        B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_thread_join", VType::Void, {VType::Str})), {a0.val});
        return {VType::Void, nullptr};
    }
    if (fname == "thread.yield") {
        B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_thread_yield", VType::Void, {})), {});
        return {VType::Void, nullptr};
    }
    if (fname == "thread.sleep") {
        LVal a0 = gen_expr(c->args[0].get());
        B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_thread_sleep", VType::Void, {VType::Int})), {to_i64(a0)});
        return {VType::Void, nullptr};
    }
    if (fname == "thread.hardware") {
        llvm::Value* fn = decl_vor("vor_thread_hardware", VType::Int, {});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {})};
    }
    if (fname == "thread.mutex") {
        llvm::Value* fn = decl_vor("vor_thread_mutex", VType::Str, {});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {})};
    }
    if (fname == "thread.lock") {
        LVal a0 = gen_expr(c->args[0].get());
        B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_thread_lock", VType::Void, {VType::Str})), {a0.val});
        return {VType::Void, nullptr};
    }
    if (fname == "thread.unlock") {
        LVal a0 = gen_expr(c->args[0].get());
        B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_thread_unlock", VType::Void, {VType::Str})), {a0.val});
        return {VType::Void, nullptr};
    }
    if (fname == "thread.trylock") {
        LVal a0 = gen_expr(c->args[0].get());
        llvm::Value* fn = decl_vor("vor_thread_trylock", VType::Int, {VType::Str});
        llvm::Value* r = B->CreateCall(llvm::cast<llvm::Function>(fn), {a0.val});
        return {VType::Bool, B->CreateICmpNE(r, B->getInt64(0))};
    }
    if (fname == "thread.atomic") {
        LVal a0 = gen_expr(c->args[0].get());
        llvm::Value* fn = decl_vor("vor_thread_atomic", VType::Str, {VType::Int});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {to_i64(a0)})};
    }
    if (fname == "thread.atomic_get") {
        LVal a0 = gen_expr(c->args[0].get());
        llvm::Value* fn = decl_vor("vor_thread_atomic_get", VType::Int, {VType::Str});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {a0.val})};
    }
    if (fname == "thread.atomic_set") {
        LVal a0 = gen_expr(c->args[0].get()); LVal a1 = gen_expr(c->args[1].get());
        B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_thread_atomic_set", VType::Void, {VType::Str, VType::Int})), {a0.val, to_i64(a1)});
        return {VType::Void, nullptr};
    }
    if (fname == "thread.atomic_add") {
        LVal a0 = gen_expr(c->args[0].get()); LVal a1 = gen_expr(c->args[1].get());
        llvm::Value* fn = decl_vor("vor_thread_atomic_add", VType::Int, {VType::Str, VType::Int});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {a0.val, to_i64(a1)})};
    }
    // 通道（int 负载子集，P4）：句柄复用 Str(指针) 槽
    if (fname == "thread.channel") {
        LVal a0 = c->args.empty() ? LVal{} : gen_expr(c->args[0].get());
        llvm::Value* fn = decl_vor("vor_thread_channel", VType::Str, {VType::Int});
        llvm::Value* cap = c->args.empty() ? B->getInt64(0) : to_i64(a0);
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {cap})};
    }
    if (fname == "thread.send") {
        LVal a0 = gen_expr(c->args[0].get()); LVal a1 = gen_expr(c->args[1].get());
        B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_thread_channel_send", VType::Void, {VType::Str, VType::Int})), {a0.val, to_i64(a1)});
        return {VType::Void, nullptr};
    }
    if (fname == "thread.recv") {
        LVal a0 = gen_expr(c->args[0].get());
        llvm::Value* fn = decl_vor("vor_thread_channel_recv", VType::Int, {VType::Str});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {a0.val})};
    }
    if (fname == "thread.close") {
        LVal a0 = gen_expr(c->args[0].get());
        B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_thread_channel_close", VType::Void, {VType::Str})), {a0.val});
        return {VType::Void, nullptr};
    }
    if (fname == "thread.channel_len" || fname == "thread.len") {
        LVal a0 = gen_expr(c->args[0].get());
        llvm::Value* fn = decl_vor("vor_thread_channel_len", VType::Int, {VType::Str});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {a0.val})};
    }
    // 线程池（P4）：submit 使用静态函数指针（同 thread.run）
    if (fname == "thread.pool") {
        LVal a0 = gen_expr(c->args[0].get());
        llvm::Value* fn = decl_vor("vor_thread_pool", VType::Str, {VType::Int});
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {to_i64(a0)})};
    }
    if (fname == "thread.pool_submit") {
        LVal a0 = gen_expr(c->args[0].get());
        auto* id = dynamic_cast<const IdentifierExpr*>(c->args[1].get());
        llvm::Function* ufn = id ? mod_->getFunction(id->name) : nullptr;
        if (!ufn) return {VType::Void, nullptr};
        llvm::FunctionType* vft = llvm::FunctionType::get(B->getVoidTy(), {}, false);
        llvm::Value* fn = declare_runtime("vor_thread_pool_submit", B->getVoidTy(), {B->getPtrTy(), vft->getPointerTo()});
        llvm::Value* fp = B->CreateBitCast(ufn, vft->getPointerTo());
        B->CreateCall(llvm::cast<llvm::Function>(fn), {a0.val, fp});
        return {VType::Void, nullptr};
    }
    if (fname == "thread.pool_size") {
        LVal a0 = gen_expr(c->args[0].get());
        llvm::Value* fn = decl_vor("vor_thread_pool_size", VType::Int, {VType::Str});
        return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {a0.val})};
    }
    if (fname == "thread.pool_shutdown") {
        LVal a0 = gen_expr(c->args[0].get());
        B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_thread_pool_shutdown", VType::Void, {VType::Str})), {a0.val});
        return {VType::Void, nullptr};
    }

    // ============================================================
    // 扩展模块转发：game2d. / render3d.（P4 含绘制层）
    // 对象以不透明 void* 句柄（VType::Str=ptr 槽）传递；标量走原生；
    // 字符串参数先取 cstr；color 列表分量按 to_i64 的 bitcast 约定还原。
    // ============================================================
    auto F = [&](const char* n, VType r, std::vector<VType> p) {
        return llvm::cast<llvm::Function>(decl_vor(n, r, std::move(p)));
    };
    auto as_cstr = [&](LVal v) -> llvm::Value* {
        return B->CreateCall(F("vor_str_cstr", VType::Str, {VType::Str}), {v.val});
    };
    auto vec3opt = [&](size_t idx, double dr, double dg, double db) -> std::array<llvm::Value*, 4> {
        if (idx < c->args.size()) {
            LVal lst = gen_expr(c->args[idx].get());
            llvm::Function* gi = F("vor_list_get", VType::Int, {VType::List, VType::Int});
            llvm::Value* e0 = B->CreateCall(gi, {lst.val, B->getInt64(0)});
            llvm::Value* e1 = B->CreateCall(gi, {lst.val, B->getInt64(1)});
            llvm::Value* e2 = B->CreateCall(gi, {lst.val, B->getInt64(2)});
            auto bf = [&](llvm::Value* e) { return B->CreateBitCast(e, B->getDoubleTy()); };
            return {B->getInt64(1), bf(e0), bf(e1), bf(e2)};
        }
        return {B->getInt64(0), llvm::ConstantFP::get(B->getDoubleTy(), dr),
                llvm::ConstantFP::get(B->getDoubleTy(), dg), llvm::ConstantFP::get(B->getDoubleTy(), db)};
    };

    // ---- game2d：图像 ----
    if (fname == "game2d.new_image") {
        llvm::Value* r = B->CreateCall(F("vor_g2d_new_image", VType::Str, {VType::Int, VType::Int}),
                                       {to_i64(gen_expr(c->args[0].get())), to_i64(gen_expr(c->args[1].get()))});
        return {VType::Str, r};
    }
    if (fname == "game2d.checker_image") {
        llvm::Value* w = to_i64(gen_expr(c->args[0].get()));
        llvm::Value* h = to_i64(gen_expr(c->args[1].get()));
        llvm::Value* t = c->args.size() >= 3 ? to_i64(gen_expr(c->args[2].get())) : B->getInt64(32);
        return {VType::Str, B->CreateCall(F("vor_g2d_checker_image", VType::Str, {VType::Int, VType::Int, VType::Int}), {w, h, t})};
    }
    if (fname == "game2d.solid_image") {
        llvm::Value* w = to_i64(gen_expr(c->args[0].get()));
        llvm::Value* h = to_i64(gen_expr(c->args[1].get()));
        llvm::Value* r = to_i64(gen_expr(c->args[2].get()));
        llvm::Value* g = to_i64(gen_expr(c->args[3].get()));
        llvm::Value* b = to_i64(gen_expr(c->args[4].get()));
        return {VType::Str, B->CreateCall(F("vor_g2d_solid_image", VType::Str, {VType::Int, VType::Int, VType::Int, VType::Int, VType::Int}), {w, h, r, g, b})};
    }
    if (fname == "game2d.image_size") {
        LVal im = gen_expr(c->args[0].get());
        return {VType::Pair, B->CreateCall(F("vor_g2d_image_size_pair", VType::Pair, {VType::Str}), {im.val})};
    }
    if (fname == "game2d.image_w") {
        LVal im = gen_expr(c->args[0].get());
        return {VType::Int, B->CreateCall(F("vor_g2d_image_w", VType::Int, {VType::Str}), {im.val})};
    }
    if (fname == "game2d.image_h") {
        LVal im = gen_expr(c->args[0].get());
        return {VType::Int, B->CreateCall(F("vor_g2d_image_h", VType::Int, {VType::Str}), {im.val})};
    }

    // ---- game2d：精灵 ----
    if (fname == "game2d.new_sprite") {
        LVal im = gen_expr(c->args[0].get());
        llvm::Value* x = c->args.size() >= 2 ? to_fp(gen_expr(c->args[1].get())) : llvm::ConstantFP::get(B->getDoubleTy(), 0.0);
        llvm::Value* y = c->args.size() >= 3 ? to_fp(gen_expr(c->args[2].get())) : llvm::ConstantFP::get(B->getDoubleTy(), 0.0);
        return {VType::Str, B->CreateCall(F("vor_g2d_new_sprite", VType::Str, {VType::Str, VType::Float, VType::Float}), {im.val, x, y})};
    }
    if (fname == "game2d.sprite_move") {
        LVal sp = gen_expr(c->args[0].get());
        llvm::Value* dx = to_fp(gen_expr(c->args[1].get()));
        llvm::Value* dy = to_fp(gen_expr(c->args[2].get()));
        B->CreateCall(F("vor_g2d_sprite_move", VType::Void, {VType::Str, VType::Float, VType::Float}), {sp.val, dx, dy});
        return nothing();
    }
    if (fname == "game2d.sprite_pos") {
        LVal sp = gen_expr(c->args[0].get());
        if (c->args.size() >= 3) {
            llvm::Value* x = to_fp(gen_expr(c->args[1].get()));
            llvm::Value* y = to_fp(gen_expr(c->args[2].get()));
            B->CreateCall(F("vor_g2d_sprite_setpos", VType::Void, {VType::Str, VType::Float, VType::Float}), {sp.val, x, y});
            return nothing();
        }
        llvm::Value* px = B->CreateCall(F("vor_g2d_sprite_posx", VType::Float, {VType::Str}), {sp.val});
        llvm::Value* py = B->CreateCall(F("vor_g2d_sprite_posy", VType::Float, {VType::Str}), {sp.val});
        llvm::Function* pn = F("vor_pair_new", VType::Pair, {VType::Int, VType::Int});
        return {VType::Pair, B->CreateCall(pn, {B->CreateBitCast(px, B->getInt64Ty()), B->CreateBitCast(py, B->getInt64Ty())})};
    }
    if (fname == "game2d.sprite_angle" || fname == "game2d.sprite_scale") {
        LVal sp = gen_expr(c->args[0].get());
        if (c->args.size() >= 2) {
            llvm::Value* a = to_fp(gen_expr(c->args[1].get()));
            const char* setf = fname == "game2d.sprite_angle" ? "vor_g2d_sprite_setangle" : "vor_g2d_sprite_setscale";
            B->CreateCall(F(setf, VType::Void, {VType::Str, VType::Float}), {sp.val, a});
            return nothing();
        }
        const char* getf = fname == "game2d.sprite_angle" ? "vor_g2d_sprite_angle" : "vor_g2d_sprite_scale";
        return {VType::Float, B->CreateCall(F(getf, VType::Float, {VType::Str}), {sp.val})};
    }
    if (fname == "game2d.sprite_visible") {
        LVal sp = gen_expr(c->args[0].get());
        if (c->args.size() >= 2) {
            llvm::Value* v = to_i64(gen_expr(c->args[1].get()));
            B->CreateCall(F("vor_g2d_sprite_setvisible", VType::Void, {VType::Str, VType::Int}), {sp.val, v});
            return nothing();
        }
        llvm::Value* b = B->CreateCall(F("vor_g2d_sprite_visible", VType::Int, {VType::Str}), {sp.val});
        return {VType::Bool, B->CreateICmpNE(b, B->getInt64(0))};
    }
    if (fname == "game2d.collides") {
        LVal a = gen_expr(c->args[0].get());
        LVal b = gen_expr(c->args[1].get());
        llvm::Value* r = B->CreateCall(F("vor_g2d_sprite_collides", VType::Int, {VType::Str, VType::Str}), {a.val, b.val});
        return {VType::Bool, B->CreateICmpNE(r, B->getInt64(0))};
    }
    if (fname == "game2d.sprite_contains_point") {
        LVal sp = gen_expr(c->args[0].get());
        llvm::Value* px = to_fp(gen_expr(c->args[1].get()));
        llvm::Value* py = to_fp(gen_expr(c->args[2].get()));
        llvm::Value* r = B->CreateCall(F("vor_g2d_sprite_contains", VType::Int, {VType::Str, VType::Float, VType::Float}), {sp.val, px, py});
        return {VType::Bool, B->CreateICmpNE(r, B->getInt64(0))};
    }

    // ---- game2d：窗口 / 状态 ----
    if (fname == "game2d.set_window") {
        llvm::Value* w = to_i64(gen_expr(c->args[0].get()));
        llvm::Value* h = to_i64(gen_expr(c->args[1].get()));
        llvm::Value* t = c->args.size() >= 3 ? as_cstr(gen_expr(c->args[2].get())) : B->CreateGlobalString("Vortex Game2D", "t");
        B->CreateCall(F("vor_g2d_set_window", VType::Void, {VType::Int, VType::Int, VType::Str}), {w, h, t});
        return nothing();
    }
    if (fname == "game2d.set_title") {
        B->CreateCall(F("vor_g2d_set_title", VType::Void, {VType::Str}), {as_cstr(gen_expr(c->args[0].get()))});
        return nothing();
    }
    if (fname == "game2d.width") return {VType::Int, B->CreateCall(F("vor_g2d_width", VType::Int, {}), {})};
    if (fname == "game2d.height") return {VType::Int, B->CreateCall(F("vor_g2d_height", VType::Int, {}), {})};
    if (fname == "game2d.time") return {VType::Float, B->CreateCall(F("vor_g2d_time", VType::Float, {}), {})};
    if (fname == "game2d.dt") return {VType::Float, B->CreateCall(F("vor_g2d_dt", VType::Float, {}), {})};
    if (fname == "game2d.running") {
        llvm::Value* r = B->CreateCall(F("vor_g2d_running", VType::Int, {}), {});
        return {VType::Bool, B->CreateICmpNE(r, B->getInt64(0))};
    }
    if (fname == "game2d.set_fps") {
        B->CreateCall(F("vor_g2d_set_fps", VType::Void, {VType::Int}), {to_i64(gen_expr(c->args[0].get()))});
        return nothing();
    }
    if (fname == "game2d.quit") { B->CreateCall(F("vor_g2d_quit", VType::Void, {}), {}); return nothing(); }

    // ---- game2d：绘制 ----
    if (fname == "game2d.clear") {
        auto rc = [&](size_t i, long long d) { return c->args.size() >= i + 1 ? to_i64(gen_expr(c->args[i].get())) : B->getInt64(d); };
        B->CreateCall(F("vor_g2d_clear", VType::Void, {VType::Int, VType::Int, VType::Int}), {rc(0,0), rc(1,0), rc(2,0)});
        return nothing();
    }
    if (fname == "game2d.draw_rect") {
        std::vector<llvm::Value*> av(9);
        for (int i = 0; i < 4; ++i) av[i] = to_i64(gen_expr(c->args[i].get()));
        auto dcol = [&](size_t i, long long d) { return c->args.size() >= i + 1 ? to_i64(gen_expr(c->args[i].get())) : B->getInt64(d); };
        av[4]=dcol(4,255); av[5]=dcol(5,255); av[6]=dcol(6,255);
        av[7]=B->getInt64(255);
        av[8]= c->args.size() >= 8 ? to_i64(gen_expr(c->args[7].get())) : B->getInt64(1);
        B->CreateCall(F("vor_g2d_draw_rect", VType::Void, {VType::Int, VType::Int, VType::Int, VType::Int, VType::Int, VType::Int, VType::Int, VType::Int, VType::Int}), {av[0],av[1],av[2],av[3],av[4],av[5],av[6],av[7],av[8]});
        return nothing();
    }
    if (fname == "game2d.draw_circle" || fname == "game2d.draw_line") {
        int n = fname == "game2d.draw_circle" ? 3 : 4;
        std::vector<llvm::Value*> av(n);
        for (int i = 0; i < n; ++i) av[i] = to_i64(gen_expr(c->args[i].get()));
        auto dcol = [&](size_t i, long long d) { return c->args.size() >= i + 1 ? to_i64(gen_expr(c->args[i].get())) : B->getInt64(d); };
        std::vector<llvm::Value*> args9(9);
        for (int i = 0; i < n; ++i) args9[i] = av[i];
        args9[4]=dcol(4,255); args9[5]=dcol(5,255); args9[6]=dcol(6,255); args9[7]=B->getInt64(255);
        if (fname == "game2d.draw_circle") {
            args9[8]= c->args.size() >= 7 ? to_i64(gen_expr(c->args[6].get())) : B->getInt64(1);
            B->CreateCall(F("vor_g2d_draw_circle", VType::Void, {VType::Int,VType::Int,VType::Int,VType::Int,VType::Int,VType::Int,VType::Int,VType::Int,VType::Int}), {args9[0],args9[1],args9[2],args9[3],args9[4],args9[5],args9[6],args9[7],args9[8]});
        } else {
            llvm::Value* th = c->args.size() >= 8 ? to_i64(gen_expr(c->args[7].get())) : B->getInt64(1);
            B->CreateCall(F("vor_g2d_draw_line", VType::Void, {VType::Int,VType::Int,VType::Int,VType::Int,VType::Int,VType::Int,VType::Int,VType::Int,VType::Int}), {args9[0],args9[1],args9[2],args9[3],args9[4],args9[5],args9[6],args9[7],th});
        }
        return nothing();
    }
    if (fname == "game2d.draw_text") {
        llvm::Value* txt = as_cstr(gen_expr(c->args[0].get()));
        llvm::Value* x = to_i64(gen_expr(c->args[1].get()));
        llvm::Value* y = to_i64(gen_expr(c->args[2].get()));
        auto dcol = [&](size_t i, long long d) { return c->args.size() >= i + 1 ? to_i64(gen_expr(c->args[i].get())) : B->getInt64(d); };
        llvm::Value* sz = c->args.size() >= 7 ? to_i64(gen_expr(c->args[6].get())) : B->getInt64(16);
        B->CreateCall(F("vor_g2d_draw_text", VType::Void, {VType::Str,VType::Int,VType::Int,VType::Int,VType::Int,VType::Int,VType::Int,VType::Int}),
                      {txt, x, y, dcol(3,255), dcol(4,255), dcol(5,255), B->getInt64(255), sz});
        return nothing();
    }
    if (fname == "game2d.draw_image") {
        LVal im = gen_expr(c->args[0].get());
        llvm::Value* x = to_i64(gen_expr(c->args[1].get()));
        llvm::Value* y = to_i64(gen_expr(c->args[2].get()));
        auto df = [&](size_t i, double d) { return c->args.size() >= i + 1 ? to_fp(gen_expr(c->args[i].get())) : llvm::ConstantFP::get(B->getDoubleTy(), d); };
        B->CreateCall(F("vor_g2d_draw_image", VType::Void, {VType::Str,VType::Int,VType::Int,VType::Float,VType::Float,VType::Float}),
                      {im.val, x, y, df(3,0.0), df(4,1.0), df(5,1.0)});
        return nothing();
    }
    if (fname == "game2d.draw_sprite") {
        B->CreateCall(F("vor_g2d_draw_sprite", VType::Void, {VType::Str}), {gen_expr(c->args[0].get()).val});
        return nothing();
    }
    if (fname == "game2d.draw_all_sprites") { B->CreateCall(F("vor_g2d_draw_all_sprites", VType::Void, {}), {}); return nothing(); }
    if (fname == "game2d.key_down") {
        llvm::Value* r = B->CreateCall(F("vor_g2d_key_down", VType::Int, {VType::Str}), {as_cstr(gen_expr(c->args[0].get()))});
        return {VType::Bool, B->CreateICmpNE(r, B->getInt64(0))};
    }
    if (fname == "game2d.set_key") {
        B->CreateCall(F("vor_g2d_set_key", VType::Void, {VType::Str, VType::Int}), {as_cstr(gen_expr(c->args[0].get())), to_i64(gen_expr(c->args[1].get()))});
        return nothing();
    }
    if (fname == "game2d.set_mouse") {
        llvm::Value* x = to_fp(gen_expr(c->args[0].get()));
        llvm::Value* y = to_fp(gen_expr(c->args[1].get()));
        auto di = [&](size_t i, long long d) { return c->args.size() >= i + 1 ? to_i64(gen_expr(c->args[i].get())) : B->getInt64(d); };
        B->CreateCall(F("vor_g2d_set_mouse", VType::Void, {VType::Float, VType::Float, VType::Int, VType::Int, VType::Int}), {x, y, di(2,0), di(3,0), di(4,0)});
        return nothing();
    }

    // ---- render3d：场景 ----
    if (fname == "render3d.new_scene") return {VType::Str, B->CreateCall(F("vor_r3d_new_scene", VType::Str, {}), {})};
    if (fname == "render3d.scene_add_node") {
        B->CreateCall(F("vor_r3d_scene_add_node", VType::Void, {VType::Str, VType::Str}),
                      {gen_expr(c->args[0].get()).val, gen_expr(c->args[1].get()).val});
        return nothing();
    }
    if (fname == "render3d.scene_add_light") {
        B->CreateCall(F("vor_r3d_scene_add_light", VType::Void, {VType::Str, VType::Str}),
                      {gen_expr(c->args[0].get()).val, gen_expr(c->args[1].get()).val});
        return nothing();
    }
    if (fname == "render3d.scene_set_ambient") {
        B->CreateCall(F("vor_r3d_scene_set_ambient", VType::Void, {VType::Str, VType::Float, VType::Float, VType::Float}),
                      {gen_expr(c->args[0].get()).val, to_fp(gen_expr(c->args[1].get())), to_fp(gen_expr(c->args[2].get())), to_fp(gen_expr(c->args[3].get()))});
        return nothing();
    }
    if (fname == "render3d.scene_set_fog") {
        LVal sc = gen_expr(c->args[0].get());
        llvm::Value* on = to_i64(gen_expr(c->args[1].get()));
        llvm::Value* st = to_fp(gen_expr(c->args[2].get()));
        llvm::Value* en = to_fp(gen_expr(c->args[3].get()));
        auto cl = vec3opt(4, 0.02, 0.03, 0.08);
        B->CreateCall(F("vor_r3d_scene_set_fog", VType::Void, {VType::Str, VType::Int, VType::Float, VType::Float, VType::Int, VType::Float, VType::Float, VType::Float}),
                      {sc.val, on, st, en, cl[0], cl[1], cl[2], cl[3]});
        return nothing();
    }

    // ---- render3d：相机 ----
    if (fname == "render3d.new_camera_perspective") return {VType::Str, B->CreateCall(F("vor_r3d_new_camera_persp", VType::Str, {}), {})};
    if (fname == "render3d.new_camera_ortho") return {VType::Str, B->CreateCall(F("vor_r3d_new_camera_ortho", VType::Str, {}), {})};
    if (fname == "render3d.camera_lookat") {
        std::vector<llvm::Value*> av;
        av.push_back(gen_expr(c->args[0].get()).val);
        for (int i = 1; i < 7; ++i) av.push_back(to_fp(gen_expr(c->args[i].get())));
        B->CreateCall(F("vor_r3d_camera_lookat", VType::Void, {VType::Str, VType::Float,VType::Float,VType::Float,VType::Float,VType::Float,VType::Float}), av);
        return nothing();
    }
    if (fname == "render3d.camera_set") {
        std::vector<llvm::Value*> av;
        av.push_back(gen_expr(c->args[0].get()).val);
        for (int i = 1; i < 7; ++i) av.push_back(to_fp(gen_expr(c->args[i].get())));
        B->CreateCall(F("vor_r3d_camera_set", VType::Void, {VType::Str, VType::Float,VType::Float,VType::Float,VType::Float,VType::Float,VType::Float}), av);
        return nothing();
    }
    if (fname == "render3d.camera_fov") {
        LVal c0 = gen_expr(c->args[0].get());
        if (c->args.size() >= 2) {
            B->CreateCall(F("vor_r3d_camera_fov_set", VType::Void, {VType::Str, VType::Float}), {c0.val, to_fp(gen_expr(c->args[1].get()))});
            return nothing();
        }
        return {VType::Float, B->CreateCall(F("vor_r3d_camera_fov_get", VType::Float, {VType::Str}), {c0.val})};
    }

    // ---- render3d：材质 / 纹理 ----
    if (fname == "render3d.new_material") {
        int n = (int)c->args.size();
        auto df = [&](size_t i, double d) { return c->args.size() >= i + 1 ? to_fp(gen_expr(c->args[i].get())) : llvm::ConstantFP::get(B->getDoubleTy(), d); };
        llvm::Value* r = B->CreateCall(F("vor_r3d_new_material", VType::Str, {VType::Float,VType::Float,VType::Float,VType::Float,VType::Float,VType::Int}),
                                       {df(0,0.8), df(1,0.8), df(2,0.8), df(3,0.0), df(4,0.5), B->getInt64(n)});
        return {VType::Str, r};
    }
    if (fname == "render3d.material_set_texture") {
        B->CreateCall(F("vor_r3d_material_set_texture", VType::Void, {VType::Str, VType::Str}),
                      {gen_expr(c->args[0].get()).val, gen_expr(c->args[1].get()).val});
        return nothing();
    }
    if (fname == "render3d.new_texture_solid") {
        llvm::Value* w = to_i64(gen_expr(c->args[0].get()));
        llvm::Value* h = to_i64(gen_expr(c->args[1].get()));
        llvm::Value* r = to_i64(gen_expr(c->args[2].get()));
        llvm::Value* g = to_i64(gen_expr(c->args[3].get()));
        llvm::Value* b = to_i64(gen_expr(c->args[4].get()));
        return {VType::Str, B->CreateCall(F("vor_r3d_new_texture_solid", VType::Str, {VType::Int,VType::Int,VType::Int,VType::Int,VType::Int}), {w,h,r,g,b})};
    }
    if (fname == "render3d.new_texture_checker") {
        llvm::Value* w = to_i64(gen_expr(c->args[0].get()));
        llvm::Value* h = to_i64(gen_expr(c->args[1].get()));
        llvm::Value* t = to_i64(gen_expr(c->args[2].get()));
        auto cl = vec3opt(3, 255, 255, 255);
        return {VType::Str, B->CreateCall(F("vor_r3d_new_texture_checker", VType::Str, {VType::Int,VType::Int,VType::Int,VType::Int,VType::Float,VType::Float,VType::Float}), {w,h,t,cl[0],cl[1],cl[2],cl[3]})};
    }

    // ---- render3d：网格 ----
    if (fname == "render3d.mesh_box") {
        llvm::Value* sx = to_fp(gen_expr(c->args[0].get()));
        llvm::Value* sy = to_fp(gen_expr(c->args[1].get()));
        llvm::Value* sz = to_fp(gen_expr(c->args[2].get()));
        llvm::Value* m = c->args.size() >= 4 ? gen_expr(c->args[3].get()).val : llvm::Constant::getNullValue(B->getPtrTy());
        return {VType::Str, B->CreateCall(F("vor_r3d_mesh_box", VType::Str, {VType::Float,VType::Float,VType::Float,VType::Str}), {sx,sy,sz,m})};
    }
    if (fname == "render3d.mesh_sphere" || fname == "render3d.mesh_plane" || fname == "render3d.mesh_cylinder") {
        llvm::Value* a = to_fp(gen_expr(c->args[0].get()));
        llvm::Value* b = (c->args.size() >= 2 && fname != "render3d.mesh_plane") ? to_fp(gen_expr(c->args[1].get())) : llvm::ConstantFP::get(B->getDoubleTy(), 1.0);
        llvm::Value* mat = llvm::Constant::getNullValue(B->getPtrTy());
        if (fname == "render3d.mesh_sphere") {
            llvm::Value* st = c->args.size() >= 2 ? to_i64(gen_expr(c->args[1].get())) : B->getInt64(24);
            llvm::Value* sl = c->args.size() >= 3 ? to_i64(gen_expr(c->args[2].get())) : B->getInt64(32);
            mat = c->args.size() >= 4 ? gen_expr(c->args[3].get()).val : mat;
            return {VType::Str, B->CreateCall(F("vor_r3d_mesh_sphere", VType::Str, {VType::Float,VType::Int,VType::Int,VType::Str}), {a,st,sl,mat})};
        } else if (fname == "render3d.mesh_plane") {
            llvm::Value* sub = c->args.size() >= 2 ? to_i64(gen_expr(c->args[1].get())) : B->getInt64(4);
            mat = c->args.size() >= 3 ? gen_expr(c->args[2].get()).val : mat;
            return {VType::Str, B->CreateCall(F("vor_r3d_mesh_plane", VType::Str, {VType::Float,VType::Int,VType::Str}), {a,sub,mat})};
        } else {
            llvm::Value* sl = c->args.size() >= 3 ? to_i64(gen_expr(c->args[2].get())) : B->getInt64(24);
            mat = c->args.size() >= 4 ? gen_expr(c->args[3].get()).val : mat;
            return {VType::Str, B->CreateCall(F("vor_r3d_mesh_cylinder", VType::Str, {VType::Float,VType::Float,VType::Int,VType::Str}), {a,b,sl,mat})};
        }
    }

    // ---- render3d：节点 ----
    if (fname == "render3d.new_node") {
        llvm::Value* m = c->args.size() >= 1 ? gen_expr(c->args[0].get()).val : llvm::Constant::getNullValue(B->getPtrTy());
        return {VType::Str, B->CreateCall(F("vor_r3d_new_node", VType::Str, {VType::Str}), {m})};
    }
    if (fname == "render3d.node_attach_mesh") {
        B->CreateCall(F("vor_r3d_node_attach_mesh", VType::Void, {VType::Str, VType::Str}), {gen_expr(c->args[0].get()).val, gen_expr(c->args[1].get()).val});
        return nothing();
    }
    if (fname == "render3d.node_set_pos" || fname == "render3d.node_set_rot" || fname == "render3d.node_set_scale") {
        const char* setf = fname == "render3d.node_set_pos" ? "vor_r3d_node_set_pos" : (fname == "render3d.node_set_rot" ? "vor_r3d_node_set_rot" : "vor_r3d_node_set_scale");
        B->CreateCall(F(setf, VType::Void, {VType::Str, VType::Float, VType::Float, VType::Float}),
                      {gen_expr(c->args[0].get()).val, to_fp(gen_expr(c->args[1].get())), to_fp(gen_expr(c->args[2].get())), to_fp(gen_expr(c->args[3].get()))});
        return nothing();
    }
    if (fname == "render3d.node_add_child") {
        B->CreateCall(F("vor_r3d_node_add_child", VType::Void, {VType::Str, VType::Str}), {gen_expr(c->args[0].get()).val, gen_expr(c->args[1].get()).val});
        return nothing();
    }

    // ---- render3d：光源 ----
    if (fname == "render3d.new_light_dir" || fname == "render3d.new_light_point") {
        llvm::Value* x = to_fp(gen_expr(c->args[0].get()));
        llvm::Value* y = to_fp(gen_expr(c->args[1].get()));
        llvm::Value* z = to_fp(gen_expr(c->args[2].get()));
        auto cl = vec3opt(3, 1.0, 1.0, 1.0);
        if (fname == "render3d.new_light_dir") {
            llvm::Value* it = c->args.size() >= 5 ? to_fp(gen_expr(c->args[4].get())) : llvm::ConstantFP::get(B->getDoubleTy(), 1.0);
            return {VType::Str, B->CreateCall(F("vor_r3d_new_light_dir", VType::Str, {VType::Float,VType::Float,VType::Float,VType::Int,VType::Float,VType::Float,VType::Float,VType::Float}),
                                              {x,y,z,cl[0],cl[1],cl[2],cl[3],it})};
        }
        llvm::Value* it = c->args.size() >= 5 ? to_fp(gen_expr(c->args[4].get())) : llvm::ConstantFP::get(B->getDoubleTy(), 1.0);
        llvm::Value* rg = c->args.size() >= 6 ? to_fp(gen_expr(c->args[5].get())) : llvm::ConstantFP::get(B->getDoubleTy(), 100.0);
        return {VType::Str, B->CreateCall(F("vor_r3d_new_light_point", VType::Str, {VType::Float,VType::Float,VType::Float,VType::Int,VType::Float,VType::Float,VType::Float,VType::Float,VType::Float}),
                                          {x,y,z,cl[0],cl[1],cl[2],cl[3],it,rg})};
    }

    // ---- render3d：渲染 ----
    if (fname == "render3d.render") {
        llvm::Value* sc = gen_expr(c->args[0].get()).val;
        llvm::Value* cam = gen_expr(c->args[1].get()).val;
        llvm::Value* w = to_i64(gen_expr(c->args[2].get()));
        llvm::Value* h = to_i64(gen_expr(c->args[3].get()));
        return {VType::Str, B->CreateCall(F("vor_r3d_render", VType::Str, {VType::Str, VType::Str, VType::Int, VType::Int}), {sc, cam, w, h})};
    }
    if (fname == "render3d.render_to_window") {
        B->CreateCall(F("vor_r3d_render_to_window", VType::Void, {VType::Str, VType::Str, VType::Int, VType::Int}),
                      {gen_expr(c->args[0].get()).val, gen_expr(c->args[1].get()).val, to_i64(gen_expr(c->args[2].get())), to_i64(gen_expr(c->args[3].get()))});
        return nothing();
    }

    // 容器构造内置（set/pair/tuple/__set_literal__）
    if (c->callee->kind == ExprKind::Identifier) {
        if (fname == "__set_literal__" && !c->args.empty()) {
            LVal lst = gen_expr(c->args[0].get());
            llvm::Value* fn = decl_vor("vor_set_from_list", VType::Set, {VType::List});
            return {VType::Set, B->CreateCall(llvm::cast<llvm::Function>(fn), {lst.val})};
        }
        if (fname == "set") {
            if (c->args.size() == 1) {
                LVal a = gen_expr(c->args[0].get());
                if (a.type == VType::List) {
                    llvm::Value* fn = decl_vor("vor_set_from_list", VType::Set, {VType::List});
                    return {VType::Set, B->CreateCall(llvm::cast<llvm::Function>(fn), {a.val})};
                }
            }
            llvm::Value* ns = decl_vor("vor_list_new", VType::List, {});
            llvm::Value* s = B->CreateCall(llvm::cast<llvm::Function>(ns), {});
            for (auto& a : c->args) {
                LVal v = gen_expr(a.get());
                B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_set_add", VType::Void, {VType::List, VType::Int})),
                              {s, to_i64(v)});
            }
            return {VType::Set, s};
        }
        if ((fname == "pair" || fname == "make_pair") && c->args.size() >= 2) {
            LVal a = gen_expr(c->args[0].get());
            LVal b = gen_expr(c->args[1].get());
            llvm::Value* fn = decl_vor("vor_pair_new", VType::Pair, {VType::Int, VType::Int});
            return {VType::Pair, B->CreateCall(llvm::cast<llvm::Function>(fn), {to_i64(a), to_i64(b)})};
        }
        if (fname == "tuple" && !c->args.empty()) {
            LVal a = gen_expr(c->args[0].get());
            if (a.type == VType::List) {
                llvm::Value* fn = decl_vor("vor_tuple_from_list", VType::Tuple, {VType::List});
                return {VType::Tuple, B->CreateCall(llvm::cast<llvm::Function>(fn), {a.val})};
            }
            // 单元素 -> 1 元组
            llvm::Value* fn = decl_vor("vor_tuple_new", VType::Tuple, {VType::Int});
            llvm::Value* t = B->CreateCall(llvm::cast<llvm::Function>(fn), {B->getInt64(1)});
            B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_tuple_set", VType::Void, {VType::Tuple, VType::Int, VType::Int})),
                          {t, B->getInt64(0), to_i64(a)});
            return {VType::Tuple, t};
        }
        if (fname == "make_tuple") {
            int n = (int)c->args.size();
            llvm::Value* fn = decl_vor("vor_tuple_new", VType::Tuple, {VType::Int});
            llvm::Value* t = B->CreateCall(llvm::cast<llvm::Function>(fn), {B->getInt64(n)});
            for (int i = 0; i < n; ++i) {
                LVal v = gen_expr(c->args[i].get());
                B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_tuple_set", VType::Void, {VType::Tuple, VType::Int, VType::Int})),
                              {t, B->getInt64(i), to_i64(v)});
            }
            return {VType::Tuple, t};
        }
    }

    // 容器成员方法（P2：list.append）
    if (c->callee->kind == ExprKind::MemberAccess) {
        auto* m = static_cast<const MemberAccessExpr*>(c->callee.get());
        LVal obj = gen_expr(m->object.get());
        if (obj.type == VType::List && m->member == "append" && !c->args.empty()) {
            LVal a0 = gen_expr(c->args[0].get());
            B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_list_push", VType::Void, {VType::List, VType::Int})),
                          {obj.val, to_i64(a0)});
            return {VType::Void, nullptr};
        }
        if ((obj.type == VType::List || obj.type == VType::StrList
         || obj.type == VType::StrListList)
        && (m->member == "len" || m->member == "length")) {
            llvm::Value* fn = decl_vor("vor_list_len", VType::Int, {VType::List});
            return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {obj.val})};
        }
        // dict 方法：len / contains / get / get_str
        if (obj.type == VType::Dict) {
            if (m->member == "len" || m->member == "length") {
                llvm::Value* fn = decl_vor("vor_dict_len", VType::Int, {VType::Dict});
                return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {obj.val})};
            }
            if (m->member == "has" || m->member == "contains") {
                LVal k = gen_expr(c->args[0].get());
                const char* f = k.type == VType::Str ? "vor_dict_contains_str" : "vor_dict_contains_int";
                llvm::Value* fn = decl_vor(f, VType::Int, k.type == VType::Str
                                           ? std::vector<VType>{VType::Dict, VType::Str}
                                           : std::vector<VType>{VType::Dict, VType::Int});
                llvm::Value* r = B->CreateCall(llvm::cast<llvm::Function>(fn), {obj.val, k.val});
                return {VType::Bool, B->CreateICmpNE(r, B->getInt64(0))};
            }
            if (m->member == "put" && c->args.size() >= 2) {
                LVal k = gen_expr(c->args[0].get());
                LVal v = gen_expr(c->args[1].get());
                const bool ks = (k.type == VType::Str);
                if (v.type == VType::Str) {
                    llvm::Value* fn = decl_vor(ks ? "vor_dict_set_str_vstr" : "vor_dict_set_int_vstr",
                        VType::Void, ks ? std::vector<VType>{VType::Dict, VType::Str, VType::Str}
                                        : std::vector<VType>{VType::Dict, VType::Int, VType::Str});
                    B->CreateCall(llvm::cast<llvm::Function>(fn), {obj.val, k.val, v.val});
                } else {
                    llvm::Value* fn = decl_vor(ks ? "vor_dict_set_str" : "vor_dict_set_int",
                        VType::Void, ks ? std::vector<VType>{VType::Dict, VType::Str, VType::Int}
                                        : std::vector<VType>{VType::Dict, VType::Int, VType::Int});
                    B->CreateCall(llvm::cast<llvm::Function>(fn), {obj.val, k.val, to_i64(v)});
                }
                return {VType::Void, nullptr};
            }
            if (m->member == "get" && !c->args.empty()) {
                LVal k = gen_expr(c->args[0].get());
                const char* f = k.type == VType::Str ? "vor_dict_get_str" : "vor_dict_get_int";
                llvm::Value* fn = decl_vor(f, VType::Int, k.type == VType::Str
                                           ? std::vector<VType>{VType::Dict, VType::Str}
                                           : std::vector<VType>{VType::Dict, VType::Int});
                return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {obj.val, k.val})};
            }
            if (m->member == "get_str" && !c->args.empty()) {
                LVal k = gen_expr(c->args[0].get());
                const char* f = k.type == VType::Str ? "vor_dict_get_str_vstr" : "vor_dict_get_int_vstr";
                llvm::Value* fn = decl_vor(f, VType::Str, k.type == VType::Str
                                           ? std::vector<VType>{VType::Dict, VType::Str}
                                           : std::vector<VType>{VType::Dict, VType::Int});
                return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(fn), {obj.val, k.val})};
            }
        }
        // set 方法：add / contains / len / remove
        if (obj.type == VType::Set) {
            if (m->member == "add" || m->member == "insert") {
                LVal v = gen_expr(c->args[0].get());
                B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_set_add", VType::Void, {VType::List, VType::Int})),
                              {obj.val, to_i64(v)});
                return {VType::Void, nullptr};
            }
            if (m->member == "contains" || m->member == "has") {
                LVal v = gen_expr(c->args[0].get());
                llvm::Value* fn = decl_vor("vor_set_contains", VType::Int, {VType::List, VType::Int});
                llvm::Value* r = B->CreateCall(llvm::cast<llvm::Function>(fn), {obj.val, to_i64(v)});
                return {VType::Bool, B->CreateICmpNE(r, B->getInt64(0))};
            }
            if (m->member == "len" || m->member == "length") {
                llvm::Value* fn = decl_vor("vor_set_len", VType::Int, {VType::List});
                return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {obj.val})};
            }
            if (m->member == "remove" || m->member == "erase") {
                LVal v = gen_expr(c->args[0].get());
                B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_set_remove", VType::Void, {VType::List, VType::Int})),
                              {obj.val, to_i64(v)});
                return {VType::Void, nullptr};
            }
        }
        // tuple 方法：len
        if (obj.type == VType::Tuple && (m->member == "len" || m->member == "length")) {
            llvm::Value* fn = decl_vor("vor_tuple_len", VType::Int, {VType::Tuple});
            return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {obj.val})};
        }
    }

    // 用户函数
    // 收集参数
    std::vector<llvm::Value*> args;
    std::vector<VType> argTypes;
    for (auto& a : c->args) {
        LVal v = gen_expr(a.get());
        args.push_back(v.val);
        argTypes.push_back(v.type);
    }
    (void)argTypes;
    // 闭包调用：fname 是闭包变量时经函数指针间接调用（P3）
    {
        LVal cv;
        if (lookup_var(fname, cv) && cv.type == VType::Closure) {
            llvm::Value* closure = cv.slot ? B->CreateLoad(B->getPtrTy(), cv.slot) : cv.val;
            llvm::Value* clI64 = B->CreateBitCast(closure, B->getInt64Ty()->getPointerTo());
            llvm::Value* fnraw = B->CreateLoad(B->getInt64Ty(), B->CreateGEP(B->getInt64Ty(), clI64, B->getInt64(0)));
            std::vector<llvm::Type*> ft;
            ft.push_back(B->getPtrTy()); // 闭包指针
            for (auto& t : argTypes) ft.push_back(llvm_type(t));
            llvm::FunctionType* Ftype = llvm::FunctionType::get(llvm_type(VType::Int), ft, false);
            llvm::Value* fn = B->CreateIntToPtr(fnraw, Ftype->getPointerTo());
            std::vector<llvm::Value*> callArgs;
            callArgs.push_back(closure);
            for (auto& a : args) callArgs.push_back(a);
            return {VType::Int, B->CreateCall(Ftype, fn, callArgs, "call")};
        }
    }
    // 目标 Function（签名首参数为 closure 指针，普通调用传 null 占位）
    llvm::Function* callee = mod_->getFunction(fname);
    if (callee) {
        std::vector<llvm::Value*> callArgs;
        callArgs.push_back(llvm::ConstantPointerNull::get(B->getPtrTy()));
        for (auto& a : args) callArgs.push_back(a);
        llvm::Value* r = B->CreateCall(callee, callArgs, "call");
        return {fn_ret_of(fname), r};
    }
    // 未转发扩展模块调用（如 render3d./game2d. 等 P4 按需裁减项）：明确报错而非崩溃
    if (fname.find('.') != std::string::npos) {
        throw std::runtime_error("编译错误：不支持的模块调用 '" + fname +
                                 "'（编译器尚未转发该扩展模块，请改用解释器运行）");
    }
    return {VType::Void, nullptr};
}

VType Gen::fn_ret_of(const std::string& fnname) {
    auto it = fn_ret_map_.find(fnname);
    if (it != fn_ret_map_.end()) return it->second;   // 用户函数：per-function 推断
    return fn_ret_;
}

// 静态推断表达式类型（不发射代码）。只覆盖能确定底型的常见形式；未知一律 Int。
VType Gen::infer_expr_type(const Expr* e) {
    if (!e) return VType::Int;
    switch (e->kind) {
        case ExprKind::Literal: {
            auto* l = static_cast<const LiteralExpr*>(e);
            switch (l->lit_kind) {
                case LiteralExpr::LitKind::Float: return VType::Float;
                case LiteralExpr::LitKind::ExtFloat: return VType::ExtFloat;
                case LiteralExpr::LitKind::String: case LiteralExpr::LitKind::UniString: return VType::Str;
                case LiteralExpr::LitKind::Bool: return VType::Bool;
                default: return VType::Int;
            }
        }
        case ExprKind::Identifier: {
            LVal v;
            if (lookup_var(static_cast<const IdentifierExpr*>(e)->name, v)) return v.type;
            return VType::Int;
        }
        case ExprKind::Cast: {
            auto* c = static_cast<const CastExpr*>(e);
            std::string t = c->target_type;
            if (t == "float" || t == "double") return VType::Float;
            if (t == "str") return VType::Str;
            if (t == "bool") return VType::Bool;
            return VType::Int;
        }
        case ExprKind::UnaryOp: {
            auto* u = static_cast<const UnaryOpExpr*>(e);
            if (u->op == "!" || u->op == "not") return VType::Bool;
            if (u->op == "~" ) { // 解引用 Ref 按其 pointee
                LVal v = gen_expr(u->operand.get());
                if (v.ref_pointee != VType::Void) return v.ref_pointee;
                return VType::Int;
            }
            if (u->op == "@") return VType::Ref;
            return infer_expr_type(u->operand.get());
        }
        case ExprKind::BinaryOp: {
            auto* b = static_cast<const BinaryOpExpr*>(e);
            VType l = infer_expr_type(b->left.get());
            VType r = infer_expr_type(b->right.get());
            const std::string& op = b->op;
            if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">="
                || op == "&&" || op == "||" || op == "&" || op == "|" || op == "^") return VType::Int;
            if (l == VType::Float || r == VType::Float) return VType::Float;   // 浮点算术
            if (op == "+" && (l == VType::Str || r == VType::Str)) return VType::Str; // 字符串拼接
            return VType::Int;
        }
        case ExprKind::TernaryOp: {
            auto* t = static_cast<const TernaryOpExpr*>(e);
            return infer_expr_type(t->then_e.get());
        }
        case ExprKind::Call: {
            // 构造调用 Class(...) -> Object（供方法/函数返回类型推断出指针返回）
            auto* c = static_cast<const CallExpr*>(e);
            if (c->callee->kind == ExprKind::Identifier) {
                const std::string& cn = static_cast<const IdentifierExpr*>(c->callee.get())->name;
                if (classes_.count(cn)) return VType::Object;
            }
            return VType::Int;
        }
        default:
            return VType::Int;
    }
}

// 推断函数返回类型：取返回值最“宽”的类型（float > str > bool > int）
VType Gen::infer_fn_ret(const FunctionDefStmt* fd) {
    VType rt = VType::Int;
    auto widen = [&](VType t) {
        if (t == VType::Object) { rt = VType::Object; return; }  // 类实例：指针返回优先
        if (rt == VType::Object) return;                          // 已确定为 Object，不再降级
        if (t == VType::Float) rt = VType::Float;
        else if (t == VType::Str && rt != VType::Float) rt = VType::Str;
        else if (t == VType::Bool && rt == VType::Int) rt = VType::Bool;
    };
    // 遍历函数体所有 return 语句
    std::vector<const BlockStmt*> blocks{fd->body.get()};
    for (size_t bi = 0; bi < blocks.size(); ++bi) {
        for (auto& st : blocks[bi]->stmts) {
            switch (st->kind) {
                case StmtKind::Return: {
                    auto* r = static_cast<const ReturnStmt*>(st.get());
                    if (r->value) widen(infer_expr_type(r->value.get()));
                    break;
                }
                case StmtKind::Block: blocks.push_back(static_cast<const BlockStmt*>(st.get())); break;
                case StmtKind::If: {
                    auto* i = static_cast<const IfStmt*>(st.get());
                    blocks.push_back(i->then_body.get());
                    for (auto& el : i->elif_list) blocks.push_back(el.body.get());
                    if (i->else_body) blocks.push_back(i->else_body.get());
                    break;
                }
                case StmtKind::ForIn: blocks.push_back(static_cast<const ForInStmt*>(st.get())->body.get()); break;
                case StmtKind::While: blocks.push_back(static_cast<const WhileStmt*>(st.get())->body.get()); break;
                case StmtKind::TryCatch: {
                    auto* t = static_cast<const TryCatchStmt*>(st.get());
                    blocks.push_back(t->try_body.get());
                    if (t->catch_body) blocks.push_back(t->catch_body.get());
                    if (t->finally_body) blocks.push_back(t->finally_body.get());
                    break;
                }
                default: break;
            }
        }
    }
    return rt;
}

std::vector<std::string> Gen::ident_candidates() const {
    std::vector<std::string> c;
    for (const auto& frame : scopes_)
        for (const auto& kv : frame) c.push_back(kv.first);
    for (const auto& f : fns_) {
        std::string k = f.first;
        auto d = k.find('.');
        if (d == std::string::npos) c.push_back(k);   // 非模块成员的内建名
    }
    static const char* base[] = {
        // 常见内置 / 类型名 / 模块名
        "print", "int", "long", "float", "double", "str", "bool", "uint", "char",
        "unistr", "bin", "memadr", "size", "len", "abs", "max", "min", "range",
        "math", "time", "random", "thread", "log", "file", "zip", "xml", "html",
        "sql", "os", "regex", "json", "base64", "datetime", "csv", "hash", "text",
        "net", "sys", "gui", "game2d", "render3d"
    };
    for (const char* b : base) c.push_back(b);
    return c;
}

// 输出一个值到 stdout
void Gen::emit_any(LVal v) {
    switch (v.type) {
        case VType::Int:
            B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_print_i64", VType::Void, {VType::Int})), {v.val});
            break;
        case VType::Float:
            B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_print_double", VType::Void, {VType::Float})), {v.val});
            break;
        case VType::Bool:
            B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_print_bool", VType::Void, {VType::Int})),
                          {B->CreateZExt(v.val, B->getInt64Ty())});
            break;
        case VType::Str:
            B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_print_str", VType::Void, {VType::Str})), {v.val});
            break;
        case VType::ExtFloat:   // 精确十进制本身就是文本指针
            B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_print_str", VType::Void, {VType::Str})), {v.val});
            break;
        case VType::List:
            B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_print_int_list", VType::Void, {VType::List})), {v.val});
            break;
        case VType::Dict:
            B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_print_dict", VType::Void, {VType::Dict})), {v.val});
            break;
        case VType::Set:
            B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_print_set", VType::Void, {VType::Set})), {v.val});
            break;
        case VType::Pair:
            B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_print_pair", VType::Void, {VType::Pair})), {v.val});
            break;
        case VType::Tuple:
            B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_print_tuple", VType::Void, {VType::Tuple})), {v.val});
            break;
        case VType::Void:
        default: break;
    }
}

llvm::Value* Gen::to_i64(LVal v) {
    switch (v.type) {
        case VType::Int:   return v.val;
        case VType::Bool:  return B->CreateZExt(v.val, B->getInt64Ty());
        case VType::Float: return B->CreateBitCast(v.val, B->getInt64Ty());
        default: return v.val;
    }
}

// ==================== 语句 ====================
void Gen::gen_block(const std::vector<StmtPtr>& body) {
    scopes_.emplace_back();
    for (auto& s : body) {
        if (block_has_term(B->GetInsertBlock())) break;
        gen_stmt(s.get());
    }
    scopes_.pop_back();
}

void Gen::gen_return(const ReturnStmt* s) {
    if (!s->value) { B->CreateRetVoid(); return; }
    LVal v = gen_expr(s->value.get());
    fn_ret_ = v.type;
    // 按函数声明的返回类型规整（float/int/bool 互转）
    if (cur_fn_) {
        VType rt = VType::Void;
        auto it = fn_ret_map_.find(cur_fn_->getName().str());
        if (it != fn_ret_map_.end()) rt = it->second;
        else if (cur_fn_->getReturnType() == B->getInt64Ty()) rt = VType::Int; // lambda 固定 i64
        else if (cur_fn_->getReturnType() == B->getDoubleTy()) rt = VType::Float;
        if (rt != VType::Void && rt != v.type) {
            if (rt == VType::Float && v.type == VType::Int) v.val = B->CreateSIToFP(v.val, B->getDoubleTy());
            else if (rt == VType::Float && v.type == VType::Bool) v.val = B->CreateUIToFP(v.val, B->getDoubleTy());
            else if (rt == VType::Int && v.type == VType::Float) v.val = B->CreateFPToSI(v.val, B->getInt64Ty());
            else if (rt == VType::Int && v.type == VType::Bool) v.val = B->CreateZExt(v.val, B->getInt64Ty());
            else if (rt == VType::Bool) v.val = B->CreateICmpNE(to_i64(v), B->getInt64(0));
            else if (rt == VType::Object && v.type == VType::Int)
                v.val = B->CreateIntToPtr(v.val, B->getPtrTy());        // 指针经 i64 槽透传
            else if (rt == VType::Int && v.type == VType::Object)
                v.val = B->CreatePtrToInt(v.val, B->getInt64Ty());
            else if (rt == VType::Object) v.val = B->CreateBitCast(v.val, B->getPtrTy());
            v.type = rt;
        }
    }
    B->CreateRet(v.val);
}

// 闭包（P3）：生成 lambda 匿名函数并构造闭包值
// 捕获策略：把生成时当前作用域所有具槽变量按引用（槽地址）打包进闭包；
// 闭包结构 [0]=函数指针,[i+1]=第 i 个捕获变量的槽地址。lambda 函数第 0 参为闭包指针，
// 捕获变量经闭包槽地址读写，与解释器"环境链实时取值"语义一致。
LVal Gen::gen_lambda(const LambdaExpr* le) {
    // 1) 收集捕获候选（当前可见、具槽、非闭包、非 lambda 参数）
    std::vector<std::string> cap_names;
    std::vector<VType> cap_types;
    std::vector<llvm::Value*> cap_slots;
    {
        std::set<std::string> params;
        for (auto& p : le->params) params.insert(p->name);
        std::set<std::string> seen;
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            for (auto& kv : *it) {
                const std::string& nm = kv.first;
                LVal lv = kv.second;
                if (!lv.slot || lv.type == VType::Closure || params.count(nm) || seen.count(nm)) continue;
                seen.insert(nm);
                cap_names.push_back(nm);
                cap_types.push_back(lv.type);
                cap_slots.push_back(lv.slot);
            }
        }
    }
    size_t nc = cap_names.size();

    // 2) 匿名函数原型：i64 __lamN(ptr closure, <params...>)
    std::string fname = "__lam" + std::to_string(lam_seq_++);
    std::vector<llvm::Type*> argT;
    argT.push_back(B->getPtrTy()); // 闭包指针
    std::vector<VType> ptypes;
    for (auto& p : le->params) {
        VType t = VType::Int;
        if (p->type) {
            if (p->type->base_name == "float") t = VType::Float;
            else if (p->type->base_name == "bool") t = VType::Bool;
            else if (p->type->base_name == "str") t = VType::Str;
        }
        argT.push_back(llvm_type(t));
        ptypes.push_back(t);
    }
    llvm::FunctionType* FT = llvm::FunctionType::get(llvm_type(VType::Int), argT, false);
    llvm::Function* fn = llvm::Function::Create(FT, llvm::Function::InternalLinkage, fname, mod_);

    // 2b) 在 lambda 函数内绑定捕获与参数并生成函数体（作用域内替换 Builder 插入点，
    //     块结束时由 InsertPointGuard 恢复调用点位置，供第 3 步在当前上下文构造闭包值）
    {
        llvm::Function* saved = cur_fn_;
        cur_fn_ = fn;
        llvm::IRBuilderBase::InsertPointGuard guard(*B);
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(B->getContext(), "entry", fn);
        B->SetInsertPoint(entry);
        llvm::DISubprogram* saved_sp = cur_sp_;
        unsigned saved_dbg_line = dbg_line_;
        if (debug_on()) dbg_begin_func(fn, fname.c_str());
        scopes_.emplace_back();
        bool saved_toplevel = top_level_;
        top_level_ = false;

        llvm::Value* closPtr = fn->arg_begin();          // 闭包指针
        closPtr->setName("$closure");
        llvm::Value* clI64 = B->CreateBitCast(closPtr, B->getInt64Ty()->getPointerTo(), "cl.bits");
        // 捕获变量绑定：闭包槽[j+1] 存槽地址，据此实时读写
        for (size_t j = 0; j < nc; ++j) {
            llvm::Value* elem = B->CreateGEP(B->getInt64Ty(), clI64, B->getInt64((long long)(j + 1)));
            llvm::Value* raw = B->CreateLoad(B->getInt64Ty(), elem);
            llvm::Value* slot = B->CreateIntToPtr(raw, B->getPtrTy());
            scopes_.back()[cap_names[j]] =
                LVal{cap_types[j], B->CreateLoad(llvm_type(cap_types[j]), slot), slot};
        }
        // 参数绑定
        for (size_t i = 0; i < le->params.size(); ++i) {
            llvm::Argument* a = fn->arg_begin() + (i + 1);
            a->setName(le->params[i]->name);
            llvm::AllocaInst* aa = make_alloca(ptypes[i], fn, le->params[i]->name.c_str());
            B->CreateStore(a, aa);
            scopes_.back()[le->params[i]->name] = LVal{ptypes[i], B->CreateLoad(llvm_type(ptypes[i]), aa), aa};
            if (debug_on()) dbg_declare(le->params[i]->name, ptypes[i], aa);
        }
        for (auto& st : le->body) {
            if (block_has_term(B->GetInsertBlock())) break;
            gen_stmt(st.get());
        }
        if (!block_has_term(B->GetInsertBlock())) B->CreateRet(B->getInt64(0));
        scopes_.pop_back();
        cur_sp_ = saved_sp;
        dbg_line_ = saved_dbg_line;
        top_level_ = saved_toplevel;
        cur_fn_ = saved;
        fn_ret_ = VType::Int;
    }

    // 3) 构造闭包值：vor_closure_new(n) 分配 (n+1) 个 i64，[0]=fn 指针，[i+1]=槽地址
    llvm::Value* alloc = B->CreateCall(
        llvm::cast<llvm::Function>(declare_runtime("vor_closure_new", B->getPtrTy(), {B->getInt64Ty()})),
        {B->getInt64((long long)nc)});
    llvm::Value* cl = B->CreateBitCast(alloc, B->getInt64Ty()->getPointerTo(), "cl");
    B->CreateStore(B->CreatePtrToInt(fn, B->getInt64Ty()), B->CreateGEP(B->getInt64Ty(), cl, B->getInt64(0)));
    for (size_t j = 0; j < nc; ++j) {
        B->CreateStore(B->CreatePtrToInt(cap_slots[j], B->getInt64Ty()),
                       B->CreateGEP(B->getInt64Ty(), cl, B->getInt64((long long)(j + 1))));
    }
    return {VType::Closure, alloc};
}

void Gen::gen_stmt(const Stmt* s) {
    dbg_step_line();
    switch (s->kind) {
        case StmtKind::VarDecl: {
            auto* vd = static_cast<const VarDeclStmt*>(s);
            // 解构绑定：auto [a, b, c] = 表达式;
            if (vd->destructuring) { gen_destructure(vd); break; }
            const std::string tb = vd->type ? vd->type->base_name : std::string();
            const bool is_auto = (tb == "auto");
            VType t = VType::Int;
            if (vd->type) {
                if (vd->type->base_name == "float") t = VType::Float;
                else if (vd->type->base_name == "bool") t = VType::Bool;
                else if (vd->type->base_name == "str" || vd->type->base_name == "string") t = VType::Str;
                else if (vd->type->base_name == "list") t = VType::List;
                else if (vd->type->base_name == "dict") t = VType::Dict;
                else if (vd->type->base_name == "set") t = VType::Set;
                else if (vd->type->base_name == "pair") t = VType::Pair;
                else if (vd->type->base_name == "tuple") t = VType::Tuple;
                else if (vd->type->base_name == "extfloat") t = VType::ExtFloat;
                else if (classes_.count(vd->type->base_name)) t = VType::Object;   // 用户 class
                else t = VType::Int;
            }
            for (auto& [nm, init] : vd->names) {
                LVal sv = init ? gen_expr(init.get()) : LVal{t, llvm::Constant::getNullValue(llvm_type(t)), nullptr};
                // 动态绑定：容器/字符串/实例按初值实际类型（list 声明等）；auto 完全按初值类型
                VType tEff = t;
                if (is_auto) tEff = sv.type;
                else if (init && (sv.type == VType::List || sv.type == VType::Str || sv.type == VType::Dict
                             || sv.type == VType::StrList || sv.type == VType::Set || sv.type == VType::Pair
                             || sv.type == VType::Tuple || sv.type == VType::Closure
                             || sv.type == VType::Object)) tEff = sv.type;
                llvm::Value* store = sv.val;
                if (tEff == VType::Bool && sv.type != VType::Bool) store = B->CreateICmpNE(sv.val, B->getInt64(0));
                llvm::Value* slot = nullptr;
                if (top_level_) {
                    // 顶层变量提升为模块全局，使函数可跨作用域访问
                    auto* gv = new llvm::GlobalVariable(*mod_, llvm_type(tEff), false,
                                                        llvm::GlobalValue::InternalLinkage,
                                                        llvm::Constant::getNullValue(llvm_type(tEff)), "g_" + nm);
                    B->CreateStore(store, gv);
                    slot = gv;
                } else {
                    slot = make_alloca(tEff, cur_bb()->getParent(), nm.c_str());
                    B->CreateStore(store, slot);
                    if (debug_on()) dbg_declare(nm, tEff, slot);
                }
                // 记录槽 + 类型；标识符访问时按需重新 load
                std::string oc;
                if (tEff == VType::Object)
                    oc = !sv.obj_class.empty() ? sv.obj_class : (is_auto ? std::string() : tb);
                // 元组：记录元素类型描述，供 type() 使用
                std::string tdesc;
                if (tEff == VType::Tuple && init) tdesc = type_desc_of_expr(init.get());
                scopes_.back()[nm] = LVal{tEff, B->CreateLoad(llvm_type(tEff), slot), slot, VType::Void, oc, tdesc};
            }
            break;
        }
        case StmtKind::ClassDecl: {
            gen_class(static_cast<const ClassDeclStmt*>(s));
            break;
        }
        case StmtKind::OperatorDecl: {
            // 按源码顺序生成 operator 函数体（其引用的类/方法此时已生成）
            gen_operator_fn(static_cast<const OperatorDeclStmt*>(s));
            break;
        }
        case StmtKind::ExtDef: {
            // 声明外部 C 函数（无函数体；调用点见 gen_call）
            gen_extdef(static_cast<const ExtDefStmt*>(s));
            break;
        }
        case StmtKind::ExprStmt: {
            gen_expr(static_cast<const ExprStmt*>(s)->expr.get());
            break;
        }
        case StmtKind::If: {
            auto* ifs = static_cast<const IfStmt*>(s);
            llvm::Function* F = cur_bb()->getParent();
            auto mkBB = [&](const char* n) { return llvm::BasicBlock::Create(B->getContext(), n, F); };
            llvm::BasicBlock* mergeBB = mkBB("if.end");
            // 主 if
            llvm::BasicBlock* thenBB = mkBB("if.then");
            llvm::BasicBlock* nextBB = mkBB("if.next");
            LVal cond = gen_expr(ifs->cond.get());
            B->CreateCondBr(cond.val, thenBB, nextBB);
            B->SetInsertPoint(thenBB);
            for (auto& st : ifs->then_body->stmts) gen_stmt(st.get());
            if (!block_has_term(cur_bb())) B->CreateBr(mergeBB);
            // elif 链：各自独立 cond->body 块，串在 next 上
            llvm::BasicBlock* cur = nextBB;
            for (auto& el : ifs->elif_list) {
                llvm::BasicBlock* eThen = mkBB("elif.then");
                llvm::BasicBlock* eNext = mkBB("elif.next");
                B->SetInsertPoint(cur);
                LVal ec = gen_expr(el.cond.get());
                B->CreateCondBr(ec.val, eThen, eNext);
                B->SetInsertPoint(eThen);
                for (auto& st : el.body->stmts) gen_stmt(st.get());
                if (!block_has_term(cur_bb())) B->CreateBr(mergeBB);
                cur = eNext;
            }
            // else（或缺省）汇入 merge
            B->SetInsertPoint(cur);
            if (ifs->else_body) {
                for (auto& st : ifs->else_body->stmts) gen_stmt(st.get());
            }
            if (!block_has_term(cur_bb())) B->CreateBr(mergeBB);
            B->SetInsertPoint(mergeBB);
            break;
        }
        case StmtKind::While: {
            auto* w = static_cast<const WhileStmt*>(s);
            llvm::Function* F = cur_bb()->getParent();
            llvm::BasicBlock* condBB = llvm::BasicBlock::Create(B->getContext(), "while.cond", F);
            llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(B->getContext(), "while.body", F);
            llvm::BasicBlock* endBB = llvm::BasicBlock::Create(B->getContext(), "while.end", F);

            B->CreateBr(condBB);
            B->SetInsertPoint(condBB);
            LVal cond = gen_expr(w->cond.get());
            B->CreateCondBr(cond.val, bodyBB, endBB);

            B->SetInsertPoint(bodyBB);
            for (auto& st : w->body->stmts) gen_stmt(st.get());
            if (!block_has_term(cur_bb())) B->CreateBr(condBB);

            B->SetInsertPoint(endBB);
            break;
        }
        case StmtKind::ForIn: {
            auto* fi = static_cast<const ForInStmt*>(s);
            llvm::Function* F = cur_bb()->getParent();
            LVal cont = gen_expr(fi->container.get());
            // 支持遍历 int list / set / tuple / int-key dict
            if (cont.type != VType::List && cont.type != VType::Dict
                && cont.type != VType::Set && cont.type != VType::Tuple
                && cont.type != VType::StrList && cont.type != VType::StrListList) break;
            const bool isDict = (cont.type == VType::Dict);
            const bool isTuple = (cont.type == VType::Tuple);
            const bool isStrList = (cont.type == VType::StrList);
            const bool isStrListList = (cont.type == VType::StrListList);

            llvm::BasicBlock* condBB = llvm::BasicBlock::Create(B->getContext(), "for.cond", F);
            llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(B->getContext(), "for.body", F);
            llvm::BasicBlock* updBB  = llvm::BasicBlock::Create(B->getContext(), "for.upd", F);
            llvm::BasicBlock* endBB  = llvm::BasicBlock::Create(B->getContext(), "for.end", F);

            llvm::AllocaInst* idxSlot = make_alloca(VType::Int, F, "for.idx");
            B->CreateStore(B->getInt64(0), idxSlot);
            B->CreateBr(condBB);

            // 索引
            B->SetInsertPoint(updBB); // 先建 upd（含计数递增），供 continue/下次迭代使用
            llvm::Value* i1 = B->CreateLoad(B->getInt64Ty(), idxSlot);
            B->CreateStore(B->CreateAdd(i1, B->getInt64(1)), idxSlot);
            B->CreateBr(condBB);

            B->SetInsertPoint(condBB);
            llvm::Value* len = isDict
                ? B->CreateCall(llvm::cast<llvm::Function>(
                    decl_vor("vor_dict_int_key_count", VType::Int, {VType::Dict})), {cont.val})
                : (isTuple
                   ? B->CreateCall(llvm::cast<llvm::Function>(
                       decl_vor("vor_tuple_len", VType::Int, {VType::Tuple})), {cont.val})
                   : B->CreateCall(llvm::cast<llvm::Function>(
                       decl_vor("vor_list_len", VType::Int, {VType::List})), {cont.val}));
            llvm::Value* ic = B->CreateLoad(B->getInt64Ty(), idxSlot);
            llvm::Value* less = B->CreateICmpSLT(ic, len);
            B->CreateCondBr(less, bodyBB, endBB);

            // 主题：把元素 bind 到 var（按容器元素类型选择 int/str）
            B->SetInsertPoint(bodyBB);
            const VType elType = isStrList ? VType::Str
                                 : (isStrListList ? VType::StrList : VType::Int);
            llvm::AllocaInst* varSlot = make_alloca(elType, F, fi->var.c_str());
            llvm::Value* idxv = B->CreateLoad(B->getInt64Ty(), idxSlot);
            llvm::Value* elem;
            if (isDict)
                elem = B->CreateCall(llvm::cast<llvm::Function>(
                    decl_vor("vor_dict_int_key_at", VType::Int, {VType::Dict, VType::Int})), {cont.val, idxv});
            else if (isTuple)
                elem = B->CreateCall(llvm::cast<llvm::Function>(
                    decl_vor("vor_tuple_at", VType::Int, {VType::Tuple, VType::Int})), {cont.val, idxv});
            else if (isStrList)
                elem = B->CreateCall(llvm::cast<llvm::Function>(
                    decl_vor("vor_list_get_str", VType::Str, {VType::StrList, VType::Int})), {cont.val, idxv});
            else if (isStrListList)
                elem = B->CreateCall(llvm::cast<llvm::Function>(
                    decl_vor("vor_list_get_strlist", VType::StrList, {VType::StrListList, VType::Int})), {cont.val, idxv});
            else
                elem = B->CreateCall(llvm::cast<llvm::Function>(
                    decl_vor("vor_list_get", VType::Int, {VType::List, VType::Int})), {cont.val, idxv});
            B->CreateStore(elem, varSlot);
            scopes_.emplace_back();
            scopes_.back()[fi->var] = LVal{elType, B->CreateLoad(llvm_type(elType), varSlot), varSlot};
            for (auto& st : fi->body->stmts) {
                if (block_has_term(B->GetInsertBlock())) break;
                gen_stmt(st.get());
            }
            if (!block_has_term(cur_bb())) B->CreateBr(updBB);
            scopes_.pop_back();

            B->SetInsertPoint(endBB);
            break;
        }
        case StmtKind::TryCatch: {
            auto* tc = static_cast<const TryCatchStmt*>(s);
            llvm::Function* F = cur_bb()->getParent();
            auto mkBB = [&](const char* n) { return llvm::BasicBlock::Create(B->getContext(), n, F); };
            llvm::BasicBlock* tryBB = mkBB("try.body");
            llvm::BasicBlock* catchBB = mkBB("try.catch");
            llvm::BasicBlock* endBB = mkBB("try.end");
            llvm::AllocaInst* jb = B->CreateAlloca(B->getInt64Ty(), B->getInt64(512), "jb");
            // int vor_ex_setjmp(void*) —— runtime 自实现，返回 0 首次/非 0 异常
            llvm::Function* sjf = llvm::cast<llvm::Function>(
                declare_runtime("vor_ex_setjmp", B->getInt32Ty(), {B->getPtrTy()}));
            llvm::Value* sj = B->CreateCall(sjf, {jb});
            llvm::Value* isexc = B->CreateICmpEQ(sj, B->getInt32(0), "isexc");
            B->CreateCondBr(isexc, tryBB, catchBB);

            auto gen_finally = [&]() {
                if (tc->finally_body) {
                    for (auto& st : tc->finally_body->stmts) {
                        if (block_has_term(B->GetInsertBlock())) break;
                        gen_stmt(st.get());
                    }
                }
            };

            // ---- try 体：注册 handler，执行，弹出 ----
            B->SetInsertPoint(tryBB);
            B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_ex_push", VType::Void, {VType::Str})),
                          {B->CreatePointerCast(jb, B->getPtrTy())});
            for (auto& st : tc->try_body->stmts) {
                if (block_has_term(B->GetInsertBlock())) break;
                gen_stmt(st.get());
            }
            if (!block_has_term(B->GetInsertBlock())) {
                B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_ex_pop", VType::Void, {})), {});
                gen_finally();
                B->CreateBr(endBB);
            }

            // ---- catch 分支 ----
            B->SetInsertPoint(catchBB);
            if (tc->catch_body) {
                // 写了类型：先按类型过滤；不匹配则带上原类型与消息继续向上抛
                llvm::BasicBlock* bodyBB = mkBB("try.catch.body");
                if (!tc->exception_type.empty() && tc->exception_type != "Error") {
                    llvm::BasicBlock* mismatchBB = mkBB("try.catch.mismatch");
                    llvm::Value* ty = B->CreateCall(
                        llvm::cast<llvm::Function>(decl_vor("vor_ex_type", VType::Str, {})), {});
                    llvm::Value* cmp = B->CreateCall(llvm::cast<llvm::Function>(
                        decl_vor("vor_str_cmp", VType::Int, {VType::Str, VType::Str})),
                        {ty, vstr_const(tc->exception_type)});
                    B->CreateCondBr(B->CreateICmpEQ(cmp, B->getInt64(0)), bodyBB, mismatchBB);
                    B->SetInsertPoint(mismatchBB);
                    llvm::Value* ty2 = B->CreateCall(
                        llvm::cast<llvm::Function>(decl_vor("vor_ex_type", VType::Str, {})), {});
                    llvm::Value* mv2 = B->CreateCall(
                        llvm::cast<llvm::Function>(decl_vor("vor_ex_caught", VType::Str, {})), {});
                    B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_ex_pop", VType::Void, {})), {});
                    B->CreateCall(llvm::cast<llvm::Function>(
                        decl_vor("vor_throw_typed", VType::Void, {VType::Str, VType::Str})), {ty2, mv2});
                    B->CreateUnreachable();
                } else {
                    B->CreateBr(bodyBB);
                }
                B->SetInsertPoint(bodyBB);
                llvm::Value* msgv = B->CreateCall(
                    llvm::cast<llvm::Function>(decl_vor("vor_ex_caught", VType::Str, {})), {});
                bool has_var = !tc->exception_var.empty();
                bool saved_top = top_level_;
                top_level_ = false;
                scopes_.emplace_back();
                if (has_var) {
                    llvm::AllocaInst* slot = make_alloca(VType::Str, F, tc->exception_var.c_str());
                    B->CreateStore(msgv, slot);
                    scopes_.back()[tc->exception_var] =
                        LVal{VType::Str, B->CreateLoad(B->getPtrTy(), slot), slot};
                }
                for (auto& st : tc->catch_body->stmts) {
                    if (block_has_term(B->GetInsertBlock())) break;
                    gen_stmt(st.get());
                }
                scopes_.pop_back();
                top_level_ = saved_top;
                if (!block_has_term(B->GetInsertBlock())) {
                    B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_ex_pop", VType::Void, {})), {});
                    gen_finally();
                    B->CreateBr(endBB);
                }
            } else {
                // 无 catch：运行 finally 后重新抛出（对父级 handler）
                gen_finally();
                llvm::Value* msgv = B->CreateCall(
                    llvm::cast<llvm::Function>(decl_vor("vor_ex_caught", VType::Str, {})), {});
                B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_ex_pop", VType::Void, {})), {});
                B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_throw_str", VType::Void, {VType::Str})),
                              {msgv});
                B->CreateUnreachable();
            }

            B->SetInsertPoint(endBB);
            break;
        }
        case StmtKind::Assert: {
            auto* a = static_cast<const AssertStmt*>(s);
            llvm::Function* F = cur_bb()->getParent();
            llvm::BasicBlock* passBB = llvm::BasicBlock::Create(B->getContext(), "assert.pass", F);
            llvm::BasicBlock* failBB = llvm::BasicBlock::Create(B->getContext(), "assert.fail", F);
            LVal cond = gen_expr(a->cond.get());
            B->CreateCondBr(cond.val, passBB, failBB);
            // fail: throw "Assertion failed[: <msg>]"
            B->SetInsertPoint(failBB);
            // 内联 message->string 转换（与 gen_call 内 val_to_str 等价，跨函数复用）
            llvm::Value* msgStr = B->CreateCall(
                llvm::cast<llvm::Function>(decl_vor("vor_str_from_cstr", VType::Str, {VType::Str})),
                {B->CreateGlobalString("Assertion failed", "afail")});
            if (a->message) {
                LVal mv = gen_expr(a->message.get());
                llvm::Value* part = nullptr;
                if (mv.type == VType::Str) part = mv.val;
                else if (mv.type == VType::Int)
                    part = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_i64_to_str", VType::Str, {VType::Int})), {mv.val});
                else if (mv.type == VType::Float)
                    part = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_double_to_str", VType::Str, {VType::Float})), {mv.val});
                else if (mv.type == VType::Bool) {
                    llvm::Value* t = B->CreateGlobalString("true", "bstr");
                    llvm::Value* f = B->CreateGlobalString("false", "bstr");
                    part = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_str_from_cstr", VType::Str, {VType::Str})),
                                         {B->CreateSelect(mv.val, t, f)});
                } else {
                    part = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_str_from_cstr", VType::Str, {VType::Str})),
                                         {B->CreateGlobalString("", "estr")});
                }
                llvm::Value* sep = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_str_from_cstr", VType::Str, {VType::Str})),
                                                 {B->CreateGlobalString(": ", "asep")});
                llvm::Value* head = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_str_concat", VType::Str, {VType::Str, VType::Str})), {msgStr, sep});
                msgStr = B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_str_concat", VType::Str, {VType::Str, VType::Str})), {head, part});
            }
            B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_throw_str", VType::Void, {VType::Str})), {msgStr});
            B->CreateUnreachable();
            B->SetInsertPoint(passBB);
            break;
        }
        case StmtKind::Raise: {
            auto* rs = static_cast<const RaiseStmt*>(s);
            llvm::Function* throwFn = llvm::cast<llvm::Function>(
                decl_vor("vor_throw_typed", VType::Void, {VType::Str, VType::Str}));
            if (!rs->value) {
                // raise;  重抛当前异常（类型 + 消息）
                llvm::Value* ty = B->CreateCall(
                    llvm::cast<llvm::Function>(decl_vor("vor_ex_type", VType::Str, {})), {});
                llvm::Value* mv = B->CreateCall(
                    llvm::cast<llvm::Function>(decl_vor("vor_ex_caught", VType::Str, {})), {});
                B->CreateCall(throwFn, {ty, mv});
                B->CreateUnreachable();
                break;
            }
            // raise 表达式;  内建错误构造器 -> 取其类型名与首个实参作消息
            std::string etype = "Error";
            const Expr* msgExpr = rs->value.get();
            if (msgExpr->kind == ExprKind::Call) {
                auto* c = static_cast<const CallExpr*>(msgExpr);
                if (c->callee->kind == ExprKind::Identifier) {
                    const std::string& cn =
                        static_cast<const IdentifierExpr*>(c->callee.get())->name;
                    if (is_builtin_error_type(cn)) {
                        etype = cn;
                        msgExpr = c->args.empty() ? nullptr : c->args[0].get();
                    }
                }
            }
            llvm::Value* msgStr = msgExpr
                ? to_vstr(gen_expr(msgExpr))
                : vstr_const("");
            B->CreateCall(throwFn, {vstr_const(etype), msgStr});
            B->CreateUnreachable();
            break;
        }
        case StmtKind::Match: {
            auto* m = static_cast<const MatchStmt*>(s);
            llvm::Function* F = cur_bb()->getParent();
            auto mkBB = [&](const char* n) { return llvm::BasicBlock::Create(B->getContext(), n, F); };
            llvm::BasicBlock* endBB = mkBB("match.end");
            LVal subj = gen_expr(m->subject.get());
            // 每个 case 生成一个条件跳转；default/绑定模式 = 无条件 else
            llvm::BasicBlock* cur = cur_bb();
            bool emitted = false;
            for (auto& cs : m->cases) {
                llvm::BasicBlock* bodyBB = mkBB("match.body");
                llvm::BasicBlock* nextBB = mkBB("match.next");
                if (!cs.pattern || cs.pattern->kind == ExprKind::Identifier) {
                    // default / 绑定模式：无条件进入（编译器不绑定，视作通配）
                    B->SetInsertPoint(cur);
                    B->CreateBr(bodyBB);
                } else {
                    // 字面量：subject == pattern
                    B->SetInsertPoint(cur);
                    // 构造比较：subject 与 case 字面量相等
                    // 复用 gen_expr 对 BinaryOpExpr "==" 的处理，但需 subject 作为操作数。
                    // 直接构造临时表达式不可行（gen_expr 需新节点），改用手动比较。
                    LVal pv = gen_expr(cs.pattern.get());
                    llvm::Value* cmp;
                    if (subj.type == VType::Str || pv.type == VType::Str) {
                        llvm::Value* fn = decl_vor("vor_str_cmp", VType::Int, {VType::Str, VType::Str});
                        llvm::Value* l = subj.type == VType::Str ? subj.val
                                : B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_i64_to_str", VType::Str, {VType::Int})), {to_i64(subj)});
                        llvm::Value* r = pv.type == VType::Str ? pv.val
                                : B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_i64_to_str", VType::Str, {VType::Int})), {to_i64(pv)});
                        llvm::Value* c = B->CreateCall(llvm::cast<llvm::Function>(fn), {l, r});
                        cmp = B->CreateICmpEQ(c, B->getInt64(0));
                    } else {
                        cmp = B->CreateICmpEQ(to_i64(subj), to_i64(pv));
                    }
                    B->CreateCondBr(cmp, bodyBB, nextBB);
                }
                B->SetInsertPoint(bodyBB);
                for (auto& st : cs.body->stmts) gen_stmt(st.get());
                if (!block_has_term(cur_bb())) B->CreateBr(endBB);
                cur = nextBB;
                emitted = true;
            }
            if (emitted) {
                B->SetInsertPoint(cur);
                B->CreateBr(endBB);
            }
            B->SetInsertPoint(endBB);
            break;
        }
        case StmtKind::Return:
            gen_return(static_cast<const ReturnStmt*>(s));
            break;
        case StmtKind::FunctionDef: {
            auto* fd = static_cast<const FunctionDefStmt*>(s);
            // 函数原型：返回类型默认 i64，参数按类型；首参数为闭包指针（供一等函数传参统一签名）
            std::vector<llvm::Type*> params;
            std::vector<VType> ptypes;
            params.push_back(B->getPtrTy()); // closure 参数（函数体内忽略）
            for (auto& p : fd->params) {
                VType t = VType::Int;
                if (p->type) t = type_from_name(p->type->base_name);   // class 名 -> Object
                params.push_back(llvm_type(t));
                ptypes.push_back(t);
            }
            llvm::FunctionType* FT = llvm::FunctionType::get(llvm_type(VType::Int), params, false);
            // 返回类型：声明了就用声明类型，否则（def / auto）按 return 语句推断
            scopes_.emplace_back();
            for (size_t k = 0; k < fd->params.size() && k < ptypes.size(); ++k) {
                LVal lv{ptypes[k], nullptr};
                if (ptypes[k] == VType::Object && fd->params[k]->type)
                    lv.obj_class = fd->params[k]->type->base_name;
                scopes_.back()[fd->params[k]->name] = lv;
            }
            VType rt = (fd->ret_type.empty() || fd->ret_type == "auto")
                           ? infer_fn_ret(fd) : type_from_name(fd->ret_type);
            scopes_.pop_back();
            fn_ret_map_[fd->name] = rt;
            FT = llvm::FunctionType::get(llvm_type(rt), params, false);
            llvm::Function* fn = llvm::Function::Create(FT, llvm::Function::ExternalLinkage, fd->name, mod_);

            // 失败：无法递归生成已存在（首次）。设入口不做，此处仅声明。
            // LLVM 需要函数体在此生成；这里直接生成体。
            llvm::Function* saved = cur_fn_;
            cur_fn_ = fn;
            llvm::IRBuilderBase::InsertPointGuard guard(*B);
            llvm::BasicBlock* entry = llvm::BasicBlock::Create(B->getContext(), "entry", fn);
            B->SetInsertPoint(entry);
            llvm::DISubprogram* saved_sp = cur_sp_;
            unsigned saved_dbg_line = dbg_line_;
            if (debug_on()) dbg_begin_func(fn, fd->name.c_str());
            scopes_.emplace_back();
            bool saved_toplevel = top_level_;
            top_level_ = false;   // 函数体内变量为局部
            unsigned i = 0;
            for (auto& p : fd->params) {
                llvm::Argument* arg = fn->arg_begin() + i + 1; // 跳过 closure 参数
                arg->setName(p->name);
                llvm::AllocaInst* a = make_alloca(ptypes[i], fn, p->name.c_str());
                B->CreateStore(arg, a);
                LVal lv{ptypes[i], B->CreateLoad(llvm_type(ptypes[i]), a)};
                if (ptypes[i] == VType::Object && p->type) lv.obj_class = p->type->base_name;
                scopes_.back()[p->name] = lv;
                if (debug_on()) dbg_declare(p->name, ptypes[i], a);
                ++i;
            }
            for (auto& st : fd->body->stmts) {
                if (block_has_term(B->GetInsertBlock())) break;
                gen_stmt(st.get());
            }
            if (!block_has_term(B->GetInsertBlock()))
                B->CreateRet(llvm::Constant::getNullValue(llvm_type(rt)));
            scopes_.pop_back();
            cur_sp_ = saved_sp;
            dbg_line_ = saved_dbg_line;
            top_level_ = saved_toplevel;
            cur_fn_ = saved;
            fn_ret_ = rt;
            break;
        }
        case StmtKind::Assign: {
            auto* as = static_cast<const AssignStmt*>(s);
            gen_expr(as->assign.get());
            break;
        }
        default:
            break;
    }
}

// ==================== class / 实例（运行时装箱） ====================
void Gen::collect_classes() {
    for (auto& st : prog_.stmts) {
        if (st->kind != StmtKind::ClassDecl) continue;
        auto* cd = static_cast<const ClassDeclStmt*>(st.get());
        ClassInfo ci;
        ci.name = cd->name;
        ci.parent = cd->parent;
        for (auto& f : cd->fields) ci.fields.emplace_back(f.type, f.name);
        for (auto& m : cd->methods) ci.methods.push_back(m.get());
        classes_[cd->name] = std::move(ci);
    }
}

const Gen::ClassInfo* Gen::find_class(const std::string& n) const {
    auto it = classes_.find(n);
    return it == classes_.end() ? nullptr : &it->second;
}

// 沿父链查找方法（子类优先）；owner 回传定义所在类名
const FunctionDefStmt* Gen::find_method(const std::string& cls, const std::string& name,
                                        std::string* owner) const {
    const ClassInfo* ci = find_class(cls);
    while (ci) {
        for (auto* m : ci->methods)
            if (m->name == name) { if (owner) *owner = ci->name; return m; }
        ci = ci->parent.empty() ? nullptr : find_class(ci->parent);
    }
    return nullptr;
}

llvm::GlobalVariable* Gen::class_global(const std::string& name) {
    const std::string gname = "g_cls_" + name;
    if (llvm::GlobalVariable* gv = mod_->getGlobalVariable(gname)) return gv;
    return new llvm::GlobalVariable(*mod_, B->getPtrTy(), false,
                                    llvm::GlobalValue::InternalLinkage,
                                    llvm::Constant::getNullValue(B->getPtrTy()), gname);
}

llvm::Value* Gen::cstr(const std::string& s) { return B->CreateGlobalString(s, "cstr"); }

llvm::Value* Gen::coerce_arg(LVal v, VType target) {
    if (target == VType::Float && v.type != VType::Float)
        return B->CreateSIToFP(v.val, B->getDoubleTy(), "a.f");
    if (target == VType::Bool && v.type != VType::Bool)
        return B->CreateICmpNE(v.val, B->getInt64(0), "a.b");
    // 类实例与整型槽互转（64 位同宽，按位透传，避免 IR 类型不匹配）
    if (target == VType::Int && v.type == VType::Object)
        return B->CreatePtrToInt(v.val, B->getInt64Ty(), "a.p2i");
    if (target == VType::Object && v.type == VType::Int)
        return B->CreateIntToPtr(v.val, B->getPtrTy(), "a.i2p");
    return v.val;   // int / 指针同宽
}

// 常量 C 字符串 -> VStr*（需要 VStr* 形参的运行时函数使用）
llvm::Value* Gen::vstr_const(const std::string& s) {
    return B->CreateCall(
        llvm::cast<llvm::Function>(decl_vor("vor_str_from_cstr", VType::Str, {VType::Str})),
        {B->CreateGlobalString(s, "vs")});
}

// 把值规整为 VStr*（供 raise 消息使用；str 直接返回，标量转字符串）
llvm::Value* Gen::to_vstr(LVal v) {
    auto cvt = [&](const char* fn, std::vector<VType> p, llvm::Value* a) {
        return B->CreateCall(llvm::cast<llvm::Function>(decl_vor(fn, VType::Str, p)), {a});
    };
    if (v.type == VType::Str) return v.val;
    if (v.type == VType::Int)   return cvt("vor_i64_to_str", {VType::Int}, v.val);
    if (v.type == VType::Float) return cvt("vor_double_to_str", {VType::Float}, v.val);
    if (v.type == VType::Bool) {
        llvm::Value* t = B->CreateGlobalString("true", "bstr");
        llvm::Value* f = B->CreateGlobalString("false", "bstr");
        return cvt("vor_str_from_cstr", {VType::Str}, B->CreateSelect(v.val, t, f));
    }
    return cvt("vor_str_from_cstr", {VType::Str}, B->CreateGlobalString("", "estr"));
}

void Gen::gen_class(const ClassDeclStmt* cd) {
    llvm::GlobalVariable* gv = class_global(cd->name);
    llvm::Value* basePtr = llvm::Constant::getNullValue(B->getPtrTy());
    if (!cd->parent.empty()) {
        if (!classes_.count(cd->parent))
            throw std::runtime_error("unknown parent class '" + cd->parent +
                                     "' (in class " + cd->name + ")");
        basePtr = B->CreateLoad(B->getPtrTy(), class_global(cd->parent), "base");
    }
    llvm::Function* newFn = llvm::cast<llvm::Function>(declare_runtime(
        "vor_class_new", B->getPtrTy(), {B->getPtrTy(), B->getPtrTy()}));
    llvm::Value* cls = B->CreateCall(newFn, {cstr(cd->name), basePtr}, "cls");
    B->CreateStore(cls, gv);
    llvm::Function* addFn = llvm::cast<llvm::Function>(declare_runtime(
        "vor_class_add_field", B->getVoidTy(), {B->getPtrTy(), B->getPtrTy()}));
    for (auto& f : cd->fields) B->CreateCall(addFn, {cls, cstr(f.name)});
    for (auto& m : cd->methods) gen_method_fn(cd->name, m.get());
}

// 方法：签名 (self, 形参...)；self 为 Object，其余形参按声明类型
void Gen::gen_method_fn(const std::string& cls, const FunctionDefStmt* fd) {
    const std::string mname = cls + "." + fd->name;
    if (mod_->getFunction(mname)) return;
    auto pv = [this](const auto& p) -> VType {
        if (!p || !p->type) return VType::Int;
        return type_from_name(p->type->base_name);   // class 名 -> Object
    };
    auto ptn = [](const auto& p) -> std::string {
        return (p && p->type) ? p->type->base_name : std::string();
    };
    std::vector<llvm::Type*> params{B->getPtrTy()};   // self
    std::vector<VType> ptypes;
    std::vector<std::string> ptnames;                 // 形参声明的类型名（供 obj_class 记录）
    for (size_t i = 1; i < fd->params.size(); ++i) {
        VType t = pv(fd->params[i]);
        params.push_back(llvm_type(t));
        ptypes.push_back(t);
        ptnames.push_back(ptn(fd->params[i]));
    }
    // 推断返回类型：临时作用域放入 self 与形参
    scopes_.emplace_back();
    scopes_.back()["self"] = LVal{VType::Object, nullptr, nullptr, VType::Void, cls};
    for (size_t i = 0; i < ptypes.size(); ++i) {
        LVal lv{ptypes[i], nullptr};
        if (ptypes[i] == VType::Object) lv.obj_class = ptnames[i];
        scopes_.back()[fd->params[i + 1]->name] = lv;
    }
    VType rt = (fd->ret_type.empty() || fd->ret_type == "auto")
                   ? infer_fn_ret(fd) : type_from_name(fd->ret_type);
    scopes_.pop_back();
    fn_ret_map_[mname] = rt;

    llvm::FunctionType* FT = llvm::FunctionType::get(llvm_type(rt), params, false);
    llvm::Function* fn = llvm::Function::Create(FT, llvm::Function::ExternalLinkage, mname, mod_);
    llvm::Function* saved = cur_fn_;
    cur_fn_ = fn;
    llvm::IRBuilderBase::InsertPointGuard guard(*B);
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(B->getContext(), "entry", fn);
    B->SetInsertPoint(entry);
    llvm::DISubprogram* saved_sp = cur_sp_;
    unsigned saved_dbg_line = dbg_line_;
    if (debug_on()) dbg_begin_func(fn, mname.c_str());
    scopes_.emplace_back();
    bool saved_toplevel = top_level_;
    top_level_ = false;
    fn->arg_begin()->setName("self");
    scopes_.back()["self"] = LVal{VType::Object, fn->getArg(0), nullptr, VType::Void, cls};
    for (size_t i = 0; i < ptypes.size(); ++i) {
        const std::string& pn = fd->params[i + 1]->name;
        llvm::Argument* arg = fn->arg_begin() + (unsigned)i + 1;
        arg->setName(pn);
        llvm::AllocaInst* a = make_alloca(ptypes[i], fn, pn.c_str());
        B->CreateStore(arg, a);
        LVal lv{ptypes[i], B->CreateLoad(llvm_type(ptypes[i]), a)};
        if (ptypes[i] == VType::Object) lv.obj_class = ptnames[i];
        scopes_.back()[pn] = lv;
        if (debug_on()) dbg_declare(pn, ptypes[i], a);
    }
    for (auto& st : fd->body->stmts) {
        if (block_has_term(B->GetInsertBlock())) break;
        gen_stmt(st.get());
    }
    if (!block_has_term(B->GetInsertBlock()))
        B->CreateRet(llvm::Constant::getNullValue(llvm_type(rt)));
    scopes_.pop_back();
    cur_sp_ = saved_sp;
    dbg_line_ = saved_dbg_line;
    top_level_ = saved_toplevel;
    cur_fn_ = saved;
    fn_ret_ = rt;
}

LVal Gen::gen_construct(const std::string& cls, const CallExpr* c) {
    if (!classes_.count(cls)) throw std::runtime_error("unknown class '" + cls + "'");
    auto pv = [this](const auto& p) -> VType {
        if (!p || !p->type) return VType::Int;
        return type_from_name(p->type->base_name);   // class 名 -> Object
    };
    llvm::Value* clsPtr = B->CreateLoad(B->getPtrTy(), class_global(cls), "cls");
    llvm::Function* newFn = llvm::cast<llvm::Function>(declare_runtime(
        "vor_obj_new", B->getPtrTy(), {B->getPtrTy()}));
    llvm::Value* obj = B->CreateCall(newFn, {clsPtr}, "obj");
    // init：沿父链解析（子类优先）
    std::string owner;
    const FunctionDefStmt* init = find_method(cls, "init", &owner);
    if (init) {
        llvm::Function* fn = mod_->getFunction(owner + ".init");
        if (!fn) throw std::runtime_error("method '" + owner + ".init' not generated");
        std::vector<llvm::Value*> args{obj};
        for (size_t i = 0; i < c->args.size(); ++i) {
            LVal a = gen_expr(c->args[i].get());
            VType want = (i + 1 < init->params.size()) ? pv(init->params[i + 1]) : VType::Int;
            args.push_back(coerce_arg(a, want));
        }
        B->CreateCall(fn, args);
    }
    return LVal{VType::Object, obj, nullptr, VType::Void, cls};
}

LVal Gen::gen_method_call(const LVal& obj, const std::string& name, const CallExpr* c) {
    auto pv = [this](const auto& p) -> VType {
        if (!p || !p->type) return VType::Int;
        return type_from_name(p->type->base_name);   // class 名 -> Object
    };
    std::string owner;
    const FunctionDefStmt* fd = find_method(obj.obj_class, name, &owner);
    if (!fd)
        throw std::runtime_error("class '" + obj.obj_class + "' has no method '" + name + "'");
    const std::string mname = owner + "." + name;
    llvm::Function* fn = mod_->getFunction(mname);
    if (!fn) throw std::runtime_error("method '" + mname + "' not generated");
    std::vector<llvm::Value*> args{obj.val};
    for (size_t i = 0; i < c->args.size(); ++i) {
        LVal a = gen_expr(c->args[i].get());
        VType want = (i + 1 < fd->params.size()) ? pv(fd->params[i + 1]) : VType::Int;
        args.push_back(coerce_arg(a, want));
    }
    VType rt = fn_ret_map_.count(mname) ? fn_ret_map_[mname] : VType::Int;
    return {rt, B->CreateCall(fn, args, "mcall")};
}

LVal Gen::gen_obj_field_read(const LVal& obj, const std::string& member) {
    llvm::Function* fn = llvm::cast<llvm::Function>(declare_runtime(
        "vor_obj_get", B->getInt64Ty(), {B->getPtrTy(), B->getPtrTy()}));
    return {VType::Int, B->CreateCall(fn, {obj.val, cstr(member)}, "fld")};
}

// ==================== operator 重载（静态分派） ====================
void Gen::collect_operators() {
    for (auto& st : prog_.stmts) {
        if (st->kind != StmtKind::OperatorDecl) continue;
        auto* od = static_cast<const OperatorDeclStmt*>(st.get());
        op_overloads_[op_key(od->op, canon_type_name(od->left_type),
                             canon_type_name(od->right_type), od->unary)] = od;
    }
}

// ---------- Lib-O3：外部 C 函数 ----------
// 预处理：扫描顶层 extdef 声明
void Gen::collect_extdefs() {
    for (auto& st : prog_.stmts) {
        if (st->kind != StmtKind::ExtDef) continue;
        auto* ed = static_cast<const ExtDefStmt*>(st.get());
        extdefs_[ed->name] = ed;
    }
}

// C 类型名 -> LLVM 类型；未知类型返回 nullptr
llvm::Type* Gen::c_abi_type(const std::string& n) const {
    if (n == "cint" || n == "cuint") return B->getInt32Ty();
    if (n == "clong" || n == "culong") return B->getInt64Ty();
    if (n == "cdouble") return B->getDoubleTy();
    if (n == "cstr" || n == "cptr") return B->getPtrTy();
    return nullptr;
}

// 发射 extdef 函数声明（外部链接；符号名原样，C ABI）
void Gen::gen_extdef(const ExtDefStmt* ed) {
    if (mod_->getFunction(ed->name)) return;
    llvm::Type* rt = c_abi_type(ed->ret_type);
    if (!rt)
        throw std::runtime_error("extdef '" + ed->name + "': unknown C type '" +
                                 ed->ret_type + "'");
    std::vector<llvm::Type*> pts;
    for (auto& [pt, pn] : ed->params) {
        llvm::Type* t = c_abi_type(pt);
        if (!t)
            throw std::runtime_error("extdef '" + ed->name + "': unknown C type '" + pt + "'");
        pts.push_back(t);
    }
    llvm::FunctionType* ft = llvm::FunctionType::get(rt, pts, false);
    llvm::Function::Create(ft, llvm::Function::ExternalLinkage, ed->name, mod_);
}

// 外部函数调用：把 Vortex 值转成 C ABI 实参，返回值再转回 Vortex 值
LVal Gen::gen_ext_call(const ExtDefStmt* ed, const CallExpr* c) {
    if (c->args.size() != ed->params.size())
        throw std::runtime_error("extdef '" + ed->name + "' expects " +
                                 std::to_string(ed->params.size()) + " args, got " +
                                 std::to_string(c->args.size()));
    llvm::Function* fn = mod_->getFunction(ed->name);
    if (!fn) { gen_extdef(ed); fn = mod_->getFunction(ed->name); }

    std::vector<llvm::Value*> cargs;
    for (size_t i = 0; i < c->args.size(); ++i) {
        const std::string& pt = ed->params[i].first;
        LVal a = gen_expr(c->args[i].get());
        llvm::Value* av = nullptr;
        if (pt == "cint" || pt == "cuint") {
            if (a.type == VType::Float)      av = B->CreateFPToSI(a.val, B->getInt32Ty(), "x.i32");
            else if (a.type == VType::Bool)  av = B->CreateZExt(a.val, B->getInt32Ty(), "x.i32");
            else if (a.type == VType::Str) { // str -> 数值（解析）
                llvm::Value* iv = B->CreateCall(llvm::cast<llvm::Function>(
                    decl_vor("vor_cast_i64", VType::Int, {VType::Str})), {a.val});
                av = B->CreateTrunc(iv, B->getInt32Ty(), "x.i32");
            } else av = B->CreateTrunc(a.val, B->getInt32Ty(), "x.i32");
        } else if (pt == "clong" || pt == "culong") {
            av = to_i64(a);
        } else if (pt == "cdouble") {
            if (a.type == VType::Float)     av = a.val;
            else if (a.type == VType::Bool) av = B->CreateUIToFP(a.val, B->getDoubleTy(), "x.f64");
            else if (a.type == VType::Str)
                av = B->CreateCall(llvm::cast<llvm::Function>(
                    decl_vor("vor_cast_f64", VType::Float, {VType::Str})), {a.val});
            else av = B->CreateSIToFP(to_i64(a), B->getDoubleTy(), "x.f64");
        } else if (pt == "cstr") {
            llvm::Value* vs = (a.type == VType::Str) ? a.val : to_vstr(a);
            av = B->CreateCall(llvm::cast<llvm::Function>(
                decl_vor("vor_str_cstr", VType::Str, {VType::Str})), {vs}, "x.cstr");
        } else { // cptr：不透明指针（整型按位转指针，指针类直接透传）
            if (a.type == VType::Object || a.type == VType::Str || a.type == VType::List ||
                a.type == VType::Dict || a.type == VType::Closure || a.type == VType::ExtFloat)
                av = a.val;
            else av = B->CreateIntToPtr(to_i64(a), B->getPtrTy(), "x.ptr");
        }
        cargs.push_back(av);
    }

    llvm::CallInst* call = B->CreateCall(fn, cargs, ed->name + ".call");
    const std::string& rt = ed->ret_type;
    if (rt == "cint")   return {VType::Int,   B->CreateSExt(call, B->getInt64Ty(), "x.sext")};
    if (rt == "cuint")  return {VType::Int,   B->CreateZExt(call, B->getInt64Ty(), "x.zext")};
    if (rt == "clong" || rt == "culong") return {VType::Int, call};
    if (rt == "cdouble") return {VType::Float, call};
    if (rt == "cstr")
        return {VType::Str, B->CreateCall(llvm::cast<llvm::Function>(
            decl_vor("vor_str_from_cstr", VType::Str, {VType::Str})), {call})};
    // cptr
    return {VType::Int, B->CreatePtrToInt(call, B->getInt64Ty(), "x.p2i")};
}

VType Gen::type_from_name(const std::string& n) const {
    if (n == "float" || n == "double") return VType::Float;
    if (n == "bool") return VType::Bool;
    if (n == "str" || n == "string" || n == "unistr") return VType::Str;
    if (n == "extfloat") return VType::ExtFloat;
    if (classes_.count(n)) return VType::Object;
    return VType::Int;
}

std::string Gen::lval_type_name(const LVal& v) const {
    if (!v.obj_class.empty()) return v.obj_class;
    switch (v.type) {
        case VType::Int:   return "int";
        case VType::Float: return "float";
        case VType::Bool:  return "bool";
        case VType::Str:   return "str";
        case VType::ExtFloat: return "extfloat";
        case VType::List:  return "list";
        case VType::Dict:  return "dict";
        default:           return "";
    }
}

std::string Gen::vtype_desc(VType t) {
    switch (t) {
        case VType::Int:    return "int";
        case VType::Float:  return "double";      // 对齐解释器 Value::type_name
        case VType::Bool:   return "bool";
        case VType::Str:    return "str";
        case VType::List:
        case VType::StrList:
        case VType::StrListList: return "list";
        case VType::Dict:   return "dict";
        case VType::Set:    return "set";
        case VType::Pair:   return "pair";
        case VType::Tuple:  return "tuple";
        case VType::Object: return "instance";
        case VType::ExtFloat: return "extfloat";
        case VType::Closure:return "function";
        case VType::Ref:    return "memadr";
        default:            return "None";
    }
}

// type() 的静态类型描述：字面量/元组/实例按源码精确给出，其余按静态类型
std::string Gen::type_desc_of_expr(const Expr* e) {
    if (!e) return "None";
    if (e->kind == ExprKind::Literal) {
        auto* l = static_cast<const LiteralExpr*>(e);
        switch (l->lit_kind) {
            case LiteralExpr::LitKind::Char:      return "char";
            case LiteralExpr::LitKind::Unichar:   return "unichar";
            case LiteralExpr::LitKind::Float:     return "double";
            case LiteralExpr::LitKind::ExtFloat:  return "extfloat";
            case LiteralExpr::LitKind::String:
            case LiteralExpr::LitKind::UniString: return "str";
            case LiteralExpr::LitKind::Bool:      return "bool";
            case LiteralExpr::LitKind::None:      return "None";
            default:                              return "int";
        }
    }
    if (e->kind == ExprKind::ListInit || e->kind == ExprKind::ListComp) return "list";
    if (e->kind == ExprKind::DictInit || e->kind == ExprKind::DictComp) return "dict";
    if (e->kind == ExprKind::Call) {
        auto* c = static_cast<const CallExpr*>(e);
        if (c->callee->kind == ExprKind::Identifier) {
            const std::string& cn = static_cast<const IdentifierExpr*>(c->callee.get())->name;
            // 元组字面量：(a, b, c) 被解析为 make_tuple(...)
            if (cn == "make_tuple") {
                std::string s = "auto_generated_class(";
                for (size_t i = 0; i < c->args.size(); ++i) {
                    if (i) s += ",";
                    s += type_desc_of_expr(c->args[i].get());
                }
                return s + ")";
            }
            if (cn == "__set_literal__" || cn == "set") return "set";
            if (cn == "make_pair" || cn == "pair") return "pair";
            if (classes_.count(cn)) return cn;   // 构造调用 -> 类名
        }
        return vtype_desc(infer_expr_type(e));
    }
    if (e->kind == ExprKind::Identifier) {
        LVal v;
        if (lookup_var(static_cast<const IdentifierExpr*>(e)->name, v)) {
            if (!v.type_desc.empty()) return v.type_desc;   // 元组等已记录的描述
            if (v.type == VType::Object)
                return v.obj_class.empty() ? std::string("instance") : v.obj_class;
            return vtype_desc(v.type);
        }
        return "None";
    }
    return vtype_desc(infer_expr_type(e));
}

std::string Gen::op_fn_name(const OperatorDeclStmt* od) const {
    return std::string("op$") + (od->unary ? "u" : "b") + "$" +
           (od->left_type.empty() ? "object" : od->left_type) + "$" +
           op_mnemonic(od->op) + "$" +
           (od->unary ? "x" : (od->right_type.empty() ? "object" : od->right_type));
}

const OperatorDeclStmt* Gen::find_operator_decl(const std::string& op, const std::string& lt,
                                                const std::string& rt, bool unary) const {
    auto probe = [&](const std::string& a, const std::string& b) -> const OperatorDeclStmt* {
        auto it = op_overloads_.find(op_key(op, canon_type_name(a), canon_type_name(b), unary));
        return it == op_overloads_.end() ? nullptr : it->second;
    };
    if (auto* od = probe(lt, rt)) return od;
    auto chain_of = [&](const std::string& cls) {
        std::vector<std::string> out;
        const ClassInfo* ci = find_class(cls);
        while (ci) {
            out.push_back(ci->name);
            ci = ci->parent.empty() ? nullptr : find_class(ci->parent);
        }
        return out;
    };
    auto lc = chain_of(lt);
    for (size_t i = 1; i < lc.size(); ++i)
        if (auto* od = probe(lc[i], rt)) return od;
    auto rc = chain_of(rt);
    for (size_t i = 1; i < rc.size(); ++i)
        if (auto* od = probe(lt, rc[i])) return od;
    return nullptr;
}

void Gen::gen_operator_fn(const OperatorDeclStmt* od) {
    const std::string fnname = op_fn_name(od);
    if (mod_->getFunction(fnname)) return;
    VType lt = type_from_name(od->left_type);
    VType rpt = od->unary ? VType::Void : type_from_name(od->right_type);
    VType ret = type_from_name(od->ret_type);
    std::vector<llvm::Type*> params{llvm_type(lt)};
    std::vector<VType> ptypes{lt};
    if (!od->unary) { params.push_back(llvm_type(rpt)); ptypes.push_back(rpt); }
    fn_ret_map_[fnname] = ret;
    llvm::FunctionType* FT = llvm::FunctionType::get(llvm_type(ret), params, false);
    llvm::Function* fn = llvm::Function::Create(FT, llvm::Function::ExternalLinkage, fnname, mod_);
    llvm::Function* saved = cur_fn_;
    cur_fn_ = fn;
    llvm::IRBuilderBase::InsertPointGuard guard(*B);
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(B->getContext(), "entry", fn);
    B->SetInsertPoint(entry);
    llvm::DISubprogram* saved_sp = cur_sp_;
    unsigned saved_dbg_line = dbg_line_;
    if (debug_on()) dbg_begin_func(fn, fnname.c_str());
    scopes_.emplace_back();
    bool saved_toplevel = top_level_;
    VType saved_ret = fn_ret_;
    top_level_ = false;
    const std::string pnames[2] = {od->left_name, od->right_name};
    const std::string ptypename[2] = {od->left_type, od->right_type};
    for (size_t i = 0; i < ptypes.size(); ++i) {
        llvm::Argument* arg = fn->arg_begin() + (unsigned)i;
        arg->setName(pnames[i]);
        llvm::AllocaInst* a = make_alloca(ptypes[i], fn, pnames[i].c_str());
        B->CreateStore(arg, a);
        LVal lv{ptypes[i], B->CreateLoad(llvm_type(ptypes[i]), a)};
        if (ptypes[i] == VType::Object) lv.obj_class = ptypename[i];
        scopes_.back()[pnames[i]] = lv;
        if (debug_on()) dbg_declare(pnames[i], ptypes[i], a);
    }
    fn_ret_ = ret;
    for (auto& st : od->body->stmts) {
        if (block_has_term(B->GetInsertBlock())) break;
        gen_stmt(st.get());
    }
    if (!block_has_term(B->GetInsertBlock()))
        B->CreateRet(llvm::Constant::getNullValue(llvm_type(ret)));
    scopes_.pop_back();
    cur_sp_ = saved_sp;
    dbg_line_ = saved_dbg_line;
    top_level_ = saved_toplevel;
    fn_ret_ = saved_ret;
    cur_fn_ = saved;
}

LVal Gen::call_operator_fn(const OperatorDeclStmt* od, const LVal& l, const LVal& r) {
    const std::string fnname = op_fn_name(od);
    llvm::Function* fn = mod_->getFunction(fnname);
    if (!fn) throw std::runtime_error("operator '" + od->op + "' (" + fnname + ") not generated");
    std::vector<llvm::Value*> args;
    args.push_back(coerce_arg(l, type_from_name(od->left_type)));
    if (!od->unary) args.push_back(coerce_arg(r, type_from_name(od->right_type)));
    VType rt = fn_ret_map_.count(fnname) ? fn_ret_map_[fnname] : VType::Int;
    LVal out{rt, B->CreateCall(fn, args, "opcall")};
    if (rt == VType::Object) out.obj_class = od->ret_type;
    return out;
}

// ==================== auto 解构绑定 ====================
void Gen::gen_destructure(const VarDeclStmt* vd) {
    LVal src = gen_expr(vd->unpack_init.get());
    const size_t n = vd->names.size();
    // 绑定一个名字（顶层提升为模块全局，局部用 alloca）；元素统一按 int 处理
    auto bind = [&](size_t i, llvm::Value* v) {
        const std::string& nm = vd->names[i].first;
        if (top_level_) {
            auto* gv = new llvm::GlobalVariable(*mod_, B->getInt64Ty(), false,
                                                llvm::GlobalValue::InternalLinkage,
                                                B->getInt64(0), "g_" + nm);
            B->CreateStore(v, gv);
            scopes_.back()[nm] = LVal{VType::Int, B->CreateLoad(B->getInt64Ty(), gv), gv};
        } else {
            llvm::AllocaInst* a = make_alloca(VType::Int, cur_bb()->getParent(), nm.c_str());
            B->CreateStore(v, a);
            if (debug_on()) dbg_declare(nm, VType::Int, a);
            scopes_.back()[nm] = LVal{VType::Int, B->CreateLoad(B->getInt64Ty(), a), a};
        }
    };
    if (src.type == VType::Object) {
        // 实例：字段沿父链由根到子展开，不展开方法
        std::vector<const ClassInfo*> chain;   // 子 → 父
        for (const ClassInfo* ci = find_class(src.obj_class); ci;
             ci = ci->parent.empty() ? nullptr : find_class(ci->parent))
            chain.push_back(ci);
        std::vector<std::string> fields;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            for (auto& f : (*it)->fields) fields.push_back(f.second);
        if (fields.size() != n)
            throw std::runtime_error("cannot unpack " + std::to_string(fields.size()) +
                                     " field(s) of class '" + src.obj_class + "' into " +
                                     std::to_string(n) + " name(s)");
        for (size_t i = 0; i < n; ++i) {
            llvm::Value* v = B->CreateCall(llvm::cast<llvm::Function>(
                declare_runtime("vor_obj_get", B->getInt64Ty(), {B->getPtrTy(), B->getPtrTy()})),
                {src.val, cstr(fields[i])}, "ufld");
            bind(i, v);
        }
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        llvm::Value* v = nullptr;
        if (src.type == VType::Tuple) {
            v = B->CreateCall(llvm::cast<llvm::Function>(
                decl_vor("vor_tuple_at", VType::Int, {VType::Tuple, VType::Int})),
                {src.val, B->getInt64((long long)i)}, "utup");
        } else if (src.type == VType::List || src.type == VType::StrList ||
                   src.type == VType::StrListList) {
            v = B->CreateCall(llvm::cast<llvm::Function>(
                decl_vor("vor_list_get", VType::Int, {VType::List, VType::Int})),
                {src.val, B->getInt64((long long)i)}, "ulst");
        } else if (src.type == VType::Pair && i < 2) {
            v = B->CreateCall(llvm::cast<llvm::Function>(
                decl_vor(i == 0 ? "vor_pair_first" : "vor_pair_second",
                         VType::Int, {VType::Pair})), {src.val}, "upair");
        } else {
            throw std::runtime_error("cannot unpack a value of this type into " +
                                     std::to_string(n) + " name(s)");
        }
        bind(i, v);
    }
}

// ==================== 顶层：模块生成 ====================
std::string Gen::run(llvm::Module& mod) {
    mod_ = &mod;
    B = new llvm::IRBuilder<>(mod.getContext());
    (void)cfg_;
    if (debug_on()) dbg_begin(mod);

    // main() -> i32  （参数：int argc, char** argv）
    llvm::FunctionType* mainFT = llvm::FunctionType::get(B->getInt32Ty(), {B->getInt32Ty(), B->getPtrTy()}, false);
    llvm::Function* mainF = llvm::Function::Create(mainFT, llvm::Function::ExternalLinkage, "main", mod_);
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(mod.getContext(), "entry", mainF);
    B->SetInsertPoint(entry);
    if (debug_on()) dbg_begin_func(mainF, "main");

    // 把命令行参数交给运行时（供 os.argc / os.arg 使用）
    llvm::Function* setArgsFn = llvm::cast<llvm::Function>(
        declare_runtime("vor_set_args", B->getVoidTy(), {B->getInt32Ty(), B->getPtrTy()}));
    B->CreateCall(setArgsFn, {mainF->getArg(0), mainF->getArg(1)});

    // 全局作用域
    top_level_ = true;
    scopes_.emplace_back();
    collect_classes();   // 先扫描 class 声明，供 VarDecl / 构造 / 方法调用解析
    collect_operators(); // 再扫描顶层 operator 声明，供二元/一元运算静态分派
    collect_extdefs();   // 再扫描顶层 extdef 声明，供外部 C 函数调用解析

    // 遍历顶层语句（P1：顺序生成，函数定义内置在 gen_stmt 中）

    for (size_t si = 0; si < prog_.stmts.size() && B->GetInsertBlock(); ++si) {
        try { gen_stmt(prog_.stmts[si].get()); }
        catch (const std::exception& e) { return std::string("[codegen] ") + e.what(); }
    }

    scopes_.pop_back();
    llvm::BasicBlock* curBB = B->GetInsertBlock();
    if (curBB && !block_has_term(curBB)) B->CreateRet(B->getInt32(0));

    if (debug_on()) dbg_end();
    delete B; B = nullptr;
    return {};
}

// ==================== build_vortex_exe：编译为 exe ====================
static bool lex_parse(const std::string& src, std::string& err, Program& prog) {
    Lexer lex(src);
    auto toks = lex.tokenize();
    if (!lex.errors().empty()) { err = "[Lexer] " + lex.errors()[0]; return false; }
    Parser parser(std::move(toks));
    try {
        auto p = parser.parse_program();
        if (!p) { err = "[Parse] null program"; return false; }
        prog = std::move(*p);
    } catch (const std::runtime_error& e) {
        err = "[Parse] " + std::string(e.what());
        return false;
    }
    if (!parser.errors().empty()) { err = "[Parse] " + parser.errors()[0]; return false; }
    return true;
}

} // namespace cg

// ==================== Lib-O3：DLL 导入库辅助 ====================
// MinGW 工具目录（gendef/dlltool 所在）
static std::string gcc_tools_dir() {
    if (const char* gp = std::getenv("VORTEX_GCC_DIR")) return std::string(gp) + "/bin";
    return "D:/gcc/bin";
}

// 解析 extdef 的 from 库路径：绝对路径原样；相对路径先按"源文件目录"再按 CWD
static std::string resolve_lib_path(const std::string& lib, const std::string& source_path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p(lib);
    if (p.is_absolute()) return lib;
    if (!source_path.empty()) {
        fs::path cand = fs::path(source_path).parent_path() / p;
        if (fs::exists(cand, ec)) return cand.lexically_normal().string();
    }
    if (fs::exists(p, ec)) return p.string();
    return lib;
}

// 为 DLL 生成 MinGW 导入库(.a)：gendef -> dlltool。返回 .a 路径（空=失败）
static std::string make_import_lib(const std::string& dllPath, std::string& err) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(dllPath, ec)) { err = "library not found: " + dllPath; return {}; }
    if (fs::path(dllPath).extension().string() != ".dll") {
        // 非 DLL 的直接作为链接输入（.a/.o/.lib）
        return dllPath;
    }
    const std::string base = fs::path(dllPath).stem().string();
    fs::path tmp = fs::temp_directory_path(ec) /
                   ("vortex_imp_" + std::to_string(std::hash<std::string>{}(dllPath)));
    fs::create_directories(tmp, ec);
    const std::string defN  = base + ".def";
    const std::string aFile = (tmp / ("lib" + base + ".a")).string();
    const std::string tools = gcc_tools_dir();
    const std::string dllAbs = fs::absolute(dllPath, ec).string();

    std::string gen = "cd /d \"" + tmp.string() + "\" && \"" + tools + "/gendef.exe\" \"" +
                      dllAbs + "\" >NUL 2>&1";
    if (std::system(gen.c_str()) != 0) { err = "gendef failed for " + dllPath; return {}; }

    std::string mk = "cd /d \"" + tmp.string() + "\" && \"" + tools + "/dlltool.exe\" -d \"" +
                     defN + "\" -l \"" + aFile + "\" -D \"" + base + ".dll\" >NUL 2>&1";
    if (std::system(mk.c_str()) != 0) { err = "dlltool failed for " + dllPath; return {}; }
    if (!fs::exists(aFile, ec)) { err = "import library not produced for " + dllPath; return {}; }
    return aFile;
}

// 编译 .vt 源码 -> LLVM 模块 IR（text）；返回错误串
std::string cg::compile_program(const Program& prog, std::string& ir_text, const BuildConfig& cfg) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::LLVMContext ctx;
    auto mod = std::make_unique<llvm::Module>("vortex", ctx);
    Gen gen(prog, cfg);
    std::string err = gen.run(*mod);
    if (!err.empty()) return err;
    std::string s;
    llvm::raw_string_ostream os(s);
    mod->print(os, nullptr);
    ir_text = os.str();
    return {};
}

// 顶层：编译 .vt 源码 -> 独立 exe
std::string build_vortex_exe(const std::string& src, const BuildConfig& cfg, const std::string& source_path) {
    // 1. 解析（含用户模块合并）
    Program prog;
    std::string err;
    if (!cg::collect_modules(src, source_path, prog, err)) return err;

    // 2. 生成 LLVM 模块
    llvm::LLVMContext ctx;
    auto mod = std::make_unique<llvm::Module>("vortex", ctx);
    cg::Gen gen(prog, cfg);
    try {
        err = gen.run(*mod);
    } catch (const std::exception& e) {
        return std::string("[Gen-exception] ") + e.what();
    }
    if (!err.empty()) return err;

    // 3. 校验（无论成败都保留 IR 到 .ll 便于诊断）
    std::string irTxt; llvm::raw_string_ostream rio(irTxt);
    mod->print(rio, nullptr);
    { std::ofstream of(cfg.output + ".ll"); of << rio.str(); }
    std::string verr;
    llvm::raw_string_ostream vos(verr);
    if (llvm::verifyModule(*mod, &vos))
        return "[Verifier] " + vos.str();

    // 4. 生成目标文件并用 g++ 链接
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();

    std::string triple = llvm::sys::getDefaultTargetTriple();
    std::string error;
#if LLVM_VERSION_MAJOR >= 22
    llvm::Triple tripleArg(triple);         // 新版本接口收 const llvm::Triple&
#else
    const std::string& tripleArg = triple;  // 旧版本接口收 StringRef
#endif
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(tripleArg, error);
    if (!target) return "[Target] " + error;

    auto RL = std::make_unique<llvm::TargetOptions>();
    llvm::Reloc::Model RM = llvm::Reloc::PIC_;
#if LLVM_VERSION_MAJOR >= 18
    llvm::CodeGenOptLevel optLevel = llvm::CodeGenOptLevel::Default;
#else
    llvm::CodeGenOpt::Level optLevel = llvm::CodeGenOpt::Default;
#endif
    std::unique_ptr<llvm::TargetMachine> tm(
        target->createTargetMachine(tripleArg, "generic", "", *RL, RM,
                                    std::nullopt, optLevel));
    mod->setDataLayout(tm->createDataLayout());

    // 目标文件
    std::string objPath = cfg.output + ".o";
    std::error_code ec;
    llvm::raw_fd_ostream out(objPath, ec, llvm::sys::fs::OF_None);
    if (ec) return "[Object] " + ec.message();

    {
        llvm::legacy::PassManager pass;
#if LLVM_VERSION_MAJOR >= 18
        llvm::CodeGenFileType fileType = llvm::CodeGenFileType::ObjectFile;
#else
        llvm::CodeGenFileType fileType = llvm::CGFT_ObjectFile;
#endif
        bool fail = tm->addPassesToEmitFile(pass, out, nullptr, fileType);
        if (fail) return "[Object] addPassesToEmitFile failed";
        pass.run(*mod);
    }
    out.close();

    // 4b. 收集 extdef 依赖库（Lib-O3）：from "..." 指定的 DLL -> 生成 MinGW 导入库并加入链接
    std::string extraArgs;
    {
        std::set<std::string> seenLibs;
        auto add_lib = [&](const std::string& lib, std::string& e2) -> bool {
            if (lib.empty() || !seenLibs.insert(lib).second) return true;
            std::string ierr;
            std::string a = make_import_lib(resolve_lib_path(lib, source_path), ierr);
            if (a.empty()) { e2 = ierr; return false; }
            extraArgs += " \"" + a + "\"";
            return true;
        };
        for (auto& st : prog.stmts) {
            if (st->kind != StmtKind::ExtDef) continue;
            auto* ed = static_cast<const ExtDefStmt*>(st.get());
            std::string le;
            if (!add_lib(ed->lib, le)) return "[Link] " + le;
        }
        for (auto& f : cfg.link_files) {
            std::string ierr;
            std::string a = make_import_lib(resolve_lib_path(f, source_path), ierr);
            if (a.empty()) return "[Link] " + ierr;
            extraArgs += " \"" + a + "\"";
        }
    }

    // 5. 链接：g++ obj + runtime -> exe
    std::string rtLib;
#ifdef VORTEX_RT_LIB
    rtLib = VORTEX_RT_LIB;
#else
    if (const char* rt = std::getenv("VORTEX_RT")) rtLib = rt;
    else rtLib = "libvortex_runtime.a";
#endif
    std::string cmd;
#ifndef VORTEX_DEFAULT_GCC_DIR
#define VORTEX_DEFAULT_GCC_DIR "D:/gcc"
#endif
#ifdef VORTEX_LLD
    // ---------- 用 LLVM lld 链接（生成 exe 不依赖外部 g++） ----------
    // 仍需要 MinGW 的 C++ 运行库/启动对象（runtime 是 C++）。其目录默认取
    // 环境变量 VORTEX_GCC_DIR，否则用编译时注入的默认路径。
    const char* gp = std::getenv("VORTEX_GCC_DIR");
    std::string gccDir = gp ? gp : VORTEX_DEFAULT_GCC_DIR;
    cmd = "\"" + std::string(VORTEX_LLD) + "\" \"" + objPath + "\" \"" + rtLib +
          "\"" + extraArgs + " -o \"" + cfg.output + "\" -static -L\"" + gccDir + "/lib\" \"" +
          gccDir + "/lib/crt2.o" +
          "\" -lmingw32 -lgcc -lstdc++ -lmoldname -lmingwex -lmsvcrt" +
          " -ladvapi32 -lshell32 -luser32 -lkernel32 -limagehlp -lws2_32" +
          " -lz -lsqlite3 -lwinhttp -L\"" + gccDir + "/opt/lib\"";
    if (cfg.debug) cmd += " -g";
    int rc = std::system(cmd.c_str());
    if (rc != 0) return "[Link] lld failed (code " + std::to_string(rc) + ")";
#else
    cmd = "g++ \"" + objPath + "\" \"" + rtLib +
          "\"" + extraArgs + " -o \"" + cfg.output + "\" -static -static-libgcc -static-libstdc++ -pthread -lz -lsqlite3 -lwinhttp -LD:/gcc/opt/lib";
    if (cfg.debug) cmd += " -g";
    int rc = std::system(cmd.c_str());
    if (rc != 0) return "[Link] g++ failed (code " + std::to_string(rc) + ")";
#endif
    return {};
}

} // namespace vortex
