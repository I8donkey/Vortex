// ============================================================
// runtime.cpp — 类型化运行时实现（C ABI）
// 编译为静态库，供 LLVM 后端生成的产物链接。
// ============================================================
#include "runtime.h"
#include "net_core.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <random>
#include <chrono>
#include <thread>
#include <ctime>
#include <sstream>
#include <fstream>
#include <mutex>
#include <atomic>
#include <vector>
#include <string>
#include <memory>
#include <algorithm>
#include <numeric>
#include <queue>
#include <condition_variable>
#include <functional>
#include <filesystem>
#include <set>
#include <zlib.h>
#include <sqlite3.h>
#ifdef _WIN32
#include <direct.h>
#include <sys/stat.h>
#include <windows.h>
#endif
namespace fs = std::filesystem;

// ========== 内存分配助手 ==========
static void* xmalloc(size_t n) {
    void* p = std::malloc(n);
    if (!p) std::abort();
    return p;
}

// ========== 引用计数 ==========
void vor_obj_retain(void* obj) {
    VObject* o = (VObject*)obj;
    if (o) o->refcount++;
}
void vor_obj_release(void* obj) {
    VObject* o = (VObject*)obj;
    if (!o) return;
    if (--o->refcount <= 0) {
        // 按 tag 释放：1=VStr(内联数据无需单独释放) 2=VList(释放 elems) 3=VDict(释放 entries 与子对象)
        if (o->tag == 2) {
            VList* l = (VList*)o;
            std::free(l->elems);
        } else if (o->tag == 3) {
            VDict* d = (VDict*)o;
            for (int i = 0; i < d->len; ++i) {
                VDictEntry* e = &d->entries[i];
                if (e->kkind == VK_STR) vor_obj_release(e->kstr);
                if (e->vkind == VK_STR) vor_obj_release(e->vstr);
            }
            std::free(d->entries);
        } else if (o->tag == 4) {
            VTuple* t = (VTuple*)o;
            std::free(t->elems);
        }
        std::free(o);
    }
}

// ========== 字符串 ==========
VStr* vor_str_from_bytes(const char* s, int len) {
    if (len < 0) len = 0;
    VStr* v = (VStr*)xmalloc(sizeof(VStr) + (size_t)len);
    v->hdr.refcount = 1;
    v->hdr.tag = 1;
    v->hdr.next = nullptr;
    v->len = len;
    if (len > 0 && s) std::memcpy(v->data, s, (size_t)len);
    v->data[len] = '\0';
    return v;
}
VStr* vor_str_from_cstr(const char* s) {
    if (!s) s = "";
    return vor_str_from_bytes(s, (int)std::strlen(s));
}
VStr* vor_str_concat(VStr* a, VStr* b) {
    int n = a->len + b->len;
    VStr* v = (VStr*)xmalloc(sizeof(VStr) + (size_t)n);
    v->hdr.refcount = 1;
    v->hdr.tag = 1;
    v->hdr.next = nullptr;
    v->len = n;
    std::memcpy(v->data, a->data, (size_t)a->len);
    std::memcpy(v->data + a->len, b->data, (size_t)b->len);
    v->data[n] = '\0';
    return v;
}
int vor_str_len(VStr* s) { return s ? s->len : 0; }
const char* vor_str_cstr(VStr* s) { return s ? s->data : ""; }
int vor_str_cmp(const VStr* a, const VStr* b) {
    const char* x = a ? a->data : "";
    const char* y = b ? b->data : "";
    return strcmp(x, y);
}

static void format_double(char* buf, size_t cap, double v) {
    // 与解释器对齐：15 位有效数字，去掉多余的尾零
    snprintf(buf, cap, "%.15g", v);
}

VStr* vor_i64_to_str(long long v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", v);
    return vor_str_from_cstr(buf);
}
VStr* vor_double_to_str(double v) {
    char buf[64];
    format_double(buf, sizeof(buf), v);
    return vor_str_from_cstr(buf);
}
VStr* vor_bool_to_str(int b) {
    return vor_str_from_cstr(b ? "true" : "false");
}

// str -> 标量（解析失败抛异常，与解释器一致）
static void throw_rt(const char* msg) {
    VStr* m = vor_str_from_cstr(msg);
    vor_throw_str(m);
    vor_obj_release(m);
}
long long vor_cast_i64(VStr* s) {
    if (!s) throw_rt("Cannot convert str to int");
    std::string txt(s->data, (size_t)s->len);
    try {
        size_t pos = 0;
        long long v = std::stoll(txt, &pos);
        if (pos != txt.size()) throw std::invalid_argument("trailing");
        return v;
    } catch (...) {
        throw_rt(("Cannot convert '" + txt + "' to int").c_str());
    }
    return 0;
}
double vor_cast_f64(VStr* s) {
    if (!s) throw_rt("Cannot convert str to float");
    std::string txt(s->data, (size_t)s->len);
    try {
        size_t pos = 0;
        double v = std::stod(txt, &pos);
        if (pos != txt.size()) throw std::invalid_argument("trailing");
        return v;
    } catch (...) {
        throw_rt(("Cannot convert '" + txt + "' to float").c_str());
    }
    return 0.0;
}

// ========== 异常：setjmp/longjmp (P3 try/catch) ==========
// UCRT 不导出普通 `setjmp`（它是 SEH 内建），故自行提供 Win64 低层
// setjmp/longjmp（naked asm），避免任何 DLL 入口依赖。
struct RT_ExFrame { void* jb; char msg[512]; RT_ExFrame* prev; };
static thread_local RT_ExFrame* rt_ex_top_ = nullptr;

#ifndef VORTEX_RT_EXJMP_ASM
#define VORTEX_RT_EXJMP_ASM
__asm__(
".intel_syntax noprefix\n\t"
".text\n\t"
// int vor_ex_setjmp(void* jb) — 保存调用方(代码生成帧)的 Win64 callee-saved
".globl vor_ex_setjmp\n\t"
"vor_ex_setjmp:\n\t"
"  mov rax, rcx\n\t"                  // jb
"  mov [rax+0], rbx\n\t"
"  mov [rax+8], rbp\n\t"
"  mov [rax+16], rdi\n\t"
"  mov [rax+24], rsi\n\t"
"  mov [rax+32], r12\n\t"
"  mov [rax+40], r13\n\t"
"  mov [rax+48], r14\n\t"
"  mov [rax+56], r15\n\t"
"  mov [rax+64], rsp\n\t"             // 保存调用点 SP(含返回地址槽)
"  mov rdx, [rsp]\n\t"
"  mov [rax+72], rdx\n\t"             // 返回地址
"  xor eax, eax\n\t"                  // 首次返回 0
"  ret\n\t"
// void vor_ex_longjmp(void* jb, int val) — 恢复上下文跳回 setjmp 点
".globl vor_ex_longjmp\n\t"
"vor_ex_longjmp:\n\t"
"  mov rax, rcx\n\t"                  // jb
"  mov rbx, [rax+0]\n\t"
"  mov rbp, [rax+8]\n\t"
"  mov rdi, [rax+16]\n\t"
"  mov rsi, [rax+24]\n\t"
"  mov r12, [rax+32]\n\t"
"  mov r13, [rax+40]\n\t"
"  mov r14, [rax+48]\n\t"
"  mov r15, [rax+56]\n\t"
"  mov rsp, [rax+64]\n\t"
"  mov rcx, [rax+72]\n\t"             // 返回地址
"  mov [rsp], rcx\n\t"
"  mov eax, edx\n\t"                  // 返回值 = val
"  ret\n\t"
".intel_syntax noprefix\n\t"
".att_syntax prefix\n\t"
);
#endif

void vor_ex_push(void* jb) {
    RT_ExFrame* f = (RT_ExFrame*)xmalloc(sizeof(RT_ExFrame));
    f->jb = jb; f->msg[0] = '\0'; f->prev = rt_ex_top_;
    rt_ex_top_ = f;
}
void vor_ex_pop(void) {
    if (rt_ex_top_) { RT_ExFrame* p = rt_ex_top_->prev; std::free(rt_ex_top_); rt_ex_top_ = p; }
}
void vor_throw_str(VStr* msg) {
    if (rt_ex_top_) {
        std::snprintf(rt_ex_top_->msg, sizeof(rt_ex_top_->msg), "%s",
                      msg ? msg->data : "");
        vor_ex_longjmp(rt_ex_top_->jb, 1);
    }
    std::fprintf(stderr, "%s\n", msg ? msg->data : "");
    std::abort();
}
VStr* vor_ex_caught(void) {
    return vor_str_from_cstr(rt_ex_top_ ? rt_ex_top_->msg : "");
}

// ========== 列表 ==========
void vor_print_int_list(VList* l) {
    if (!l) { std::fputs("[]", stdout); return; }
    std::fputc('[', stdout);
    for (int i = 0; i < l->len; ++i) {
        if (i) std::fputs(", ", stdout);
        std::fprintf(stdout, "%lld", l->elems[i]);
    }
    std::fputc(']', stdout);
}
VList* vor_list_new(void) {
    VList* l = (VList*)xmalloc(sizeof(VList));
    l->hdr.refcount = 1;
    l->hdr.tag = 2;
    l->hdr.next = nullptr;
    l->len = 0;
    l->cap = 0;
    l->elems = nullptr;
    return l;
}
static void list_grow(VList* l, int need) {
    if (need <= l->cap) return;
    int ncap = l->cap ? l->cap : 4;
    while (ncap < need) ncap *= 2;
    long long* ne = (long long*)realloc(l->elems, (size_t)ncap * sizeof(long long));
    if (!ne) std::abort();
    l->elems = ne;
    l->cap = ncap;
}
void vor_list_push(VList* l, long long e) {
    list_grow(l, l->len + 1);
    l->elems[l->len++] = e;
}
void vor_list_set(VList* l, int idx, long long e) {
    if (idx < 0 || idx >= l->len) return;
    l->elems[idx] = e;
}
long long vor_list_get(const VList* l, int idx) {
    if (idx < 0 || idx >= l->len) return 0;
    return l->elems[idx];
}
// 字符串列表：按索引读取，返回一份独立拷贝（消费者自持引用，避免与列表共享生命周期）
VStr* vor_list_get_str(const VList* l, int idx) {
    if (!l || idx < 0 || idx >= l->len) return vor_str_from_cstr("");
    VStr* held = (VStr*)l->elems[idx];
    if (!held) return vor_str_from_cstr("");
    return vor_str_from_bytes(held->data, held->len);
}
int vor_list_len(const VList* l) { return l->len; }
long long vor_list_sum(const VList* l) {
    long long s = 0;
    if (!l) return 0;
    for (int i = 0; i < l->len; ++i) s += l->elems[i];
    return s;
}
long long vor_list_prod(const VList* l) {
    long long p = 1;
    if (!l) return 1;
    for (int i = 0; i < l->len; ++i) p *= l->elems[i];
    return p;
}

// ========== 字典 ==========
VDict* vor_dict_new(void) {
    VDict* d = (VDict*)xmalloc(sizeof(VDict));
    d->hdr.refcount = 1;
    d->hdr.tag = 3;
    d->hdr.next = nullptr;
    d->len = 0;
    d->cap = 0;
    d->entries = nullptr;
    return d;
}
static void dict_grow(VDict* d, int need) {
    if (need <= d->cap) return;
    int ncap = d->cap ? d->cap : 4;
    while (ncap < need) ncap *= 2;
    VDictEntry* ne = (VDictEntry*)realloc(d->entries, (size_t)ncap * sizeof(VDictEntry));
    if (!ne) std::abort();
    d->entries = ne;
    d->cap = ncap;
}
int vor_dict_len(const VDict* d) { return d ? d->len : 0; }

