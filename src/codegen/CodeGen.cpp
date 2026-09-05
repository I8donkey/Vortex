// ============================================================
// CodeGen.cpp — vortex LLVM 后端（P1 子集）
//
// 支持：int/float/bool/str 字面量与变量、算术/比较/逻辑运算、
//       if/while、用户 def 函数（int/double 参数）、print、math 初值。
// 标量直通原生寄存器；字符串经 VStr*（引用计数运行时）收发。
// ============================================================
#include "codegen/CodeGen.h"

#include "lexer.h"
#include "parser.h"
#include "interpreter.h"

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
#include <llvm/TargetParser/Host.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>

#include <cstdio>
#include <memory>
#include <fstream>
#include <string>
#include <vector>
#include <optional>
#include <set>
#include <algorithm>

namespace vortex {
namespace cg {

// 可靠地判断基本块是否已含终止指令（遍历，避免 getTerminator 在空块上的不可靠行为）
static bool block_has_term(llvm::BasicBlock* bb) {
    if (!bb) return false;
    for (auto& I : *bb)
        if (I.isTerminator()) return true;
    return false;
}

// ==================== 静态类型 ====================
enum class VType { Int, Float, Bool, Str, List, StrList, Dict, Set, Pair, Tuple, Ref, Closure, Void };

struct LVal {
    VType type;
    llvm::Value* val = nullptr;     // 值 (标量) 或指针 (Str)
    llvm::Value* slot = nullptr;    // 变量槽（alloca 或模块全局，用于读写）
    VType ref_pointee = VType::Void;  // type==Ref 时的被引用变量类型（Void 表未知/非引用）
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
    llvm::Function* cur_fn_ = nullptr;
    // lambda 序列号（生成唯一匿名函数名）
    int lam_seq_ = 0;
    // 内建函数表
    std::unordered_map<std::string, std::pair<VType, Builtin>> fns_;

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

