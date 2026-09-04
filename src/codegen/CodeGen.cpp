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

#include <cstdio>
#include <memory>
#include <fstream>
#include <string>
#include <vector>
#include <optional>

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
enum class VType { Int, Float, Bool, Str, List, Dict, Set, Pair, Tuple, Ref, Void };

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
    // 内建函数表
    std::unordered_map<std::string, std::pair<VType, Builtin>> fns_;

    // 运行时函数声明缓存
    llvm::Function* declare_runtime(const char* name, llvm::Type* ret, std::vector<llvm::Type*> params, bool vararg=false);
    llvm::Value* decl_vor(const char* name, VType ret, std::vector<VType> params);

    // 表达式/语句生成
    LVal gen_expr(const Expr* e);
    void gen_stmt(const Stmt* s);
    void gen_block(const std::vector<StmtPtr>& body);
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
        case VType::Dict:  return B->getPtrTy();          // VDict*
        case VType::Set:   return B->getPtrTy();          // VList* (集合复用)
        case VType::Pair:  return B->getPtrTy();          // VPair*
        case VType::Tuple: return B->getPtrTy();          // VTuple*
        case VType::Void:  return B->getVoidTy();
    }
    return B->getVoidTy();
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

    // len(String) / len(List) / len(Dict)：转发运行时长度
    if (fname == "len") {
        LVal v = gen_expr(c->args[0].get());
        if (v.type == VType::Str) {
            llvm::Value* fn = decl_vor("vor_str_len", VType::Int, {VType::Str});
            return {VType::Int, B->CreateCall(llvm::cast<llvm::Function>(fn), {v.val})};
        }
        if (v.type == VType::List) {
            llvm::Value* fn = decl_vor("vor_list_len", VType::Int, {VType::List});
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
    // 目标 Function
    llvm::Function* callee = mod_->getFunction(fname);
    if (callee) {
        llvm::Value* r = B->CreateCall(callee, args, "call");
        return {fn_ret_of(fname), r};
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

void Gen::gen_stmt(const Stmt* s) {
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
                             || sv.type == VType::Set || sv.type == VType::Pair || sv.type == VType::Tuple)) tEff = sv.type;
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
                && cont.type != VType::Set && cont.type != VType::Tuple) break;
            const bool isDict = (cont.type == VType::Dict);
            const bool isTuple = (cont.type == VType::Tuple);

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

            // 主题：把元素 bind 到 var（仅 int 元素，作为局部变量 alloca）
            B->SetInsertPoint(bodyBB);
            llvm::AllocaInst* varSlot = make_alloca(VType::Int, F, fi->var.c_str());
            llvm::Value* idxv = B->CreateLoad(B->getInt64Ty(), idxSlot);
            llvm::Value* elem = isDict
                ? B->CreateCall(llvm::cast<llvm::Function>(
                    decl_vor("vor_dict_int_key_at", VType::Int, {VType::Dict, VType::Int})), {cont.val, idxv})
                : (isTuple
                   ? B->CreateCall(llvm::cast<llvm::Function>(
                       decl_vor("vor_tuple_at", VType::Int, {VType::Tuple, VType::Int})), {cont.val, idxv})
                   : B->CreateCall(llvm::cast<llvm::Function>(
                       decl_vor("vor_list_get", VType::Int, {VType::List, VType::Int})), {cont.val, idxv}));
            B->CreateStore(elem, varSlot);
            scopes_.emplace_back();
            scopes_.back()[fi->var] = LVal{VType::Int, B->CreateLoad(B->getInt64Ty(), varSlot), varSlot};
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
                ++i;
            }
            for (auto& st : fd->body->stmts) {
                if (block_has_term(B->GetInsertBlock())) break;
                gen_stmt(st.get());
            }
            if (!block_has_term(B->GetInsertBlock())) B->CreateRet(B->getInt64(0));
            scopes_.pop_back();
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

    // main() -> i32
    llvm::FunctionType* mainFT = llvm::FunctionType::get(B->getInt32Ty(), {B->getPtrTy(), B->getPtrTy()}, false);
    llvm::Function* mainF = llvm::Function::Create(mainFT, llvm::Function::ExternalLinkage, "main", mod_);
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(mod.getContext(), "entry", mainF);
    B->SetInsertPoint(entry);

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
                      "\" -o \"" + cfg.output + "\" -static -static-libgcc -static-libstdc++ -pthread";
    int rc = std::system(cmd.c_str());
    if (rc != 0) return "[Link] g++ failed (code " + std::to_string(rc) + ")";
    return {};
}

} // namespace vortex