static int str_eq(VStr* a, VStr* b) {
    if (!a || !b) return a == b;
    return a->len == b->len && std::memcmp(a->data, b->data, (size_t)a->len) == 0;
}
// 找到 key 相等项索引；未命中返回 -1
static int find_key(const VDict* d, int kkind, long long knum, VStr* kstr) {
    for (int i = 0; i < d->len; ++i) {
        const VDictEntry* e = &d->entries[i];
        if (e->kkind != kkind) continue;
        if (kkind == VK_INT) { if (e->kint == knum) return i; }
        else if (str_eq(e->kstr, kstr)) return i;
    }
    return -1;
}
void vor_dict_set_int(VDict* d, long long key, long long val) {
    int i = find_key(d, VK_INT, key, nullptr);
    if (i < 0) { dict_grow(d, d->len + 1); i = d->len++; d->entries[i].kkind = VK_INT; d->entries[i].kint = key; d->entries[i].kstr = nullptr; }
    else if (d->entries[i].vkind == VK_STR) vor_obj_release(d->entries[i].vstr);
    d->entries[i].vkind = VK_INT; d->entries[i].vint = val; d->entries[i].vstr = nullptr;
}
void vor_dict_set_str(VDict* d, VStr* key, long long val) {
    if (!key) return;
    int i = find_key(d, VK_STR, 0, key);
    if (i < 0) { dict_grow(d, d->len + 1); i = d->len++; d->entries[i].kkind = VK_STR; d->entries[i].kstr = key; vor_obj_retain(key); d->entries[i].kint = 0; }
    else if (d->entries[i].vkind == VK_STR) vor_obj_release(d->entries[i].vstr);
    d->entries[i].vkind = VK_INT; d->entries[i].vint = val; d->entries[i].vstr = nullptr;
}
void vor_dict_set_str_vstr(VDict* d, VStr* key, VStr* val) {
    if (!key) return;
    int i = find_key(d, VK_STR, 0, key);
    if (i < 0) { dict_grow(d, d->len + 1); i = d->len++; d->entries[i].kkind = VK_STR; d->entries[i].kstr = key; vor_obj_retain(key); d->entries[i].kint = 0; }
    else if (d->entries[i].vkind == VK_STR) vor_obj_release(d->entries[i].vstr);
    d->entries[i].vkind = VK_STR; d->entries[i].vstr = val; if (val) vor_obj_retain(val); else d->entries[i].vint = 0;
    d->entries[i].vint = 0;
}
long long vor_dict_get_int(VDict* d, long long key) {
    int i = find_key(d, VK_INT, key, nullptr);
    if (i < 0) return 0;
    return d->entries[i].vkind == VK_INT ? d->entries[i].vint : 0;
}
long long vor_dict_get_str(VDict* d, VStr* key) {
    if (!key) return 0;
    int i = find_key(d, VK_STR, 0, key);
    if (i < 0) return 0;
    return d->entries[i].vkind == VK_INT ? d->entries[i].vint : 0;
}
VStr* vor_dict_get_int_vstr(VDict* d, long long key) {
    int i = find_key(d, VK_INT, key, nullptr);
    if (i < 0) return nullptr;
    return d->entries[i].vkind == VK_STR ? d->entries[i].vstr : nullptr;
}
VStr* vor_dict_get_str_vstr(VDict* d, VStr* key) {
    if (!key) return nullptr;
    int i = find_key(d, VK_STR, 0, key);
    if (i < 0) return nullptr;
    return d->entries[i].vkind == VK_STR ? d->entries[i].vstr : nullptr;
}
int vor_dict_contains_int(VDict* d, long long key) {
    return find_key(d, VK_INT, key, nullptr) >= 0;
}
int vor_dict_contains_str(VDict* d, VStr* key) {
    return key && find_key(d, VK_STR, 0, key) >= 0;
}
int vor_dict_int_key_count(const VDict* d) {
    int c = 0;
    for (int i = 0; i < d->len; ++i) if (d->entries[i].kkind == VK_INT) c++;
    return c;
}
long long vor_dict_int_key_at(const VDict* d, int j) {
    for (int i = 0; i < d->len; ++i)
        if (d->entries[i].kkind == VK_INT) { if (j-- == 0) return d->entries[i].kint; }
    return 0;
}
void vor_print_dict(VDict* d) {
    if (!d) { std::fputs("{}", stdout); return; }
    std::fputc('{', stdout);
    for (int i = 0; i < d->len; ++i) {
        if (i) std::fputs(", ", stdout);
        const VDictEntry* e = &d->entries[i];
        if (e->kkind == VK_STR) { std::fputc('"', stdout); vor_print_str(e->kstr); std::fputc('"', stdout); }
        else std::fprintf(stdout, "%lld", e->kint);
        std::fputs(": ", stdout);
        if (e->vkind == VK_STR) { std::fputc('"', stdout); vor_print_str(e->vstr); std::fputc('"', stdout); }
        else std::fprintf(stdout, "%lld", e->vint);
    }
    std::fputc('}', stdout);
}

// ========== 集合（复用 VList + 去重，元素 int）==========
int vor_set_contains(const VList* s, long long e) {
    if (!s) return 0;
    for (int i = 0; i < s->len; ++i) if (s->elems[i] == e) return 1;
    return 0;
}
void vor_set_add(VList* s, long long e) {
    if (!s || vor_set_contains(s, e)) return;
    vor_list_push(s, e);
}
void vor_set_remove(VList* s, long long e) {
    if (!s) return;
    for (int i = 0; i < s->len; ++i)
        if (s->elems[i] == e) {
            for (int j = i; j < s->len - 1; ++j) s->elems[j] = s->elems[j + 1];
            s->len--;
            return;
        }
}
int vor_set_len(const VList* s) { return s ? s->len : 0; }
VList* vor_set_from_list(const VList* l) {
    VList* s = vor_list_new();
    if (l) for (int i = 0; i < l->len; ++i) vor_set_add(s, l->elems[i]);
    return s;
}
void vor_print_set(const VList* s) {
    if (!s) { std::fputs("{}", stdout); return; }
    std::fputc('{', stdout);
    for (int i = 0; i < s->len; ++i) {
        if (i) std::fputs(", ", stdout);
        std::fprintf(stdout, "%lld", s->elems[i]);
    }
    std::fputc('}', stdout);
}

// ========== 序对 ==========
VPair* vor_pair_new(long long a, long long b) {
    VPair* p = (VPair*)xmalloc(sizeof(VPair));
    p->hdr.refcount = 1;
    p->hdr.tag = 5;
    p->hdr.next = nullptr;
    p->first = a;
    p->second = b;
    return p;
}
long long vor_pair_first(const VPair* p)  { return p ? p->first : 0; }
long long vor_pair_second(const VPair* p) { return p ? p->second : 0; }
void vor_print_pair(const VPair* p) {
    if (!p) { std::fputs("(0, 0)", stdout); return; }
    std::fprintf(stdout, "(%lld, %lld)", p->first, p->second);
}

// ========== 元组 ==========
VTuple* vor_tuple_new(int n) {
    VTuple* t = (VTuple*)xmalloc(sizeof(VTuple));
    t->hdr.refcount = 1;
    t->hdr.tag = 4;
    t->hdr.next = nullptr;
    t->len = n > 0 ? n : 0;
    t->cap = t->len;
    t->elems = t->cap ? (long long*)xmalloc((size_t)t->cap * sizeof(long long)) : nullptr;
    if (t->elems) std::memset(t->elems, 0, (size_t)t->len * sizeof(long long));
    return t;
}
VTuple* vor_tuple_from_list(const VList* l) {
    int n = l ? l->len : 0;
    VTuple* t = vor_tuple_new(n);
    if (l) for (int i = 0; i < l->len; ++i) t->elems[i] = l->elems[i];
    return t;
}
void vor_tuple_set(VTuple* t, int idx, long long e) {
    if (t && idx >= 0 && idx < t->len) t->elems[idx] = e;
}
long long vor_tuple_at(const VTuple* t, long long idx) {
    if (!t || idx < 0 || idx >= t->len) return 0;
    return t->elems[idx];
}
int vor_tuple_len(const VTuple* t) { return t ? t->len : 0; }
void vor_print_tuple(const VTuple* t) {
    if (!t) { std::fputs("()", stdout); return; }
    std::fputc('(', stdout);
    for (int i = 0; i < t->len; ++i) {
        if (i) std::fputs(", ", stdout);
        std::fprintf(stdout, "%lld", t->elems[i]);
    }
    std::fputc(')', stdout);
}
void vor_print_str(VStr* s) {
    if (s) std::fwrite(s->data, 1, (size_t)s->len, stdout);
}
void vor_print_i64(long long v) {
    std::fprintf(stdout, "%lld", v);
}
void vor_print_double(double v) {
    char buf[64];
    format_double(buf, sizeof(buf), v);
    std::fputs(buf, stdout);
}
void vor_print_bool(int b) {
    std::fputs(b ? "true" : "false", stdout);
}
void vor_print_newline(void) {
    std::fputc('\n', stdout);
}
void vor_flush(void) {
    std::fflush(stdout);
}

// ========== 数学 ==========
double vor_pow(double a, double b)   { return std::pow(a, b); }
double vor_sqrt(double a)            { return std::sqrt(a); }
double vor_fabs(double a)            { return std::fabs(a); }
double vor_sin(double a)             { return std::sin(a); }
double vor_cos(double a)             { return std::cos(a); }
double vor_tan(double a)             { return std::tan(a); }
long long vor_floor(double a)        { return (long long)std::floor(a); }
long long vor_ceil(double a)         { return (long long)std::ceil(a); }

// 更多 math.* 转发（与解释器 math 模块对齐）
double vor_cbrt(double a)            { return std::cbrt(a); }
double vor_exp(double a)             { return std::exp(a); }
double vor_log(double a)             { return std::log(a); }
double vor_logbase(double a, double base) { return std::log(a) / std::log(base); }
double vor_log2(double a)            { return std::log2(a); }
double vor_log10(double a)           { return std::log10(a); }
double vor_log1p(double a)           { return std::log1p(a); }
double vor_expm1(double a)           { return std::expm1(a); }
double vor_erf(double a)             { return std::erf(a); }
double vor_tgamma(double a)          { return std::tgamma(a); }
double vor_lgamma(double a)          { return std::lgamma(a); }
double vor_asin(double a)            { return std::asin(a); }
double vor_acos(double a)            { return std::acos(a); }
double vor_atan(double a)            { return std::atan(a); }
double vor_atan2(double a, double b) { return std::atan2(a, b); }
double vor_sinh(double a)            { return std::sinh(a); }
double vor_cosh(double a)            { return std::cosh(a); }
double vor_tanh(double a)            { return std::tanh(a); }
double vor_asinh(double a)           { return std::asinh(a); }
double vor_acosh(double a)           { return std::acosh(a); }
double vor_atanh(double a)           { return std::atanh(a); }
double vor_hypot(double a, double b) { return std::hypot(a, b); }
long long vor_trunc(double a)        { return (long long)std::trunc(a); }
double vor_roundn(double a, long long nd) {
    double p = std::pow(10.0, (double)nd);
    return std::round(a * p) / p;
}
double vor_fmod(double a, double b)  { return std::fmod(a, b); }
double vor_fmin(double a, double b)  { return std::fmin(a, b); }
double vor_fmax(double a, double b)  { return std::fmax(a, b); }
long long vor_gcd(long long a, long long b) { return (long long)std::gcd(a, b); }
long long vor_lcm(long long a, long long b) {
    if (a == 0 || b == 0) return 0;
    long long g = std::gcd(a, b);
    return (a / g) * b;
}
long long vor_isinf(double a) { return std::isinf(a) ? 1 : 0; }
long long vor_isnan(double a) { return std::isnan(a) ? 1 : 0; }
long long vor_isfinite(double a) { return std::isfinite(a) ? 1 : 0; }
long long vor_isclose(double a, double b, double rel_tol, double abs_tol) {
    if (rel_tol < 0) rel_tol = 0;
    if (abs_tol < 0) abs_tol = 0;
    double diff = std::fabs(a - b);
    double mx = std::fmax(std::fabs(a), std::fabs(b));
    return diff <= std::fmax(rel_tol * mx, abs_tol) ? 1 : 0;
}
long long vor_iabs(long long a)  { return a < 0 ? -a : a; }
long long vor_imin(long long a, long long b) { return a < b ? a : b; }
long long vor_imax(long long a, long long b) { return a > b ? a : b; }
long long vor_comb(long long n, long long k) {
    if (k < 0 || k > n) return 0;
    if (k > n - k) k = n - k;
    long long r = 1;
    for (long long i = 0; i < k; ++i) r = r * (n - i) / (i + 1);
    return r;
}
long long vor_perm(long long n, long long k) {
    if (k < 0 || k > n) return 0;
    long long r = 1;
    for (long long i = 0; i < k; ++i) r *= (n - i);
    return r;
}
double vor_radians(double a)    { return a * (3.14159265358979323846 / 180.0); }
double vor_degrees(double a)    { return a * (180.0 / 3.14159265358979323846); }
double vor_copysign(double a, double b) { return std::copysign(a, b); }
double vor_remainder(double a, double b){ return std::remainder(a, b); }
long long vor_factorial(long long n) {
    if (n < 0) throw_rt("math.factorial: negative argument");
    long long r = 1;
    for (long long i = 2; i <= n; ++i) r *= i;
    return r;
}
long long vor_isqrt(long long n) {
    if (n < 0) throw_rt("math.isqrt: negative argument");
    long long r = (long long)std::sqrt((double)n);
    while ((r + 1) * (r + 1) <= n) ++r;
    while (r * r > n) --r;
    return r;
}

// ========== time 模块转发（P4 批A） ==========
double vor_time_now(void) {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}
void vor_sleep(double sec) {
    if (sec > 0) std::this_thread::sleep_for(std::chrono::duration<double>(sec));
}
static std::chrono::steady_clock::time_point rt_counter_start = std::chrono::steady_clock::now();
double vor_counter(void) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - rt_counter_start).count();
}
void vor_counter_reset(void) { rt_counter_start = std::chrono::steady_clock::now(); }
double vor_process_time(void) { return (double)std::clock() / CLOCKS_PER_SEC; }

