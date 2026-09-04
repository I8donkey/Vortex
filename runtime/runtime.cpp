// ============================================================
// runtime.cpp — 类型化运行时实现（C ABI）
// 编译为静态库，供 LLVM 后端生成的产物链接。
// ============================================================
#include "runtime.h"
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
#include <queue>
#include <condition_variable>
#include <functional>

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
int vor_list_len(const VList* l) { return l->len; }

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

// ========== random 模块转发（P4 批A） ==========
// 单例 mt19937，与解释器 get_rng() 同款引擎/头实现，同 seed 下序列一致
static std::mt19937& rng_state() {
    static std::mt19937 rng((unsigned)std::time(nullptr));
    return rng;
}
void vor_rng_seed(unsigned long long seed) { rng_state().seed((std::mt19937::result_type)seed); }
double vor_rng_random(void) { return std::uniform_real_distribution<>(0.0, 1.0)(rng_state()); }
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