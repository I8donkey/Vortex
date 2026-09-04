// ============================================================
// runtime.h — vortex 原生编译器的类型化运行时 (C ABI)
// 供 LLVM 后端生成的 IR 调用。所有导出使用 C 链接。
//
// 设计：标量(int/float/bool)直接走原生寄存器；字符串/容器用
// 引用计数(引用计数头 VObject)。复用方见 方案文档 §3。
// ============================================================
#ifndef VORTEX_RUNTIME_H
#define VORTEX_RUNTIME_H

#ifdef __cplusplus
extern "C" {
#endif

// ========== 引用计数对象头 ==========
typedef struct VObject {
    int refcount;   // 引用计数
    int tag;        // 对象类型标签
    void* next;     // 预留：GC 遍历链表
} VObject;

// ========== 字符串 ==========
typedef struct VStr {
    VObject hdr;
    int len;        // 字节长度(不含 \0)
    char data[1];   // 内联数据(含结尾 \0)
} VStr;

// 创建 str：拷贝 cstr（cstr 可为 NULL，视为空串）
VStr* vor_str_from_cstr(const char* s);
// 创建 str：由字节指针 + 长度
VStr* vor_str_from_bytes(const char* s, int len);
// 拼接两个 str -> 新 str
VStr* vor_str_concat(VStr* a, VStr* b);
// str 长度
int   vor_str_len(VStr* s);
// 返回内部 C 字符串(只读，生命周期归 str)
const char* vor_str_cstr(VStr* s);
// 整型/浮点 -> str（供 print/cast）
VStr* vor_i64_to_str(long long v);
VStr* vor_double_to_str(double v);

// ========== 列表（引用计数，变长数组）==========
// 元素为 64 位盒(标量直存 / 指针)，内部仅按字拷贝，不感知类型。
typedef struct VList {
    VObject hdr;
    int len;
    int cap;
    long long* elems; // 容量 cap 的数组
} VList;

VList* vor_list_new(void);
// 打印 int 列表，形如 [1, 2, 3]
void vor_print_int_list(VList* l);
void   vor_list_push(VList* l, long long e);
void   vor_list_set(VList* l, int idx, long long e);
long long vor_list_get(const VList* l, int idx);
int    vor_list_len(const VList* l);

// ========== 字典（引用计数，线性数组）==========
// key/value 各支持 int 或 str 两种形式。
#define VK_INT 0
#define VK_STR 1
typedef struct VDictEntry {
    int kkind;        // VK_INT / VK_STR
    long long kint;   // kkind==VK_INT 时的键
    VStr* kstr;       // kkind==VK_STR 时的键
    int vkind;        // VK_INT / VK_STR
    long long vint;   // vkind==VK_INT 时的值
    VStr* vstr;       // vkind==VK_STR 时的值
} VDictEntry;
typedef struct VDict {
    VObject hdr;
    int len;
    int cap;
    VDictEntry* entries;
} VDict;

VDict* vor_dict_new(void);
int    vor_dict_len(const VDict* d);
// 设值（按 key 替换或追加）
void vor_dict_set_int(VDict* d, long long key, long long val);
void vor_dict_set_str(VDict* d, VStr* key, long long val);
void vor_dict_set_int_vstr(VDict* d, long long key, VStr* val);
void vor_dict_set_str_vstr(VDict* d, VStr* key, VStr* val);
// 取值（int value）；未命中返回 0
long long vor_dict_get_int(VDict* d, long long key);
long long vor_dict_get_str(VDict* d, VStr* key);
// 取值（str value）；未命中返回 NULL
VStr* vor_dict_get_int_vstr(VDict* d, long long key);
VStr* vor_dict_get_str_vstr(VDict* d, VStr* key);
// 包含判断
int vor_dict_contains_int(VDict* d, long long key);
int vor_dict_contains_str(VDict* d, VStr* key);
// int-key 遍历（for-in 用）
int vor_dict_int_key_count(const VDict* d);
long long vor_dict_int_key_at(const VDict* d, int j);
// 打印 dict，形如 {k: v, k2: v2}（格式与解释器对齐）
void vor_print_dict(VDict* d);

// ========== 引用计数 ==========
void vor_obj_retain(void* obj);   // hdr 必须在对象首部
void vor_obj_release(void* obj);

// ========== 打印（输出到 stdout，格式与解释器对齐）==========
void vor_print_str(VStr* s);
void vor_print_i64(long long v);
void vor_print_double(double v);
void vor_print_bool(int b);
void vor_print_newline(void);
void vor_flush(void);

// 运行时初始化/清理（进程级）
void vor_rt_init(void);
void vor_rt_shutdown(void);

// ========== 数学 ==========
double vor_pow(double a, double b);
double vor_sqrt(double a);
double vor_fabs(double a);
double vor_sin(double a);
double vor_cos(double a);
double vor_tan(double a);
long long vor_floor(double a);
long long vor_ceil(double a);

#ifdef __cplusplus
}
#endif

#endif // VORTEX_RUNTIME_H