// ========== sys 模块转发 ==========
VStr* vor_sys_version(void) {
    return vor_str_from_cstr("Vortex 1.0");
}
long long vor_sys_time_ms(void) {
    using namespace std::chrono;
    return (long long)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
double vor_sys_clock(void) { return vor_time_now(); }
long long vor_sys_sleep(long long ms) { vor_sleep((double)ms / 1000.0); return 0; }
#if defined(_WIN32)
#define VORTEX_ATTRIB_NORETURN __declspec(noreturn)
#else
#define VORTEX_ATTRIB_NORETURN
#endif
VORTEX_ATTRIB_NORETURN void vor_sys_exit(long long code) {
    std::exit((int)code);
}
#if defined(_WIN32)
#undef VORTEX_ATTRIB_NORETURN
#endif

// ========== random 模块转发（P4 批A） ==========
// 单例 mt19937，与解释器 get_rng() 同款引擎/头实现，同 seed 下序列一致
static std::mt19937& rng_state() {
    static std::mt19937 rng((unsigned)std::time(nullptr));
    return rng;
}
void vor_rng_seed(unsigned long long seed) { rng_state().seed((std::mt19937::result_type)seed); }
double vor_rng_random(void) { return std::uniform_real_distribution<>(0.0, 1.0)(rng_state()); }
long long vor_rng_getrandbits(long long k) {
    if (k < 0) throw_rt("random.getrandbits: negative k");
    if (k > 63) k = 63;
    double r = vor_rng_random();
    return (long long)(r * std::ldexp(1.0, (int)k));
}
double vor_rng_uniform(double a, double b) {
    if (b < a) std::swap(a, b);
    return std::uniform_real_distribution<>(a, b)(rng_state());
}
long long vor_rng_randint(long long lo, long long hi) {
    return std::uniform_int_distribution<long long>(lo, hi)(rng_state());
}
long long vor_rng_randrange(long long start, long long stop, long long step) {
    if (step <= 0) step = 1;
    long long n = (stop - start + step - 1) / step;
    return start + (long long)std::uniform_int_distribution<long long>(0, n - 1)(rng_state()) * step;
}
double vor_rng_gauss(double mu, double sigma) { return std::normal_distribution<>(mu, sigma)(rng_state()); }
double vor_rng_expovariate(double lambda) { return std::exponential_distribution<>(lambda)(rng_state()); }
double vor_rng_triangular(double lo, double hi, double mode) {
    if (hi <= lo) return lo;
    if (mode < lo || mode > hi) mode = (lo + hi) / 2.0;
    double r = std::uniform_real_distribution<double>(0.0, 1.0)(rng_state());
    double c = (mode - lo) / (hi - lo);
    return (r < c)
        ? lo + std::sqrt(r * c) * (hi - lo)
        : mode + (hi - mode) * (1.0 - std::sqrt((1.0 - r) / (1.0 - c)));
}
VStr* vor_rng_getstate(void) { std::ostringstream oss; oss << rng_state(); return vor_str_from_cstr(oss.str().c_str()); }
void vor_rng_setstate(VStr* s) {
    if (!s) return;
    std::istringstream iss(s->data); iss >> rng_state();
}

// ========== log 模块转发（P4 批A） ==========
namespace vortlog {
struct State {
    int min_level = 20;
    bool console_on = true;
    std::string format = "[%(time)] [%(level)] %(name): %(message)";
    std::vector<std::shared_ptr<std::ofstream>> files;
    std::mutex mtx;
};
State& st() { static State s; return s; }
static int level_from_str(const char* s) {
    if (!s) return 20;
    if (!std::strcmp(s, "DEBUG") || !std::strcmp(s, "debug")) return 10;
    if (!std::strcmp(s, "INFO")  || !std::strcmp(s, "info"))  return 20;
    if (!std::strcmp(s, "WARN")  || !std::strcmp(s, "warn"))  return 30;
    if (!std::strcmp(s, "ERROR") || !std::strcmp(s, "error")) return 40;
    if (!std::strcmp(s, "FATAL") || !std::strcmp(s, "fatal")) return 50;
    return 20;
}
static const char* level_name(int lv) {
    switch (lv) {
        case 10: return "DEBUG";
        case 20: return "INFO";
        case 30: return "WARN";
        case 40: return "ERROR";
        case 50: return "FATAL";
        default: return "???";
    }
}
} // namespace vortlog

void vor_log_emit(int level, VStr* msg) {
    using namespace vortlog;
    State& s = st();
    if (level < s.min_level) return;
    std::lock_guard<std::mutex> lk(s.mtx);
    // 拼时间戳 YYYY-MM-DD HH:MM:SS.mmm
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm = *std::localtime(&t);
    char tbuf[64]; std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);
    std::string ts = std::string(tbuf) + "." + (ms.count() < 100 ? (ms.count() < 10 ? "00" : "0") : "") + std::to_string(ms.count());
    std::string fmt = s.format;
    auto repl = [&](const std::string& tok, const std::string& val) {
        size_t p = 0;
        while ((p = fmt.find(tok, p)) != std::string::npos) { fmt.replace(p, tok.size(), val); p += val.size(); }
    };
    repl("%(time)", ts);
    repl("%(level)", level_name(level));
    repl("%(name)", "vortex");
    repl("%(message)", msg ? std::string(msg->data, msg->len) : "");
    std::string line = fmt + "\n";
    if (s.console_on) {
        if (level >= 40) std::fputs(line.c_str(), stderr);
        else std::fputs(line.c_str(), stdout);
    }
    for (auto& fs : s.files) if (fs && fs->is_open()) *fs << line;
}
void vor_log_level(VStr* name) { vortlog::st().min_level = vortlog::level_from_str(name ? name->data : ""); }
VStr* vor_log_get_level(void) { return vor_str_from_cstr(vortlog::level_name(vortlog::st().min_level)); }
void vor_log_format(VStr* fmt) { if (fmt) vortlog::st().format = std::string(fmt->data, fmt->len); }
void vor_log_file(VStr* path) {
    if (!path) return;
    auto& st_ = vortlog::st();
    auto fs = std::make_shared<std::ofstream>(std::string(path->data, path->len), std::ios::app);
    st_.files.push_back(fs);
}
void vor_log_console(int on) { vortlog::st().console_on = on != 0; }

// ========== time 批B：tuple 交互（P4 批B） ==========
// 构造 9 元 tm 元组，字段与解释器 tm_to_tuple 一致（批B）
static VTuple* tm_to_tuple(const std::tm& t) {
    VTuple* r = vor_tuple_new(9);
    r->elems[0] = t.tm_year + 1900; r->elems[1] = t.tm_mon + 1;
    r->elems[2] = t.tm_mday;        r->elems[3] = t.tm_hour;
    r->elems[4] = t.tm_min;         r->elems[5] = t.tm_sec;
    r->elems[6] = (t.tm_wday + 6) % 7; r->elems[7] = t.tm_yday + 1; r->elems[8] = 0;
    return r;
}
static std::tm tuple_to_tm(const VTuple* tr) {
    std::tm t; std::memset(&t, 0, sizeof(t));
    if (tr && tr->len >= 6) {
        t.tm_year = (int)tr->elems[0] - 1900; t.tm_mon = (int)tr->elems[1] - 1;
        t.tm_mday = (int)tr->elems[2];        t.tm_hour = (int)tr->elems[3];
        t.tm_min  = (int)tr->elems[4];        t.tm_sec  = (int)tr->elems[5];
    }
    return t;
}
VTuple* vor_time_gmtime(double ts) {
    std::time_t t = (std::time_t)ts; return tm_to_tuple(*std::gmtime(&t));
}
VTuple* vor_time_localtime(double ts) {
    std::time_t t = (std::time_t)ts; return tm_to_tuple(*std::localtime(&t));
}
double vor_time_mktime(const VTuple* tr) {
    std::tm t = tuple_to_tm(tr); return (double)std::mktime(&t);
}
VStr* vor_time_strftime(VStr* fmt, const VTuple* tr) {
    std::tm t = tuple_to_tm(tr); std::mktime(&t);
    char buf[256];
    std::strftime(buf, sizeof(buf), fmt && fmt->data ? fmt->data : "", &t);
    return vor_str_from_cstr(buf);
}

// ========== random 批B：list/tuple 交互（P4 批B） ==========
// 返回/修改 VList（元素为 i64 盒），与解释器同引擎分布保证序列一致
long long vor_rng_choice(const VList* pop) {
    if (!pop || pop->len == 0) return 0;
    return pop->elems[(size_t)std::uniform_int_distribution<size_t>(0, (size_t)pop->len - 1)(rng_state())];
}
VList* vor_rng_choices(const VList* pop, const VList* weights, long long k) {
    VList* r = vor_list_new();
    if (!pop) return r;
    if (k < 0) k = 0;
    if (!weights) {
        std::uniform_int_distribution<size_t> d(0, (size_t)pop->len - 1);
        for (long long i = 0; i < k; ++i) vor_list_push(r, pop->elems[d(rng_state())]);
        return r;
    }
    std::vector<double> w;
    int wn = weights->len < pop->len ? weights->len : pop->len;
    for (int i = 0; i < wn; ++i) w.push_back((double)weights->elems[i]);
    std::discrete_distribution<size_t> dd(w.begin(), w.end());
    for (long long i = 0; i < k; ++i) vor_list_push(r, pop->elems[dd(rng_state())]);
    return r;
}
void vor_rng_shuffle(VList* l) {
    if (!l) return;
    std::vector<long long> v(l->elems, l->elems + l->len);
    std::shuffle(v.begin(), v.end(), rng_state());
    std::memcpy(l->elems, v.data(), sizeof(long long) * (size_t)l->len);
}
VList* vor_rng_sample(const VList* pop, long long k) {
    VList* r = vor_list_new();
    if (!pop) return r;
    std::vector<long long> v(pop->elems, pop->elems + pop->len);
    std::shuffle(v.begin(), v.end(), rng_state());
    long long n = k < (long long)v.size() ? k : (long long)v.size();
    for (long long i = 0; i < n; ++i) vor_list_push(r, v[(size_t)i]);
    return r;
}

// ========== 运行时生命周期 ==========
void vor_rt_init(void)    {}
void vor_rt_shutdown(void) {}

// ========== thread 模块转发（P4） ==========
struct RT_Thread { std::thread th; };
struct RT_Mutex  { std::mutex mtx; };
struct RT_Atomic { std::atomic<long long> v; explicit RT_Atomic(long long x) : v(x) {} };
// 编译后端通道：int 负载子集（静态类型，无法承载任意动态值）
struct RT_Channel {
    std::queue<long long> q;
    std::mutex mtx;
    std::condition_variable cv;
    bool closed = false;
    size_t capacity = 0; // 0 = 无界
};
struct RT_Pool {
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> stop{false};
    std::atomic<int> active{0};
    std::atomic<int> pending{0};
};