    // 内置函数调用
    LVal gen_call(const CallExpr* c);
    // 输出一个值到 stdout
    void emit_any(LVal v);
    // 把值规整为 i64 槽位（布尔零扩展、浮点按位拷贝）
    llvm::Value* to_i64(LVal v);
    // 用户函数推断返回类型（P1 简化：返回最后 return 的类型/当前记录）
    VType fn_ret_of(const std::string& fnname);

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
        case VType::Dict:  return B->getPtrTy();          // VDict*
        case VType::Set:   return B->getPtrTy();          // VList* (集合复用)
        case VType::Pair:  return B->getPtrTy();          // VPair*
        case VType::Tuple: return B->getPtrTy();          // VTuple*
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
            if (lookup_var(static_cast<const IdentifierExpr*>(e)->name, v)) {
                // 有 alloca 槽则每次重新 load，保证读到最新值
                if (v.slot) return {v.type, B->CreateLoad(llvm_type(v.type), v.slot)};
                return v;
            }
            break;
        }
        case ExprKind::Cast: {
            auto* ce = static_cast<const CastExpr*>(e);
            std::string t = ce->target_type;
            LVal sv = gen_expr(ce->value.get());
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
            if (u->op == "!") return {VType::Bool, B->CreateNot(v.val)};
            if (u->op == "-") {
                if (v.type == VType::Int)  return {VType::Int,   B->CreateNeg(v.val)};
                if (v.type == VType::Float)return {VType::Float, B->CreateFNeg(v.val)};
            }
            break;
        }
        case ExprKind::AssignOp: {
            auto* a = static_cast<const AssignOpExpr*>(e);
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
            // pair.first / pair.second（作为值表达式，非调用）
            auto* m = static_cast<const MemberAccessExpr*>(e);
            LVal obj = gen_expr(m->object.get());
            if (obj.type == VType::Pair && m->member == "first") {
                llvm::Value* fn = decl_vor("vor_pair_first", VType::Int, {VType::Pair});
                return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {obj.val})};
            }
            if (obj.type == VType::Pair && m->member == "second") {
                llvm::Value* fn = decl_vor("vor_pair_second", VType::Int, {VType::Pair});
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

    // 算术
    llvm::Value* res = nullptr;
    if (op == "+") {
        if ((l.type == VType::Str) != (r.type == VType::Str)) {
            // 一侧为 str、另一侧非 str：与解释器一致抛异常
            llvm::Value* msg = B->CreateGlobalString("Type 'str' is not numeric", "ncat");
            B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_throw_str", VType::Void, {VType::Str})),
                          {B->CreateCall(llvm::cast<llvm::Function>(decl_vor("vor_str_from_cstr", VType::Str, {VType::Str})), {msg})});
            return {VType::Int, B->getInt64(0)};
        }
        llvm::Value* pr[2] = {decl_vor("vor_str_concat", VType::Str, {VType::Str, VType::Str}),
                              nullptr};
        if (l.type == VType::Str && r.type == VType::Str) {
            res = B->CreateCall(llvm::cast<llvm::Function>(pr[0]), {l.val, r.val});
            return {VType::Str, res};
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
        res = B->CreateSDiv(l.val, r.val, "idiv");
    }
    if (op == "%") {
        if (isFloat) { res = B->CreateFRem(lf, rf, "frem"); }
        else res = B->CreateSRem(l.val, r.val, "rem");
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

// ---------- 函数调用 ----------
LVal Gen::gen_call(const CallExpr* c) {
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
        if (obj.type == VType::List && (m->member == "len" || m->member == "length")) {
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
    // 目标 Function
    llvm::Function* callee = mod_->getFunction(fname);
    if (callee) {
        llvm::Value* r = B->CreateCall(callee, args, "call");
        return {fn_ret_of(fname), r};
    }
    // 未转发扩展模块调用（如 render3d./game2d. 等 P4 按需裁减项）：明确报错而非崩溃
    if (fname.find('.') != std::string::npos) {
        throw std::runtime_error("编译错误：不支持的模块调用 '" + fname +
                                 "'（编译器尚未转发该扩展模块，请改用解释器运行）");
    }
    return {VType::Void, nullptr};
}

VType Gen::fn_ret_of(const std::string&) { return fn_ret_; }

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
                else t = VType::Int;
            }
            for (auto& [nm, init] : vd->names) {
                LVal sv = init ? gen_expr(init.get()) : LVal{t, llvm::Constant::getNullValue(llvm_type(t)), nullptr};
                // 动态绑定：容器/字符串按初值实际类型（list 声明等）
                VType tEff = t;
                if (init && (sv.type == VType::List || sv.type == VType::Str || sv.type == VType::Dict
                             || sv.type == VType::StrList || sv.type == VType::Set || sv.type == VType::Pair
                             || sv.type == VType::Tuple || sv.type == VType::Closure)) tEff = sv.type;
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
                scopes_.back()[nm] = LVal{tEff, B->CreateLoad(llvm_type(tEff), slot), slot};
            }
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
                && cont.type != VType::StrList) break;
            const bool isDict = (cont.type == VType::Dict);
            const bool isTuple = (cont.type == VType::Tuple);
            const bool isStrList = (cont.type == VType::StrList);

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
            const VType elType = isStrList ? VType::Str : VType::Int;
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
        case StmtKind::Return:
            gen_return(static_cast<const ReturnStmt*>(s));
            break;
        case StmtKind::FunctionDef: {
            auto* fd = static_cast<const FunctionDefStmt*>(s);
            // 函数原型：返回类型默认 i64，参数按类型
            std::vector<llvm::Type*> params;
            std::vector<VType> ptypes;
            for (auto& p : fd->params) {
                VType t = VType::Int;
                if (p->type) {
                    if (p->type->base_name == "float") t = VType::Float;
                    else if (p->type->base_name == "bool") t = VType::Bool;
                    else if (p->type->base_name == "str") t = VType::Str;
                }
                params.push_back(llvm_type(t));
                ptypes.push_back(t);
            }
            llvm::FunctionType* FT = llvm::FunctionType::get(llvm_type(VType::Int), params, false);
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
                llvm::Argument* arg = fn->arg_begin() + i;
                arg->setName(p->name);
                llvm::AllocaInst* a = make_alloca(ptypes[i], fn, p->name.c_str());
                B->CreateStore(arg, a);
                scopes_.back()[p->name] = {ptypes[i], B->CreateLoad(llvm_type(ptypes[i]), a)};
                if (debug_on()) dbg_declare(p->name, ptypes[i], a);
                ++i;
            }
            for (auto& st : fd->body->stmts) {
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
std::string build_vortex_exe(const std::string& src, const BuildConfig& cfg) {
    // 1. 解析
    Program prog;
    std::string err;
    if (!cg::lex_parse(src, err, prog)) return err;

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
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(llvm::Triple(triple), error);
    if (!target) return "[Target] " + error;

    auto RL = std::make_unique<llvm::TargetOptions>();
    llvm::Reloc::Model RM = llvm::Reloc::PIC_;
    llvm::Triple tripleObj(triple);
    std::unique_ptr<llvm::TargetMachine> tm(
        target->createTargetMachine(tripleObj, "generic", "", *RL, RM,
                                    std::nullopt, llvm::CodeGenOptLevel::Default));
    mod->setDataLayout(tm->createDataLayout());

    // 目标文件
    std::string objPath = cfg.output + ".o";
    std::error_code ec;
    llvm::raw_fd_ostream out(objPath, ec, llvm::sys::fs::OF_None);
    if (ec) return "[Object] " + ec.message();

    {
        llvm::legacy::PassManager pass;
        bool fail = tm->addPassesToEmitFile(pass, out, nullptr, llvm::CodeGenFileType::ObjectFile);
        if (fail) return "[Object] addPassesToEmitFile failed";
        pass.run(*mod);
    }
    out.close();

    // 5. 链接：g++ obj + runtime -> exe
    std::string rtLib;
#ifdef VORTEX_RT_LIB
    rtLib = VORTEX_RT_LIB;
#else
    if (const char* rt = std::getenv("VORTEX_RT")) rtLib = rt;
    else rtLib = "libvortex_runtime.a";
#endif
    std::string cmd = "g++ \"" + objPath + "\" \"" + rtLib +
                      "\" -o \"" + cfg.output + "\" -static -static-libgcc -static-libstdc++ -pthread -lz -lsqlite3 -lwinhttp -LD:/gcc/opt/lib";
    if (cfg.debug) cmd += " -g";
    int rc = std::system(cmd.c_str());
    if (rc != 0) return "[Link] g++ failed (code " + std::to_string(rc) + ")";
    return {};
}

} // namespace vortex
