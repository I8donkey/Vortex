// ============================================================
// runtime.cpp — 类型化运行时实现（C ABI）
// 编译为静态库，供 LLVM 后端生成的产物链接。
// ============================================================
#include "runtime.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

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

// ========== 运行时生命周期 ==========
void vor_rt_init(void)    {}
void vor_rt_shutdown(void) {}