void* vor_thread_run(void (*fn)(void)) {
    if (!fn) return nullptr;
    RT_Thread* h = new RT_Thread();
    h->th = std::thread([fn]() { fn(); });
    return h;
}
void vor_thread_join(void* h) {
    RT_Thread* t = (RT_Thread*)h;
    if (t && t->th.joinable()) t->th.join();
}
void vor_thread_yield(void) { std::this_thread::yield(); }
void vor_thread_sleep(long long ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
long long vor_thread_hardware(void) {
    unsigned n = std::thread::hardware_concurrency();
    return n ? (long long)n : 0;
}
void* vor_thread_mutex(void) { return new RT_Mutex(); }
void  vor_thread_lock(void* m)   { if (m) ((RT_Mutex*)m)->mtx.lock(); }
void  vor_thread_unlock(void* m) { if (m) ((RT_Mutex*)m)->mtx.unlock(); }
int   vor_thread_trylock(void* m){ return (m && ((RT_Mutex*)m)->mtx.try_lock()) ? 1 : 0; }
void* vor_thread_atomic(long long v) { return new RT_Atomic(v); }
long long vor_thread_atomic_get(void* a) { return a ? ((RT_Atomic*)a)->v.load() : 0; }
void vor_thread_atomic_set(void* a, long long v) { if (a) ((RT_Atomic*)a)->v.store(v); }
long long vor_thread_atomic_add(void* a, long long v) { return a ? ((RT_Atomic*)a)->v.fetch_add(v) : 0; }

// ---- 通道（int 负载子集，P4） ----
void* vor_thread_channel(long long cap) {
    RT_Channel* c = new RT_Channel();
    c->capacity = (size_t)std::max(0LL, cap);
    return c;
}
void vor_thread_channel_send(void* h, long long v) {
    RT_Channel* c = (RT_Channel*)h;
    if (!c) return;
    std::unique_lock<std::mutex> lk(c->mtx);
    if (c->closed) { throw_rt("thread.send: channel closed"); return; }
    if (c->capacity > 0) {
        c->cv.wait(lk, [&]{ return c->q.size() < c->capacity || c->closed; });
        if (c->closed) { throw_rt("thread.send: channel closed"); return; }
    }
    c->q.push(v);
    c->cv.notify_one();
}
long long vor_thread_channel_recv(void* h) {
    RT_Channel* c = (RT_Channel*)h;
    if (!c) return 0;
    std::unique_lock<std::mutex> lk(c->mtx);
    c->cv.wait(lk, [&]{ return !c->q.empty() || c->closed; });
    if (c->q.empty()) { throw_rt("thread.recv: channel closed and empty"); return 0; }
    long long v = c->q.front();
    c->q.pop();
    c->cv.notify_one();
    return v;
}
void vor_thread_channel_close(void* h) {
    RT_Channel* c = (RT_Channel*)h;
    if (!c) return;
    { std::lock_guard<std::mutex> lk(c->mtx); c->closed = true; }
    c->cv.notify_all();
}
long long vor_thread_channel_len(void* h) {
    RT_Channel* c = (RT_Channel*)h;
    if (!c) return 0;
    std::lock_guard<std::mutex> lk(c->mtx);
    return (long long)c->q.size();
}

// ---- 线程池（P4） ----
void* vor_thread_pool(long long n) {
    RT_Pool* p = new RT_Pool();
    size_t workers = (size_t)std::max(1LL, n);
    for (size_t i = 0; i < workers; ++i) {
        p->workers.emplace_back([p]() {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lk(p->mtx);
                    p->cv.wait(lk, [p]{ return p->stop.load() || !p->tasks.empty(); });
                    if (p->stop.load() && p->tasks.empty()) return;
                    task = std::move(p->tasks.front());
                    p->tasks.pop();
                    p->pending.fetch_sub(1);
                }
                p->active.fetch_add(1);
                task();
                p->active.fetch_sub(1);
            }
        });
    }
    return p;
}
void vor_thread_pool_submit(void* h, void (*fn)(void)) {
    RT_Pool* p = (RT_Pool*)h;
    if (!p || !fn) return;
    {
        std::lock_guard<std::mutex> lk(p->mtx);
        p->tasks.push([fn]() { fn(); });
        p->pending.fetch_add(1);
    }
    p->cv.notify_one();
}
long long vor_thread_pool_size(void* h) {
    RT_Pool* p = (RT_Pool*)h;
    if (!p) return 0;
    return (long long)(p->active.load() + p->pending.load());
}
void vor_thread_pool_shutdown(void* h) {
    RT_Pool* p = (RT_Pool*)h;
    if (!p) return;
    { std::lock_guard<std::mutex> lk(p->mtx); p->stop.store(true); }
    p->cv.notify_all();
    for (auto& w : p->workers) if (w.joinable()) w.join();
    p->workers.clear();
}

// ---- 闭包值（P3）：([0]=唤起指针, [1..]=各捕获变量槽地址) ----
void* vor_closure_new(long long n) {
    size_t nw = (size_t)std::max(0LL, n) + 1;
    long long* p = (long long*)std::calloc(nw, sizeof(long long));
    if (!p) std::abort();
    return (void*)p;
}

// ========== file 模块转发 ==========
static std::string vstr_str(const VStr* s) {
    return s ? std::string(s->data, s->len) : std::string();
}
VStr* vor_file_read(VStr* path) {
    std::ifstream f(vstr_str(path), std::ios::binary);
    if (!f) throw_rt("file.read: cannot open file");
    std::ostringstream oss; oss << f.rdbuf();
    return vor_str_from_bytes(oss.str().data(), (int)oss.str().size());
}
VList* vor_file_readlines(VStr* path) {
    std::ifstream f(vstr_str(path), std::ios::binary);
    if (!f) throw_rt("file.readlines: cannot open file");
    std::ostringstream oss; oss << f.rdbuf();
    std::string content = oss.str();
    VList* l = vor_list_new();
    size_t pos = 0;
    while (true) {
        size_t nl = content.find('\n', pos);
        if (nl == std::string::npos) {
            vor_list_push(l, (long long)vor_str_from_bytes(content.data() + pos, (int)(content.size() - pos)));
            break;
        }
        std::string line = content.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();  // 兼容 CRLF
        vor_list_push(l, (long long)vor_str_from_cstr(line.c_str()));
        pos = nl + 1;
    }
    return l;
}
VList* vor_file_listdir(VStr* path) {
    VList* l = vor_list_new();
    std::error_code ec;
    std::vector<std::string> names;
    for (fs::directory_iterator it(vstr_str(path), ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        names.push_back(it->path().filename().string());
    }
    std::sort(names.begin(), names.end());  // 排序保证与解释器输出一致
    for (auto& n : names) vor_list_push(l, (long long)vor_str_from_cstr(n.c_str()));
    return l;
}
void vor_file_write(VStr* path, VStr* data) {
    std::ofstream f(vstr_str(path), std::ios::binary | std::ios::trunc);
    if (!f) throw_rt("file.write: cannot open file");
    if (data) f.write(data->data, data->len);
}
void vor_file_append(VStr* path, VStr* data) {
    std::ofstream f(vstr_str(path), std::ios::binary | std::ios::app);
    if (!f) throw_rt("file.append: cannot open file");
    if (data) f.write(data->data, data->len);
}
int vor_file_exists(VStr* path) {
    std::error_code ec; return fs::exists(vstr_str(path), ec) ? 1 : 0;
}
int vor_file_remove(VStr* path) {
    std::error_code ec;
    return fs::remove(vstr_str(path), ec) ? 1 : 0;
}
int vor_file_rename(VStr* from, VStr* to) {
    std::error_code ec; fs::rename(vstr_str(from), vstr_str(to), ec);
    return ec ? 0 : 1;
}
long long vor_file_size(VStr* path) {
    std::error_code ec;
    auto sz = fs::file_size(vstr_str(path), ec);
    return ec ? -1 : (long long)sz;
}
int vor_file_isdir(VStr* path) {
    std::error_code ec; return fs::is_directory(vstr_str(path), ec) ? 1 : 0;
}
int vor_file_isfile(VStr* path) {
    std::error_code ec; return fs::is_regular_file(vstr_str(path), ec) ? 1 : 0;
}
int vor_file_mkdir(VStr* path) {
    std::error_code ec; fs::create_directories(vstr_str(path), ec);
    return ec ? 0 : 1;
}
int vor_file_rmdir(VStr* path) {
    std::error_code ec;
    return fs::remove_all(vstr_str(path), ec) > 0 ? 1 : 0;
}

// ========== zip 模块转发（zlib store/deflate，最小 zip 容器） ==========
namespace vortzip {
// central dir 用到的 per-entry 元数据（按 es 索引顺序）
static std::vector<unsigned> cdir_lho;
static std::vector<std::tuple<unsigned,unsigned,unsigned,unsigned>> cdir_info;
struct Entry { std::string name; std::vector<unsigned char> data; };
static std::vector<Entry> read_archive(const std::string& path) {
    std::vector<Entry> out;
    std::ifstream f(path, std::ios::binary);
    std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)), {});
    if (buf.size() < 22) return out;
    // EOCD: last 22 bytes, search backward for 0x06054b50
    size_t eocd = (size_t)-1;
    for (size_t i = buf.size() >= 22 ? buf.size() - 22 : 0; i + 4 <= buf.size(); ++i) {
        if (buf[i]==0x50 && (size_t)buf[i+1]==0x4b && (size_t)buf[i+2]==0x05 && (size_t)buf[i+3]==0x06) {
            eocd = i; break;
        }
    }
    if (eocd == (size_t)-1) return out;
    auto rd16 = [&](size_t p){ return (size_t)buf[p] | ((size_t)buf[p+1]<<8); };
    auto rd32 = [&](size_t p){ return (size_t)buf[p] | ((size_t)buf[p+1]<<8) | ((size_t)buf[p+2]<<16) | ((size_t)buf[p+3]<<24); };
    size_t cd = rd32(eocd + 16);          // offset of central directory
    size_t n  = rd16(eocd + 10);          // number of entries
    size_t p  = cd;
    for (size_t k = 0; k < n && p + 46 <= buf.size(); ++k) {
        // central dir header sig 0x02014b50
        if (!(buf[p]==0x50 && (size_t)buf[p+1]==0x4b)) break;
        unsigned method = rd16(p + 10);
        unsigned csize  = (unsigned)rd32(p + 20);
        unsigned usize  = (unsigned)rd32(p + 24);
        unsigned nlen   = (unsigned)rd16(p + 28);
        unsigned lho    = (unsigned)rd32(p + 42);
        std::string name(reinterpret_cast<const char*>(&buf[p + 46]), nlen);
        if (lho + 30 <= buf.size() && buf[lho]==0x50 && (size_t)buf[lho+1]==0x4b) {
            unsigned lnlen = (unsigned)rd16(lho + 26);
            unsigned lelen = (unsigned)rd16(lho + 28);
            size_t ds = lho + 30 + lnlen + lelen;
            if (ds + csize <= buf.size()) {
                Entry e; e.name = name;
                if (method == 0) e.data.assign(buf.begin()+ds, buf.begin()+ds+csize);
                else if (method == 8) {
                    uLongf dlen = usize ? usize : csize * 4 + 1024;
                    std::vector<unsigned char> raw(dlen);
                    int rc = uncompress(raw.data(), &dlen, &buf[ds], csize);
                    if (rc == Z_OK) raw.resize(dlen);
                    else raw.clear();
                    e.data = std::move(raw);
                }
                out.push_back(std::move(e));
            }
        }
        p += 46 + nlen + rd16(p + 30) + rd16(p + 32); // +extra +comment
    }
    return out;
}
static void write_archive(const std::string& path, const std::vector<Entry>& es) {
    std::vector<unsigned char> out;
    for (size_t i = 0; i < es.size(); ++i) {
        const Entry& e = es[i];
        // compress with deflate if beneficial
        uLongf clen = compressBound(e.data.size());
        std::vector<unsigned char> c(e.data.empty() ? 0 : clen);
        unsigned method = 0; unsigned csize = (unsigned)e.data.size(); unsigned usize = (unsigned)e.data.size();
        if (!e.data.empty()) {
            int rc = compress2(c.data(), &clen, e.data.data(), e.data.size(), 6);
            if (rc == Z_OK && clen < e.data.size()) { method = 8; csize = (unsigned)clen; usize = (unsigned)e.data.size(); }
        }
        unsigned crc = (unsigned)crc32(0L, Z_NULL, 0);
        if (!e.data.empty()) crc = (unsigned)crc32(crc, e.data.data(), e.data.size());
        unsigned lho = (unsigned)out.size();
        // local header
        auto put16 = [&](unsigned v){ out.push_back(v&0xff); out.push_back((v>>8)&0xff); };
        auto put32 = [&](unsigned v){ out.push_back(v&0xff); out.push_back((v>>8)&0xff); out.push_back((v>>16)&0xff); out.push_back((v>>24)&0xff); };
        put32(0x04034b50); put16(20); put16(0x0800); put16(method);
        put16(0); put16(0); put32(crc); put32(csize); put32(usize);
        put16((unsigned)e.name.size()); put16(0);
        out.insert(out.end(), e.name.begin(), e.name.end());
        if (method==0) out.insert(out.end(), e.data.begin(), e.data.end());
        else out.insert(out.end(), c.begin(), c.begin()+csize);
        // central dir 用到的 per-entry 元数据（按 es 索引顺序）
        cdir_lho.push_back(lho);
        cdir_info.push_back({crc, method, csize, usize});
    }
    // central directory
    unsigned cd_start = (unsigned)out.size();
    for (size_t j = 0; j < es.size(); ++j) {
        const Entry& e = es[j];
        auto cdi = cdir_info[j];
        auto lho = cdir_lho[j];
        auto put16 = [&](unsigned v){ out.push_back(v&0xff); out.push_back((v>>8)&0xff); };
        auto put32 = [&](unsigned v){ out.push_back(v&0xff); out.push_back((v>>8)&0xff); out.push_back((v>>16)&0xff); out.push_back((v>>24)&0xff); };
        put32(0x02014b50); put16(20); put16(20); put16(0x0800); put16(std::get<1>(cdi));
        put16(0); put16(0); put32(std::get<0>(cdi)); put32(std::get<2>(cdi)); put32(std::get<3>(cdi));
        put16((unsigned)e.name.size()); put16(0); put16(0); put16(0); put16(0);
        put32(0); put32(lho);
        out.insert(out.end(), e.name.begin(), e.name.end());
    }
    unsigned cd_size = (unsigned)out.size() - cd_start;
    // EOCD
    auto put16 = [&](unsigned v){ out.push_back(v&0xff); out.push_back((v>>8)&0xff); };
    auto put32 = [&](unsigned v){ out.push_back(v&0xff); out.push_back((v>>8)&0xff); out.push_back((v>>16)&0xff); out.push_back((v>>24)&0xff); };
    put32(0x06054b50); put16(0); put16(0); put16((unsigned)es.size()); put16((unsigned)es.size());
    put32(cd_size); put32(cd_start); put16(0);
    std::ofstream f(path, std::ios::binary);
    f.write((char*)out.data(), (std::streamsize)out.size());
}
} // namespace vortzip

