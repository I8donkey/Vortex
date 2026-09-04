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
enum class VType { Int, Float, Bool, Str, List, Dict, Set, Pair, Tuple, Void };

struct LVal {
    VType type;
    llvm::Value* val = nullptr;     // 值 (标量) 或指针 (Str)
    llvm::AllocaInst* slot = nullptr; // 变量槽（用于读写）
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
        case ExprKind::BinaryOp:
            return gen_binop(static_cast<const BinaryOpExpr*>(e));
        case ExprKind::UnaryOp: {
            auto* u = static_cast<const UnaryOpExpr*>(e);
            LVal v = gen_expr(u->operand.get());
            if (u->op == "!") return {VType::Bool, B->CreateNot(v.val)};
            if (u->op == "-") {
                if (v.type == VType::Int)  return {VType::Int,   B->CreateNeg(v.val)};
                if (v.type == VType::Float)return {VType::Float, B->CreateFNeg(v.val)};
            }
            if (u->op == "@") v = v; // @x 取地址 P1 不支持真实引用
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
            LVal target;
            if (a->target->kind != ExprKind::Identifier) break;
            std::string nm = static_cast<const IdentifierExpr*>(a->target.get())->name;
            if (!lookup_var(nm, target) || !target.slot) break;

            llvm::Value* rhs = gen_expr(a->value.get()).val;   // RHS（内联对旧值的引用在本 store 前求值）
            bool isFloat = (target.type == VType::Float);
            llvm::Value* toStore = rhs;

            if (a->op == "=") {
                toStore = rhs;
            } else {
                llvm::Value* old = B->CreateLoad(llvm_type(target.type), target.slot);
                if (a->op == "+=") toStore = isFloat ? B->CreateFAdd(old, rhs) : B->CreateAdd(old, rhs);
                else if (a->op == "-=") toStore = isFloat ? B->CreateFSub(old, rhs) : B->CreateSub(old, rhs);
                else if (a->op == "*=") toStore = isFloat ? B->CreateFMul(old, rhs) : B->CreateMul(old, rhs);
                else if (a->op == "/=") toStore = isFloat ? B->CreateFDiv(old, rhs) : B->CreateSDiv(old, rhs);
                else toStore = rhs;
            }
            B->CreateStore(toStore, target.slot);
            return {target.type, toStore};
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

    // 逻辑短路：&& || 用基本块
    if (op == "&&") {
        llvm::Function* F = cur_bb()->getParent();
        llvm::BasicBlock* rhs = llvm::BasicBlock::Create(B->getContext(), "and.rhs", F);
        llvm::BasicBlock* merge = llvm::BasicBlock::Create(B->getContext(), "and.merge", F);
        llvm::Value* lv = B->CreateTrunc(l.val, B->getInt1Ty(), "and.l");
        B->CreateCondBr(lv, rhs, merge);
        B->SetInsertPoint(rhs);
        llvm::Value* rv = B->CreateTrunc(r.val, B->getInt1Ty(), "and.r");
        B->CreateBr(merge);
        B->SetInsertPoint(merge);
        llvm::PHINode* phi = B->CreatePHI(B->getInt1Ty(), 2, "and");
        phi->addIncoming(B->getFalse(), cur_bb());
        phi->addIncoming(rv, rhs);
        return {VType::Bool, phi};
    }
    if (op == "||") {
        llvm::Function* F = cur_bb()->getParent();
        llvm::BasicBlock* rhs = llvm::BasicBlock::Create(B->getContext(), "or.rhs", F);
        llvm::BasicBlock* merge = llvm::BasicBlock::Create(B->getContext(), "or.merge", F);
        llvm::Value* lv = B->CreateTrunc(l.val, B->getInt1Ty(), "or.l");
        B->CreateCondBr(lv, merge, rhs);
        B->SetInsertPoint(rhs);
        llvm::Value* rv = B->CreateTrunc(r.val, B->getInt1Ty(), "or.r");
        B->CreateBr(merge);
        B->SetInsertPoint(merge);
        llvm::PHINode* phi = B->CreatePHI(B->getInt1Ty(), 2, "or");
        phi->addIncoming(B->getTrue(), cur_bb());
        phi->addIncoming(rv, rhs);
        return {VType::Bool, phi};
    }

    // 算术
    llvm::Value* res = nullptr;
    if (op == "+") {
        llvm::Value* pr[2] = {decl_vor("vor_str_concat", VType::Str, {VType::Str, VType::Str}),
                              nullptr};
        if (l.type == VType::Str && r.type == VType::Str) {
            res = B->CreateCall(llvm::cast<llvm::Function>(pr[0]), {l.val, r.val});
            return {VType::Str, res};
        }
        res = isFloat ? B->CreateFAdd(l.val, r.val, "add") : B->CreateAdd(l.val, r.val, "add");
        return {isFloat ? VType::Float : VType::Int, res};
    }
    if (op == "-") res = isFloat ? B->CreateFSub(l.val, r.val, "sub") : B->CreateSub(l.val, r.val, "sub");
    if (op == "*") res = isFloat ? B->CreateFMul(l.val, r.val, "mul") : B->CreateMul(l.val, r.val, "mul");
    if (op == "/") { // vortex 的 "/" 为浮点除法（与解释器一致），实参转 double
            res = B->CreateFDiv(B->CreateSIToFP(l.val, B->getDoubleTy()),
                                B->CreateSIToFP(r.val, B->getDoubleTy()), "fdiv");
            return {VType::Float, res};
        }
    if (op == "//") res = B->CreateSDiv(l.val, r.val, "idiv"); // "//" 才是整数除法
    if (op == "%") {
        if (isFloat) { res = B->CreateFRem(l.val, r.val, "frem"); }
        else res = B->CreateSRem(l.val, r.val, "rem");
    }
    if (op == "**") {
        llvm::Value* fn = decl_vor("vor_pow", VType::Float, {VType::Float, VType::Float});
        llvm::Value* f1 = B->CreateSIToFP(l.val, B->getDoubleTy());
        llvm::Value* f2 = B->CreateSIToFP(r.val, B->getDoubleTy());
        res = B->CreateCall(llvm::cast<llvm::Function>(fn), {f1, f2});
        return {VType::Float, res};
    }
    if (res) return {isFloat ? VType::Float : VType::Int, res};

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

    res = isFloat ? B->CreateFCmp(pred, l.val, r.val, "cmp") : B->CreateICmp(pred, l.val, r.val, "cmp");
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
            for (auto& a : c->args) {
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
                llvm::AllocaInst* alloca = make_alloca(tEff, cur_bb()->getParent(), nm.c_str());
                llvm::Value* store = sv.val;
                if (tEff == VType::Bool && sv.type != VType::Bool) store = B->CreateICmpNE(sv.val, B->getInt64(0));
                B->CreateStore(store, alloca);
                // 记录槽 + 类型；标识符访问时按需重新 load
                scopes_.back()[nm] = LVal{tEff, B->CreateLoad(llvm_type(tEff), alloca), alloca};
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
            llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(B->getContext(), "if.then", F);
            llvm::BasicBlock* elseBB = llvm::BasicBlock::Create(B->getContext(), "if.else", F);
            llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(B->getContext(), "if.end", F);

            LVal cond = gen_expr(ifs->cond.get());
            B->CreateCondBr(cond.val, thenBB, elseBB);

            B->SetInsertPoint(thenBB);
            for (auto& st : ifs->then_body->stmts) gen_stmt(st.get());
            if (!block_has_term(cur_bb())) B->CreateBr(mergeBB);

            B->SetInsertPoint(elseBB);
            for (auto& el : ifs->elif_list) {
                // 仅支持单一 elif→简化成嵌套 if，不完整
            }
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
                      "\" -o \"" + cfg.output + "\" -static -static-libgcc -static-libstdc++";
    int rc = std::system(cmd.c_str());
    if (rc != 0) return "[Link] g++ failed (code " + std::to_string(rc) + ")";
    return {};
}

} // namespace vortex