void vor_zip_add(VStr* zipPath, VStr* name, VStr* data) {
    using namespace vortzip;
    auto es = read_archive(zipPath ? vstr_str(zipPath) : "");
    std::string n = vstr_str(name);
    std::string d = vstr_str(data);
    // replace existing entry with same name
    bool replaced = false;
    for (auto& e : es) if (e.name == n) { e.data.assign(d.begin(), d.end()); replaced = true; }
    if (!replaced) { Entry e; e.name = n; e.data.assign(d.begin(), d.end()); es.push_back(std::move(e)); }
    std::sort(es.begin(), es.end(), [](const Entry& a, const Entry& b){ return a.name < b.name; });
    cdir_lho.clear(); cdir_info.clear();
    write_archive(zipPath ? vstr_str(zipPath) : "", es);
}
VStr* vor_zip_extract(VStr* zipPath, VStr* name) {
    using namespace vortzip;
    auto es = read_archive(zipPath ? vstr_str(zipPath) : "");
    std::string nm = vstr_str(name);
    for (const auto& e : es) if (e.name == nm) {
        return vor_str_from_bytes((const char*)e.data.data(), (int)e.data.size());
    }
    throw_rt("zip.extract: entry not found");
}
long long vor_zip_count(VStr* zipPath) {
    using namespace vortzip;
    return (long long)read_archive(zipPath ? vstr_str(zipPath) : "").size();
}
int vor_zip_has(VStr* zipPath, VStr* name) {
    using namespace vortzip;
    auto es = read_archive(zipPath ? vstr_str(zipPath) : "");
    for (const auto& e : es) if (e.name == vstr_str(name)) return 1;
    return 0;
}
VList* vor_zip_names(VStr* zipPath) {
    using namespace vortzip;
    auto es = read_archive(zipPath ? vstr_str(zipPath) : "");
    std::sort(es.begin(), es.end(), [](const Entry& x, const Entry& y){ return x.name < y.name; });
    VList* l = vor_list_new();
    for (const auto& e : es) vor_list_push(l, (long long)vor_str_from_cstr(e.name.c_str()));
    return l;
}

// ========== xml 模块转发（escape/unescape/parse_text） ==========
static VStr* xml_rt_escape(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (char ch : s) {
        switch (ch) {
            case '&': r += "&amp;"; break;
            case '<': r += "&lt;"; break;
            case '>': r += "&gt;"; break;
            case '"': r += "&quot;"; break;
            case '\'': r += "&apos;"; break;
            default: r += ch;
        }
    }
    return vor_str_from_cstr(r.c_str());
}
static std::string xml_rt_unescape_str(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            size_t semi = s.find(';', i);
            if (semi != std::string::npos && semi - i <= 8) {
                std::string ent = s.substr(i + 1, semi - i - 1);
                if (ent == "amp") { r += '&'; i = semi; continue; }
                if (ent == "lt")  { r += '<'; i = semi; continue; }
                if (ent == "gt")  { r += '>'; i = semi; continue; }
                if (ent == "quot"){ r += '"'; i = semi; continue; }
                if (ent == "apos"){ r += '\''; i = semi; continue; }
                if (!ent.empty() && ent[0] == '#') {
                    unsigned long code = 0; bool ok = true;
                    for (size_t j = 1; j < ent.size(); ++j) {
                        if (ent[j] < '0' || ent[j] > '9') { ok = false; break; }
                        code = code * 10 + (unsigned long)(ent[j] - '0');
                    }
                    if (ok && code > 0) { r += (char)code; i = semi; continue; }
                }
            }
            r += '&';
        } else {
            r += s[i];
        }
    }
    return r;
}
VStr* vor_xml_escape(VStr* s) {
    return xml_rt_escape(vstr_str(s));
}
VStr* vor_xml_unescape(VStr* s) {
    return vor_str_from_cstr(xml_rt_unescape_str(vstr_str(s)).c_str());
}
VStr* vor_xml_parse_text(VStr* xml, VStr* tag) {
    std::string x = vstr_str(xml);
    std::string tg = vstr_str(tag);
    std::string open = "<" + tg;
    size_t pos = x.find(open);
    if (pos == std::string::npos) return vor_str_from_cstr("");
    size_t gt = x.find('>', pos);
    if (gt == std::string::npos) return vor_str_from_cstr("");
    size_t close = x.find("</" + tg + ">", gt + 1);
    if (close == std::string::npos) return vor_str_from_cstr("");
    return vor_str_from_cstr(xml_rt_unescape_str(x.substr(gt + 1, close - gt - 1)).c_str());
}

// ========== html 模块转发（escape/unescape/strip_tags） ==========
static std::string html_rt_unescape_str(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            size_t semi = s.find(';', i);
            if (semi != std::string::npos && semi - i <= 12) {
                std::string ent = s.substr(i + 1, semi - i - 1);
                if (ent == "amp")   { r += '&'; i = semi; continue; }
                if (ent == "lt")    { r += '<'; i = semi; continue; }
                if (ent == "gt")    { r += '>'; i = semi; continue; }
                if (ent == "quot")  { r += '"'; i = semi; continue; }
                if (ent == "apos" || ent == "#39") { r += '\''; i = semi; continue; }
                if (ent == "nbsp")  { r += ' '; i = semi; continue; }
                if (!ent.empty() && ent[0] == '#') {
                    bool ok = true; unsigned long code = 0;
                    for (size_t j = 1; j < ent.size(); ++j) {
                        if (ent[j] < '0' || ent[j] > '9') { ok = false; break; }
                        code = code * 10 + (unsigned long)(ent[j] - '0');
                    }
                    if (ok && code > 0) { r += (char)code; i = semi; continue; }
                }
            }
            r += '&';
        } else {
            r += s[i];
        }
    }
    return r;
}
VStr* vor_html_escape(VStr* s) {
    std::string in = vstr_str(s), r; r.reserve(in.size());
    for (char ch : in) {
        switch (ch) {
            case '&': r += "&amp;"; break;
            case '<': r += "&lt;"; break;
            case '>': r += "&gt;"; break;
            case '"': r += "&quot;"; break;
            case '\'': r += "&#39;"; break;
            default: r += ch;
        }
    }
    return vor_str_from_cstr(r.c_str());
}
VStr* vor_html_unescape(VStr* s) {
    return vor_str_from_cstr(html_rt_unescape_str(vstr_str(s)).c_str());
}
VStr* vor_html_strip_tags(VStr* s) {
    std::string in = vstr_str(s), r; r.reserve(in.size());
    bool in_tag = false;
    for (char ch : in) {
        if (ch == '<') { in_tag = true; continue; }
        if (ch == '>') { in_tag = false; continue; }
        if (!in_tag) r += ch;
    }
    return vor_str_from_cstr(html_rt_unescape_str(r).c_str());
}

// ========== sql 模块转发（sqlite3；句柄以不透明指针传递） ==========
namespace vortsql {
struct Conn { sqlite3* db = nullptr; };
static std::mutex sql_mtx;
static std::vector<Conn*> alive;
static Conn* lookup(void* h) {
    std::lock_guard<std::mutex> lk(sql_mtx);
    for (auto p : alive) if (p == h) return p;
    return nullptr;
}
} // namespace vortsql
void* vor_sql_open(VStr* path) {
    using namespace vortsql;
    auto c = new Conn();
    int rc = sqlite3_open(vstr_str(path).c_str(), &c->db);
    if (rc != SQLITE_OK) {
        std::string m = c->db ? sqlite3_errmsg(c->db) : "unknown";
        sqlite3_close(c->db);
        delete c;
        throw_rt(("sql.open: " + m).c_str());
    }
    { std::lock_guard<std::mutex> lk(sql_mtx); alive.push_back(c); }
    return (void*)c;
}
void vor_sql_close(void* h) {
    using namespace vortsql;
    Conn* c = lookup(h);
    if (!c || !c->db) { throw_rt("sql.close: invalid handle"); return; }
    sqlite3_close(c->db);
    c->db = nullptr;
    { std::lock_guard<std::mutex> lk(sql_mtx); auto& v = alive; v.erase(std::remove(v.begin(), v.end(), c), v.end()); }
    delete c;
}
long long vor_sql_execute(void* h, VStr* sql) {
    using namespace vortsql;
    Conn* c = lookup(h);
    if (!c || !c->db) { throw_rt("sql.execute: invalid handle"); return 0; }
    char* err = nullptr;
    int rc = sqlite3_exec(c->db, vstr_str(sql).c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string m = err ? err : (c->db ? sqlite3_errmsg(c->db) : "exec failed");
        if (err) sqlite3_free(err);
        throw_rt(("sql.execute: " + m).c_str());
    }
    return (long long)sqlite3_changes(c->db);
}
long long vor_sql_table_exists(void* h, VStr* name) {
    using namespace vortsql;
    Conn* c = lookup(h);
    if (!c || !c->db) { throw_rt("sql.table_exists: invalid handle"); return 0; }
    sqlite3_stmt* st = nullptr;
    std::string q = "SELECT name FROM sqlite_master WHERE type='table' AND name=?1";
    if (sqlite3_prepare_v2(c->db, q.c_str(), -1, &st, nullptr) != SQLITE_OK)
        throw_rt("sql.table_exists: prepare failed");
    sqlite3_bind_text(st, 1, vstr_str(name).c_str(), -1, SQLITE_TRANSIENT);
    int found = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return found;
}

// ========== os 模块转发（getenv/setenv/cwd/pid/platform/home/tempdir/path_join） ==========
VStr* vor_os_getenv(VStr* name) {
    const char* v = std::getenv(vstr_str(name).c_str());
    return vor_str_from_cstr(v ? v : "");
}
int vor_os_hasenv(VStr* name) {
    return std::getenv(vstr_str(name).c_str()) ? 1 : 0;
}
int vor_os_setenv(VStr* name, VStr* val) {
#ifdef _WIN32
    std::string kv = vstr_str(name) + "=" + vstr_str(val);
    return _putenv(kv.c_str()) == 0 ? 1 : 0;
#else
    return setenv(vstr_str(name).c_str(), vstr_str(val).c_str(), 1) == 0 ? 1 : 0;
#endif
}
int vor_os_unsetenv(VStr* name) {
#ifdef _WIN32
    std::string kv = vstr_str(name) + "=";
    return _putenv(kv.c_str()) == 0 ? 1 : 0;
#else
    return unsetenv(vstr_str(name).c_str()) == 0 ? 1 : 0;
#endif
}
VStr* vor_os_cwd(void) {
    std::error_code ec;
    return vor_str_from_cstr(fs::current_path(ec).string().c_str());
}
int vor_os_chdir(VStr* path) {
    std::error_code ec;
    fs::current_path(vstr_str(path), ec);
    return ec ? 0 : 1;
}
long long vor_os_pid(void) {
#ifdef _WIN32
    return (long long)(uintptr_t)GetCurrentProcessId();
#else
    return (long long)getpid();
#endif
}
VStr* vor_os_platform(void) {
#ifdef _WIN32
    return vor_str_from_cstr("windows");
#else
    return vor_str_from_cstr("linux");
#endif
}
VStr* vor_os_home(void) {
#ifdef _WIN32
    const char* h = std::getenv("USERPROFILE");
#else
    const char* h = std::getenv("HOME");
#endif
    return vor_str_from_cstr(h ? h : "");
}
VStr* vor_os_tempdir(void) {
#ifdef _WIN32
    const char* t = std::getenv("TEMP");
    if (!t) t = std::getenv("TMP");
#else
    const char* t = std::getenv("TMPDIR");
    if (!t) t = "/tmp";
#endif
    return vor_str_from_cstr(t ? t : "");
}
VStr* vor_os_path_join(VStr* a, VStr* b) {
    fs::path p(vstr_str(a));
    p /= vstr_str(b);
    return vor_str_from_cstr(p.generic_string().c_str());
}

// ========== regex 模块转发（std::regex，全标量） ==========
#include <regex>
int vor_regex_valid(VStr* pat) {
    try { std::regex r(vstr_str(pat)); return 1; }
    catch (...) { return 0; }
}
int vor_regex_match(VStr* pat, VStr* s) {
    try { return std::regex_match(vstr_str(s), std::regex(vstr_str(pat))) ? 1 : 0; }
    catch (...) { return 0; }
}
int vor_regex_search(VStr* pat, VStr* s) {
    try { return std::regex_search(vstr_str(s), std::regex(vstr_str(pat))) ? 1 : 0; }
    catch (...) { return 0; }
}
VStr* vor_regex_find(VStr* pat, VStr* s) {
    std::string text = vstr_str(s);
    try {
        std::smatch m;
        if (std::regex_search(text, m, std::regex(vstr_str(pat))))
            return vor_str_from_cstr(m.str().c_str());
    } catch (...) {}
    return vor_str_from_cstr("");
}
VStr* vor_regex_find_all(VStr* pat, VStr* s) {
    std::string out;
    try {
        std::string text = vstr_str(s);
        std::regex re(vstr_str(pat));
        auto b = std::sregex_iterator(text.begin(), text.end(), re);
        auto e = std::sregex_iterator();
        for (auto it = b; it != e; ++it) {
            if (!out.empty()) out += ", ";
            out += it->str();
        }
    } catch (...) {}
    return vor_str_from_cstr(out.c_str());
}
VStr* vor_regex_replace(VStr* pat, VStr* s, VStr* repl) {
    try { return vor_str_from_cstr(std::regex_replace(vstr_str(s), std::regex(vstr_str(pat)), vstr_str(repl)).c_str()); }
    catch (...) { return vor_str_from_cstr(vstr_str(s).c_str()); }
}
long long vor_regex_count(VStr* pat, VStr* s) {
    int n = 0;
    try {
        std::string text = vstr_str(s);
        std::regex re(vstr_str(pat));
        auto b = std::sregex_iterator(text.begin(), text.end(), re);
        auto e = std::sregex_iterator();
        for (auto it = b; it != e; ++it) ++n;
    } catch (...) {}
    return n;
}
static const char* rt_regex_meta = R"(\.^$|?*+()[]{})";
static int rt_regex_is_meta(char c) {
    const char* p = rt_regex_meta;
    while (*p) { if (*p == c) return 1; ++p; }
    return 0;
}
VStr* vor_regex_escape(VStr* s) {
    std::string text = vstr_str(s);
    std::string out;
    out.reserve(text.size() * 2);
    for (char c : text) {
        if (rt_regex_is_meta(c)) out += '\\';
        out += c;
    }
    return vor_str_from_cstr(out.c_str());
}
VList* vor_regex_split(VStr* pat, VStr* s) {
    VList* l = vor_list_new();
    if (!pat || !s) return l;
    try {
        std::regex re(vstr_str(pat));
        std::string text = vstr_str(s);
        auto begin = std::sregex_token_iterator(text.begin(), text.end(), re, -1);
        auto end = std::sregex_token_iterator();
        for (auto it = begin; it != end; ++it)
            vor_list_push(l, (long long)vor_str_from_bytes(it->str().data(), (int)it->str().size()));
    } catch (...) {
        vor_list_push(l, (long long)vor_str_from_bytes(s->data, s->len));
    }
    return l;
}

// ========== json 模块转发（标量导向：valid/parse_str/parse_int/parse_float/parse_bool/stringify_*） ==========
static void json_skip_ws(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) ++i;
}
static bool json_parse_str_at_(const std::string& s, size_t& i, std::string& out) {
    json_skip_ws(s, i);
    if (i >= s.size() || s[i] != '"') return false;
    ++i; out.clear();
    while (i < s.size()) {
        char c = s[i];
        if (c == '"') { ++i; return true; }
        if (c == '\\') {
            if (i+1 >= s.size()) return false;
            char e = s[i+1];
            switch (e) {
                case 'n': out += '\n'; break; case 't': out += '\t'; break;
                case 'r': out += '\r'; break; case 'b': out += '\b'; break;
                case 'f': out += '\f'; break; case '"': out += '"'; break;
                case '\\': out += '\\'; break; case '/': out += '/'; break;
                case 'u': {
                    if (i+6 > s.size()) return false;
                    unsigned code = 0;
                    for (int k=1;k<=4;++k){ char h=s[i+1+k]; code<<=4;
                        if(h>='0'&&h<='9')code|=(unsigned)(h-'0');
                        else if(h>='a'&&h<='f')code|=(unsigned)(h-'a'+10);
                        else if(h>='A'&&h<='F')code|=(unsigned)(h-'A'+10);
                        else return false; }
                    out += (char)code; i += 4; break;
                }
                default: return false;
            }
            i += 2;
        } else { out += c; ++i; }
    }
    return false;
}
static long long json_str_to_ll(const VStr* s) {
    try { return std::stoll(vstr_str(s)); } catch (...) { return 0; }
}
static double json_str_to_d(const VStr* s) {
    try { return std::stod(vstr_str(s)); } catch (...) { return 0.0; }
}
int vor_json_valid(VStr* s) {
    std::string src = vstr_str(s);
    size_t i = 0; json_skip_ws(src, i);
    if (i >= src.size()) return 0;
    char c = src[i];
    if (c == '"') { std::string t; return json_parse_str_at_(src, i, t) ? 1 : 0; }
    if (c == '{' || c == '[') {
        int depth = 0; bool in_str = false; char prev = 0;
        for (size_t j = i; j < src.size(); ++j) {
            char ch = src[j];
            if (in_str) { if (ch=='"' && prev!='\\') in_str=false; }
            else {
                if (ch=='"') in_str=true;
                else if (ch=='{'||ch=='[') ++depth;
                else if (ch=='}'||ch==']'){ --depth; if(depth<0) return 0; }
            }
            prev = ch;
        }
        return depth == 0 ? 1 : 0;
    }
    char* end=nullptr; (void)std::strtod(src.c_str()+i, &end);
    return (end != src.c_str()+i) ? 1 : 0;
}
VStr* vor_json_parse_str(VStr* s) {
    size_t i = 0; std::string o;
    if (!json_parse_str_at_(vstr_str(s), i, o)) throw_rt("json.parse_str: not a JSON string");
    return vor_str_from_cstr(o.c_str());
}
long long vor_json_parse_int(VStr* s) {
    return json_str_to_ll(s);
}
double vor_json_parse_float(VStr* s) {
    return json_str_to_d(s);
}
int vor_json_parse_bool(VStr* s) {
    std::string src = vstr_str(s);
    size_t i = 0; json_skip_ws(src, i);
    if (src.compare(i, 4, "true") == 0) return 1;
    return 0;
}
// 跳过任意 JSON 值（含嵌套），i 停在值结束后的位置
static void json_skip_value_(const std::string& s, size_t& i) {
    json_skip_ws(s, i);
    if (i >= s.size()) return;
    if (s[i] == '"') { std::string t; json_parse_str_at_(s, i, t); return; }
    if (s[i] == '{') {
        ++i; int d = 0;
        while (i < s.size()) {
            if (s[i]=='{') ++d; else if (s[i]=='}'){ --d; if(d<=0){ ++i; break; } }
            i++;
        }
        return;
    }
    if (s[i] == '[') {
        ++i; int d = 0;
        while (i < s.size()) {
            if (s[i]=='[') ++d; else if (s[i]==']'){ --d; if(d<=0){ ++i; break; } }
            i++;
        }
        return;
    }
    while (i < s.size() && s[i]!=',' && s[i]!='}' && s[i]!=']'
           && s[i]!=' ' && s[i]!='\t' && s[i]!='\n' && s[i]!='\r') ++i;
}
VList* vor_json_parse_array(VStr* s) {
    VList* l = vor_list_new();
    std::string src = vstr_str(s);
    size_t i = 0; json_skip_ws(src, i);
    if (i >= src.size() || src[i] != '[') return l;
    ++i;
    json_skip_ws(src, i);
    if (i < src.size() && src[i] == ']') return l;  // 空数组
    while (i < src.size()) {
        json_skip_ws(src, i);
        if (i >= src.size()) break;
        if (src[i] == '"') {
            std::string str;
            if (!json_parse_str_at_(src, i, str)) break;
            vor_list_push(l, (long long)vor_str_from_cstr(str.c_str()));
        } else if (src[i] == '[' || src[i] == '{') {
            json_skip_value_(src, i);  // 嵌套值不支持入表，跳过
        } else {
            size_t st = i;
            while (i < src.size() && src[i]!=',' && src[i]!=']'
                   && src[i]!=' ' && src[i]!='\t' && src[i]!='\n' && src[i]!='\r') ++i;
            vor_list_push(l, (long long)vor_str_from_bytes(src.data()+st, (int)(i-st)));
        }
        json_skip_ws(src, i);
        if (i < src.size() && src[i] == ',') { ++i; continue; }
        if (i < src.size() && src[i] == ']') break;
        break;
    }
    return l;
}
// get(s, key)：读取 JSON 对象顶层指定键的值；字符串解码，其余返回字面文本
VStr* vor_json_get(VStr* s, VStr* key) {
    std::string src = vstr_str(s);
    std::string k = key ? vstr_str(key) : std::string();
    size_t i = 0; json_skip_ws(src, i);
    if (i >= src.size() || src[i] != '{') return vor_str_from_cstr("");
    ++i;
    while (i < src.size()) {
        json_skip_ws(src, i);
        std::string member;
        if (!json_parse_str_at_(src, i, member)) break;   // 键
        json_skip_ws(src, i);
        if (i >= src.size() || src[i] != ':') break;
        ++i;
        json_skip_ws(src, i);
        if (i >= src.size()) break;
        if (member == k) {
            if (src[i] == '"') {
                std::string v;
                if (json_parse_str_at_(src, i, v)) return vor_str_from_cstr(v.c_str());
                return vor_str_from_cstr("");
            }
            if (src[i] == '{' || src[i] == '[') { json_skip_value_(src, i); return vor_str_from_cstr(""); }
            size_t st = i;
            while (i < src.size() && src[i]!=',' && src[i]!='}'
                   && src[i]!=' ' && src[i]!='\t' && src[i]!='\n' && src[i]!='\r') ++i;
            return vor_str_from_bytes(src.data()+st, (int)(i-st));
        }
        json_skip_value_(src, i);
        json_skip_ws(src, i);
        if (i < src.size() && src[i] == ',') { ++i; continue; }
        if (i < src.size() && src[i] == '}') break;
        break;
    }
    return vor_str_from_cstr("");
}
VStr* vor_json_stringify_str(VStr* s) {
    std::string r = "\"";
    for (char ch : vstr_str(s)) {
        switch (ch) {
            case '"': r += "\\\""; break; case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break; case '\t': r += "\\t"; break;
            case '\r': r += "\\r"; break; default: r += ch;
        }
    }
    r += "\"";
    return vor_str_from_cstr(r.c_str());
}
VStr* vor_json_stringify_int(long long v) {
    return vor_str_from_cstr(std::to_string(v).c_str());
}
VStr* vor_json_stringify_float(double v) {
    char buf[40]; snprintf(buf, sizeof(buf), "%.15g", v);
    return vor_str_from_cstr(buf);
}
VStr* vor_json_stringify_bool(int b) {
    return vor_str_from_cstr(b ? "true" : "false");
}

// ========== base64 模块转发（encode/decode，str <-> str） ==========
static const char b64enc_tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string b64_rt_encode(const std::string& in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= in.size()) {
        unsigned v = ((unsigned)(unsigned char)in[i] << 16) |
                     ((unsigned)(unsigned char)in[i+1] << 8)  |
                     ((unsigned)(unsigned char)in[i+2]);
        out += b64enc_tab[(v >> 18) & 63];
        out += b64enc_tab[(v >> 12) & 63];
        out += b64enc_tab[(v >> 6) & 63];
        out += b64enc_tab[v & 63];
        i += 3;
    }
    size_t rem = in.size() - i;
    if (rem == 1) {
        unsigned v = (unsigned)(unsigned char)in[i] << 16;
        out += b64enc_tab[(v >> 18) & 63];
        out += b64enc_tab[(v >> 12) & 63];
        out += "==";
    } else if (rem == 2) {
        unsigned v = ((unsigned)(unsigned char)in[i] << 16) |
                     ((unsigned)(unsigned char)in[i+1] << 8);
        out += b64enc_tab[(v >> 18) & 63];
        out += b64enc_tab[(v >> 12) & 63];
        out += b64enc_tab[(v >> 6) & 63];
        out += "=";
    }
    return out;
}
static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static std::string b64_rt_decode(const std::string& in) {
    std::string out;
    out.reserve((in.size() / 4) * 3);
    int buf = 0, bits = 0;
    for (char c : in) {
        if (c == '=' || c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        int v = b64val(c);
        if (v < 0) return out;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)((buf >> bits) & 0xFF); }
    }
    return out;
}
VStr* vor_base64_encode(VStr* s) {
    return vor_str_from_cstr(b64_rt_encode(vstr_str(s)).c_str());
}
VStr* vor_base64_decode(VStr* s) {
    return vor_str_from_cstr(b64_rt_decode(vstr_str(s)).c_str());
}

// ========== datetime 模块转发（ymd/to_iso/from_iso/today/add_days/days_between） ==========
static long long dt_days_from_civil(int y, unsigned m, unsigned d) {
    y -= (int)(m <= 2);
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}
static void dt_civil_from_days(long long z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long yy = (long long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y = (int)(yy + (m <= 2));
}
static bool dt_parse_(const std::string& iso, int& y, int& m, int& d) {
    y = m = d = 0; size_t i = 0;
    auto num = [&](int& out) { out = 0; int cnt = 0;
        while (i < iso.size() && iso[i] >= '0' && iso[i] <= '9') { out = out * 10 + (iso[i]-'0'); ++i; ++cnt; }
        return cnt > 0; };
    if (!num(y)) return false;
    if (i < iso.size() && iso[i] == '-') ++i; else return false;
    if (!num(m)) return false;
    if (i < iso.size() && iso[i] == '-') ++i; else return false;
    if (!num(d)) return false;
    return m >= 1 && m <= 12 && d >= 1 && d <= 31;
}
static VStr* dt_days_to_iso_(long long days) {
    int y; unsigned mo, d;
    dt_civil_from_days(days, y, mo, d);
    char buf[16]; snprintf(buf, sizeof(buf), "%04d-%02u-%02u", y, mo, d);
    return vor_str_from_cstr(buf);
}
VStr* vor_datetime_ymd(long long y, long long mo, long long d) {
    if (mo < 1 || mo > 12) throw_rt("datetime.ymd: bad month");
    return dt_days_to_iso_(dt_days_from_civil((int)y, (unsigned)mo, (unsigned)d));
}
VStr* vor_datetime_to_iso(long long epoch) {
    return dt_days_to_iso_(epoch / 86400);
}
long long vor_datetime_from_iso(VStr* iso) {
    int y, m, d;
    if (!dt_parse_(vstr_str(iso), y, m, d)) throw_rt("datetime.from_iso: bad ISO date");
    return dt_days_from_civil(y, (unsigned)m, (unsigned)d) * 86400;
}
VStr* vor_datetime_today(void) {
    std::time_t t = std::time(nullptr);
    std::tm g = *std::gmtime(&t);
    return dt_days_to_iso_(dt_days_from_civil(g.tm_year + 1900, (unsigned)(g.tm_mon + 1), (unsigned)g.tm_mday));
}
VStr* vor_datetime_add_days(VStr* iso, long long n) {
    int y, m, d;
    if (!dt_parse_(vstr_str(iso), y, m, d)) throw_rt("datetime.add_days: bad ISO date");
    return dt_days_to_iso_(dt_days_from_civil(y, (unsigned)m, (unsigned)d) + n);
}
long long vor_datetime_days_between(VStr* a, VStr* b) {
    int y1, m1, d1, y2, m2, d2;
    if (!dt_parse_(vstr_str(a), y1, m1, d1) || !dt_parse_(vstr_str(b), y2, m2, d2))
        throw_rt("datetime.days_between: bad ISO date");
    return dt_days_from_civil(y2, (unsigned)m2, (unsigned)d2) - dt_days_from_civil(y1, (unsigned)m1, (unsigned)d1);
}

// ========== csv 模块转发（quote / count_fields / field_at；to_line 由前端拼） ==========
static char csv_sep(VStr* sep) {
    if (sep) { const std::string& s = vstr_str(sep); if (!s.empty()) return s[0]; }
    return ',';
}
static bool csv_rt_meta(const std::string& f, char sep) {
    for (char c : f)
        if (c == sep || c == '"' || c == '\n' || c == '\r') return true;
    return false;
}
static std::string csv_rt_quote_field(const std::string& f, char sep) {
    if (!csv_rt_meta(f, sep)) return f;
    std::string r = "\"";
    for (char c : f) {
        if (c == '"') r += "\"\"";
        else r += c;
    }
    r += "\"";
    return r;
}
static std::vector<std::string> csv_rt_split(const std::string& line, char sep) {
    std::vector<std::string> out;
    std::string cur;
    bool in_q = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (in_q) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i+1] == '"') { cur += '"'; ++i; }
                else in_q = false;
            } else cur += c;
        } else {
            if (c == '"') in_q = true;
            else if (c == sep) { out.push_back(cur); cur.clear(); }
            else cur += c;
        }
    }
    out.push_back(cur);
    return out;
}
VStr* vor_csv_quote(VStr* f, VStr* sep) {
    return vor_str_from_cstr(csv_rt_quote_field(vstr_str(f), csv_sep(sep)).c_str());
}
long long vor_csv_count_fields(VStr* line, VStr* sep) {
    return (long long)csv_rt_split(vstr_str(line), csv_sep(sep)).size();
}
VStr* vor_csv_field_at(VStr* line, long long index, VStr* sep) {
    auto f = csv_rt_split(vstr_str(line), csv_sep(sep));
    if (index < 0 || index >= (long long)f.size()) throw_rt("csv.field_at: index out of range");
    return vor_str_from_cstr(f[(size_t)index].c_str());
}
VList* vor_csv_parse_row(VStr* line, VStr* sep) {
    auto f = csv_rt_split(vstr_str(line), csv_sep(sep));
    VList* l = vor_list_new();
    for (const auto& s : f) vor_list_push(l, (long long)vor_str_from_cstr(s.c_str()));
    return l;
}

// ========== hash 模块转发（md5/sha1/sha256，输出小写十六进制） ==========
static inline uint32_t rl32(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }
static void hash_hex_out(const uint8_t* d, size_t n, std::string& out) {
    static const char* HEX = "0123456789abcdef";
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { out += HEX[(d[i] >> 4) & 0xf]; out += HEX[d[i] & 0xf]; }
}
static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void be32s(uint8_t* p, uint32_t v) { p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }

static void rt_md5(const uint8_t* msg, size_t len, uint8_t out[16]) {
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
    static const int S[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
    uint32_t st[4] = {0x67452301,0xefcdab89,0x98badcfe,0x10325476};
    size_t padded = ((len + 8) / 64 + 1) * 64;
    std::vector<uint8_t> buf(padded, 0);
    if (len) std::memcpy(buf.data(), msg, len);
    buf[len] = 0x80;
    uint64_t bitlen = (uint64_t)len * 8;
    for (int i = 0; i < 8; ++i) buf[padded - 8 + i] = (uint8_t)(bitlen >> (8 * i));
    for (size_t off = 0; off < padded; off += 64) {
        uint32_t x[16];
        for (int i = 0; i < 16; ++i)
            x[i] = (uint32_t)buf[off+i*4] | ((uint32_t)buf[off+i*4+1]<<8) | ((uint32_t)buf[off+i*4+2]<<16) | ((uint32_t)buf[off+i*4+3]<<24);
        uint32_t A=st[0], B=st[1], C=st[2], D=st[3];
        for (int i = 0; i < 64; ++i) {
            uint32_t F, g;
            if (i < 16)      { F = (B & C) | (~B & D);       g = i; }
            else if (i < 32) { F = (D & B) | (~D & C);       g = (5*i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D;                g = (3*i + 5) % 16; }
            else             { F = C ^ (B | ~D);             g = (7*i) % 16; }
            F = F + A + K[i] + x[g];
            A = D; D = C; C = B;
            B = B + rl32(F, S[i]);
        }
        st[0]+=A; st[1]+=B; st[2]+=C; st[3]+=D;
    }
    for (int i = 0; i < 4; ++i) {
        out[i*4]=(uint8_t)st[i]; out[i*4+1]=(uint8_t)(st[i]>>8); out[i*4+2]=(uint8_t)(st[i]>>16); out[i*4+3]=(uint8_t)(st[i]>>24);
    }
}
static void rt_sha1(const uint8_t* msg, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};
    size_t padded = ((len + 9) / 64 + 1) * 64;
    std::vector<uint8_t> buf(padded, 0);
    if (len) std::memcpy(buf.data(), msg, len);
    buf[len] = 0x80;
    uint64_t bitlen = (uint64_t)len * 8;
    for (int i = 0; i < 8; ++i) buf[padded - 8 + i] = (uint8_t)(bitlen >> (8*(7-i)));
    for (size_t off = 0; off < padded; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) w[i] = be32(&buf[off + i*4]);
        for (int i = 16; i < 80; ++i) { uint32_t t = w[i-3]^w[i-8]^w[i-14]^w[i-16]; w[i] = rl32(t,1); }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i<20)      { f=(b&c)|(~b&d);      k=0x5A827999; }
            else if (i<40) { f=b^c^d;             k=0x6ED9EBA1; }
            else if (i<60) { f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDC; }
            else           { f=b^c^d;             k=0xCA62C1D6; }
            uint32_t temp = rl32(a,5) + f + e + k + w[i];
            e=d; d=c; c=rl32(b,30); b=a; a=temp;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
    }
    for (int i = 0; i < 5; ++i) be32s(out + i*4, h[i]);
}
static void rt_sha256(const uint8_t* msg, size_t len, uint8_t out[32]) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    size_t padded = ((len + 9) / 64 + 1) * 64;
    std::vector<uint8_t> buf(padded, 0);
    if (len) std::memcpy(buf.data(), msg, len);
    buf[len] = 0x80;
    uint64_t bitlen = (uint64_t)len * 8;
    for (int i = 0; i < 8; ++i) buf[padded - 8 + i] = (uint8_t)(bitlen >> (8*(7-i)));
    for (size_t off = 0; off < padded; off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = be32(&buf[off + i*4]);
        for (int i = 16; i < 64; ++i) {
            uint32_t t0 = w[i-15], t1 = w[i-2];
            uint32_t s0 = ((t0>>7)|(t0<<25)) ^ ((t0>>18)|(t0<<14)) ^ (t0>>3);
            uint32_t s1 = ((t1>>17)|(t1<<15)) ^ ((t1>>19)|(t1<<13)) ^ (t1>>10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],H=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = ((e>>6)|(e<<26)) ^ ((e>>11)|(e<<21)) ^ ((e>>25)|(e<<7));
            uint32_t ch = (e&f)^(~e&g);
            uint32_t t1 = H + S1 + ch + K[i] + w[i];
            uint32_t S0 = ((a>>2)|(a<<30)) ^ ((a>>13)|(a<<19)) ^ ((a>>22)|(a<<10));
            uint32_t maj = (a&b)^(a&c)^(b&c);
            uint32_t t2 = S0 + maj;
            H=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=H;
    }
    for (int i = 0; i < 8; ++i) be32s(out + i*4, h[i]);
}
VStr* vor_hash_md5(VStr* s) {
    std::string in = vstr_str(s), hex; uint8_t d[16];
    rt_md5((const uint8_t*)in.data(), in.size(), d); hash_hex_out(d, 16, hex);
    return vor_str_from_cstr(hex.c_str());
}
VStr* vor_hash_sha1(VStr* s) {
    std::string in = vstr_str(s), hex; uint8_t d[20];
    rt_sha1((const uint8_t*)in.data(), in.size(), d); hash_hex_out(d, 20, hex);
    return vor_str_from_cstr(hex.c_str());
}
VStr* vor_hash_sha256(VStr* s) {
    std::string in = vstr_str(s), hex; uint8_t d[32];
    rt_sha256((const uint8_t*)in.data(), in.size(), d); hash_hex_out(d, 32, hex);
    return vor_str_from_cstr(hex.c_str());
}

// ========== str 模块转发（字符串工具） ==========
static bool rt_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static std::string rt_trim_x(const std::string& s, bool left, bool right) {
    size_t b = 0, e = s.size();
    if (left)  while (b < e && rt_ws(s[b])) ++b;
    if (right) while (e > b && rt_ws(s[e-1])) --e;
    return s.substr(b, e - b);
}
VStr* vor_str_upper(VStr* s) {
    std::string r = vstr_str(s);
    for (auto& c : r) c = (char)std::toupper((unsigned char)c);
    return vor_str_from_cstr(r.c_str());
}
VStr* vor_str_lower(VStr* s) {
    std::string r = vstr_str(s);
    for (auto& c : r) c = (char)std::tolower((unsigned char)c);
    return vor_str_from_cstr(r.c_str());
}
VStr* vor_str_trim(VStr* s)  { return vor_str_from_cstr(rt_trim_x(vstr_str(s), true, true).c_str()); }
VStr* vor_str_ltrim(VStr* s) { return vor_str_from_cstr(rt_trim_x(vstr_str(s), true, false).c_str()); }
VStr* vor_str_rtrim(VStr* s) { return vor_str_from_cstr(rt_trim_x(vstr_str(s), false, true).c_str()); }
long long vor_str_contains(VStr* s, VStr* sub) {
    return vstr_str(s).find(vstr_str(sub)) != std::string::npos ? 1 : 0;
}
long long vor_str_starts_with(VStr* s, VStr* pre) {
    std::string S = vstr_str(s), P = vstr_str(pre);
    return (S.size() >= P.size() && S.compare(0, P.size(), P) == 0) ? 1 : 0;
}
long long vor_str_ends_with(VStr* s, VStr* suf) {
    std::string S = vstr_str(s), F = vstr_str(suf);
    return (S.size() >= F.size() && S.compare(S.size() - F.size(), F.size(), F) == 0) ? 1 : 0;
}
VStr* vor_str_removeprefix(VStr* s, VStr* pre) {
    std::string S = vstr_str(s), P = vstr_str(pre);
    if (S.size() >= P.size() && S.compare(0, P.size(), P) == 0)
        return vor_str_from_bytes(S.data() + P.size(), (int)(S.size() - P.size()));
    return vor_str_from_cstr(S.c_str());
}
VStr* vor_str_removesuffix(VStr* s, VStr* suf) {
    std::string S = vstr_str(s), F = vstr_str(suf);
    if (S.size() >= F.size() && S.compare(S.size() - F.size(), F.size(), F) == 0)
        return vor_str_from_bytes(S.data(), (int)(S.size() - F.size()));
    return vor_str_from_cstr(S.c_str());
}
long long vor_str_find(VStr* s, VStr* sub) {
    size_t p = vstr_str(s).find(vstr_str(sub));
    return p == std::string::npos ? -1 : (long long)p;
}
long long vor_str_rfind(VStr* s, VStr* sub) {
    size_t p = vstr_str(s).rfind(vstr_str(sub));
    return p == std::string::npos ? -1 : (long long)p;
}
long long vor_str_count(VStr* s, VStr* sub) {
    std::string S = vstr_str(s), F = vstr_str(sub);
    if (F.empty()) return 0;
    long long n = 0; size_t p = 0;
    while ((p = S.find(F, p)) != std::string::npos) { ++n; p += F.size(); }
    return n;
}
VStr* vor_str_replace(VStr* s, VStr* a, VStr* b) {
    std::string S = vstr_str(s), A = vstr_str(a), B = vstr_str(b);
    if (!A.empty()) {
        size_t p = 0;
        while ((p = S.find(A, p)) != std::string::npos) { S.replace(p, A.size(), B); p += B.size(); }
    }
    return vor_str_from_cstr(S.c_str());
}
VStr* vor_str_slice(VStr* s, long long start, long long end) {
    std::string S = vstr_str(s);
    if (end < 0) end = (long long)S.size();
    if (start < 0) start = 0;
    if ((long long)S.size() < end) end = (long long)S.size();
    if (start > end) return vor_str_from_cstr("");
    return vor_str_from_cstr(S.substr((size_t)start, (size_t)(end - start)).c_str());
}
long long vor_str_char_at(VStr* s, long long i) {
    std::string S = vstr_str(s);
    if (i < 0 || i >= (long long)S.size()) throw_rt("str.char_at: index out of range");
    return (long long)(unsigned char)S[(size_t)i];
}
VStr* vor_str_repeat(VStr* s, long long n) {
    std::string S = vstr_str(s);
    if (n <= 0) return vor_str_from_cstr("");
    std::string out; out.reserve(S.size() * (size_t)n);
    for (long long i = 0; i < n; ++i) out += S;
    return vor_str_from_cstr(out.c_str());
}
static VStr* rt_pad(VStr* s, long long w, long long pad, bool left) {
    std::string S = vstr_str(s);
    std::string out;
    char ch = pad ? (char)pad : ' ';
    if (left) {
        if (w > (long long)S.size()) out.assign((size_t)(w - S.size()), ch);
        out += S;
    } else {
        out = S;
        if (w > (long long)S.size()) out.append((size_t)(w - S.size()), ch);
    }
    return vor_str_from_cstr(out.c_str());
}
VStr* vor_str_pad_left(VStr* s, long long w, long long pad) { return rt_pad(s, w, pad, true); }
VStr* vor_str_pad_right(VStr* s, long long w, long long pad) { return rt_pad(s, w, pad, false); }
long long vor_str_first_byte(VStr* s, long long def) {
    std::string S = vstr_str(s);
    return S.empty() ? def : (long long)(unsigned char)S[0];
}

// str.format(fmt, args...)：以顺序参数替换模板中的连续 "{}"
VStr* vor_str_format(VStr* fmt, VStr** args, long long n) {
    const std::string f = fmt ? vstr_str(fmt) : std::string();
    std::string out;
    out.reserve(f.size());
    size_t argi = 0;
    for (size_t i = 0; i < f.size();) {
        if (f[i] == '{' && i + 1 < f.size() && f[i + 1] == '}') {
            if (argi < (size_t)n && args && args[argi]) out += vstr_str(args[argi]);
            ++argi;
            i += 2;
        } else {
            out += f[i];
            ++i;
        }
    }
    return vor_str_from_cstr(out.c_str());
}
VStr* vor_str_join(VStr* sep, VStr** parts, long long n) {
    std::string se = sep ? vstr_str(sep) : std::string();
    std::string out;
    for (long long i = 0; i < n; ++i) {
        if (i > 0) out += se;
        if (parts && parts[i]) out += vstr_str(parts[i]);
    }
    return vor_str_from_cstr(out.c_str());
}
// split(s, sep) -> 字符串列表。每个元素存为 VStr*，并交给列表长期持有（运行时不做释放，避免悬挂）。
VList* vor_str_split(VStr* s, VStr* sep) {
    VList* l = vor_list_new();
    std::string text = vstr_str(s);
    std::string delim = sep ? vstr_str(sep) : std::string();
    if (delim.empty()) {
        for (char c : text) {
            std::string one(1, c);
            vor_list_push(l, (long long)vor_str_from_cstr(one.c_str()));
        }
        return l;
    }
    size_t pos = 0, found;
    while ((found = text.find(delim, pos)) != std::string::npos) {
        vor_list_push(l, (long long)vor_str_from_bytes(text.data() + pos, (int)(found - pos)));
        pos = found + delim.size();
    }
    vor_list_push(l, (long long)vor_str_from_bytes(text.data() + pos, (int)(text.size() - pos)));
    return l;
}

long long vor_str_ord(VStr* s) {
    std::string S = vstr_str(s);
    return S.empty() ? -1 : (long long)(unsigned char)S[0];
}
VStr* vor_str_chr(long long c) {
    if (c < 0 || c > 255) c = 0;
    char b[2] = { (char)c, '\0' };
    return vor_str_from_cstr(b);
}
VStr* vor_str_zfill(VStr* s, long long w) {
    std::string S = vstr_str(s);
    if (w > (long long)S.size()) S = std::string((size_t)(w - S.size()), '0') + S;
    return vor_str_from_cstr(S.c_str());
}
VStr* vor_str_center(VStr* s, long long w, long long pad) {
    std::string S = vstr_str(s);
    char p = pad ? (char)pad : ' ';
    if (w > (long long)S.size()) {
        long long total = w - (long long)S.size();
        long long left = total / 2, right = total - left;
        S = std::string((size_t)left, p) + S + std::string((size_t)right, p);
    }
    return vor_str_from_cstr(S.c_str());
}
VStr* vor_str_title(VStr* s) {
    std::string S = vstr_str(s);
    bool start = true;
    for (auto& c : S) {
        if (std::isalpha((unsigned char)c)) { if (start) c = (char)std::toupper((unsigned char)c); start = false; }
        else start = true;
    }
    return vor_str_from_cstr(S.c_str());
}
VStr* vor_str_swapcase(VStr* s) {
    std::string S = vstr_str(s);
    for (auto& c : S) {
        if (std::isupper((unsigned char)c)) c = (char)std::tolower((unsigned char)c);
        else if (std::islower((unsigned char)c)) c = (char)std::toupper((unsigned char)c);
    }
    return vor_str_from_cstr(S.c_str());
}
VStr* vor_str_capitalize(VStr* s) {
    std::string S = vstr_str(s);
    if (!S.empty()) {
        if (std::isalpha((unsigned char)S[0]))
            S[0] = (char)std::toupper((unsigned char)S[0]);
        for (size_t i = 1; i < S.size(); ++i)
            if (std::isalpha((unsigned char)S[i])) S[i] = (char)std::tolower((unsigned char)S[i]);
    }
    return vor_str_from_cstr(S.c_str());
}
static int rt_str_any_alpha(const std::string& S) {
    for (auto c : S) if (std::isalpha((unsigned char)c)) return 1;
    return 0;
}
long long vor_str_isalpha(VStr* s) {
    std::string S = vstr_str(s);
    if (S.empty()) return 0;
    for (auto c : S) if (!std::isalpha((unsigned char)c)) return 0;
    return 1;
}
long long vor_str_isdigit(VStr* s) {
    std::string S = vstr_str(s);
    if (S.empty()) return 0;
    for (auto c : S) if (!std::isdigit((unsigned char)c)) return 0;
    return 1;
}
long long vor_str_isalnum(VStr* s) {
    std::string S = vstr_str(s);
    if (S.empty()) return 0;
    for (auto c : S) if (!std::isalnum((unsigned char)c)) return 0;
    return 1;
}
long long vor_str_isspace(VStr* s) {
    std::string S = vstr_str(s);
    if (S.empty()) return 0;
    for (auto c : S) if (!std::isspace((unsigned char)c)) return 0;
    return 1;
}
long long vor_str_isupper(VStr* s) {
    std::string S = vstr_str(s);
    if (!rt_str_any_alpha(S)) return 0;
    for (auto c : S) if (std::isalpha((unsigned char)c) && !std::isupper((unsigned char)c)) return 0;
    return 1;
}
long long vor_str_islower(VStr* s) {
    std::string S = vstr_str(s);
    if (!rt_str_any_alpha(S)) return 0;
    for (auto c : S) if (std::isalpha((unsigned char)c) && !std::islower((unsigned char)c)) return 0;
    return 1;
}

// ========== net 模块转发（url_encode/decode + http_get/post） ==========
VStr* vor_net_url_encode(VStr* s) {
    return vor_str_from_cstr(vortex::net_url_encode(vstr_str(s)).c_str());
}
VStr* vor_net_url_decode(VStr* s) {
    return vor_str_from_cstr(vortex::net_url_decode(vstr_str(s)).c_str());
}
VStr* vor_net_http_get(VStr* url) {
    try { return vor_str_from_cstr(vortex::net_http(vstr_str(url), "GET", "").c_str()); }
    catch (const std::exception& e) { throw_rt((std::string("net.http_get: ") + e.what()).c_str()); }
}
VStr* vor_net_http_post(VStr* url, VStr* body) {
    try { return vor_str_from_cstr(vortex::net_http(vstr_str(url), "POST", vstr_str(body)).c_str()); }
    catch (const std::exception& e) { throw_rt((std::string("net.http_post: ") + e.what()).c_str()); }
}

// ========== os 路径拆分（basename / dirname / extname） ==========
static std::vector<std::string>& rt_args() { static std::vector<std::string> a; return a; }
void vor_set_args(int argc, const char** argv) {
    auto& a = rt_args(); a.clear();
    // 跳过程序名(argv[0])，剩余为用户参数
    for (int i = 1; i < argc; ++i) a.emplace_back(argv[i] ? argv[i] : "");
}
long long vor_os_argc(void) { return (long long)rt_args().size(); }
VStr* vor_os_arg(long long i) {
    auto& a = rt_args();
    if (i < 0 || i >= (long long)a.size()) throw_rt("os.arg: index out of range");
    return vor_str_from_cstr(a[(size_t)i].c_str());
}
VStr* vor_os_basename(VStr* p) {
    std::string s = vstr_str(p);
    size_t x = s.find_last_of("/\\");
    return vor_str_from_cstr((x == std::string::npos ? s : s.substr(x + 1)).c_str());
}
VStr* vor_os_dirname(VStr* p) {
    std::string s = vstr_str(p);
    size_t x = s.find_last_of("/\\");
    if (x == std::string::npos) return vor_str_from_cstr("");
    if (x == 0) return vor_str_from_cstr("/");
    return vor_str_from_cstr(s.substr(0, x).c_str());
}
VStr* vor_os_extname(VStr* p) {
    std::string s = vstr_str(p);
    size_t x = s.find_last_of("/\\");
    size_t d = s.find_last_of('.');
    if (d == std::string::npos || d == 0 || (x != std::string::npos && d < x))
        return vor_str_from_cstr("");
    return vor_str_from_cstr(s.substr(d).c_str());
}
VList* vor_os_listdir(VStr* path) {
    std::error_code ec;
    std::vector<std::string> names;
    for (fs::directory_iterator it(vstr_str(path), ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        names.push_back(it->path().filename().string());
    }
    std::sort(names.begin(), names.end());  // 排序保证与解释器一致
    VList* l = vor_list_new();
    for (auto& n : names) vor_list_push(l, (long long)vor_str_from_cstr(n.c_str()));
    return l;
}