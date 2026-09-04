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
VStr* vor_bool_to_str(int b);
// str -> 标量（解析失败时抛异常，供 int()/float() 转换）
long long vor_cast_i64(VStr* s);
double    vor_cast_f64(VStr* s);

// ========== 异常（P3 try/catch，setjmp/longjmp） ==========
// codegen 在 try 入口对本地 jmp_buf 回调 setjmp 后调用 push 注册；
// throw 时 longjmp 回该 frame，catch 读取 msg。
void    vor_ex_push(void* jb);
void    vor_ex_pop(void);
void    vor_throw_str(VStr* msg);
VStr*   vor_ex_caught(void);
// 低层 setjmp/longjmp（runtime 内自实现，Win64）
int     vor_ex_setjmp(void* jb);
void    vor_ex_longjmp(void* jb, int val);

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

// ========== 集合（复用 VList 布局 + 去重，元素 int）==========
void vor_set_add(VList* s, long long e);
int  vor_set_contains(const VList* s, long long e);
int  vor_set_len(const VList* s);
void vor_set_remove(VList* s, long long e);
VList* vor_set_from_list(const VList* l);   // 去重拷贝为集合
void vor_print_set(const VList* s);          // 打印 {1, 2, 3}（与解释器对齐）

// ========== 序对 pair (a, b) ==========
typedef struct VPair {
    VObject hdr;
    long long first;
    long long second;
} VPair;
VPair* vor_pair_new(long long a, long long b);
long long vor_pair_first(const VPair* p);
long long vor_pair_second(const VPair* p);
void vor_print_pair(const VPair* p);

// ========== 元组 tuple (a, b, ...) ==========
typedef struct VTuple {
    VObject hdr;
    int len;
    int cap;
    long long* elems;
} VTuple;
VTuple* vor_tuple_new(int n);
VTuple* vor_tuple_from_list(const VList* l);
void vor_tuple_set(VTuple* t, int idx, long long e);
long long vor_tuple_at(const VTuple* t, long long idx);
int  vor_tuple_len(const VTuple* t);
void vor_print_tuple(const VTuple* t);

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

// ========== time 模块转发（P4 批A） ==========
double vor_time_now(void);
void   vor_sleep(double sec);
double vor_counter(void);
void   vor_counter_reset(void);
double vor_process_time(void);

// ========== random 模块转发（P4 批A） ==========
void vor_rng_seed(unsigned long long seed);
double vor_rng_random(void);
double vor_rng_uniform(double a, double b);
long long vor_rng_randint(long long lo, long long hi);
long long vor_rng_randrange(long long start, long long stop, long long step);
double vor_rng_gauss(double mu, double sigma);
double vor_rng_expovariate(double lambda);
double vor_rng_triangular(double lo, double hi, double mode);
VStr* vor_rng_getstate(void);
void  vor_rng_setstate(VStr* s);

// ========== log 模块转发（P4 批A） ==========
void vor_log_emit(int level, VStr* msg);
void vor_log_level(VStr* name);
VStr* vor_log_get_level(void);
void vor_log_format(VStr* fmt);
void vor_log_file(VStr* path);
void vor_log_console(int on);

// ========== time 批B：tuple 交互 ==========
VTuple* vor_time_gmtime(double ts);
VTuple* vor_time_localtime(double ts);
double  vor_time_mktime(const VTuple* tr);
VStr*   vor_time_strftime(VStr* fmt, const VTuple* tr);

// ========== random 批B：list 交互 ==========
long long vor_rng_choice(const VList* pop);
VList* vor_rng_choices(const VList* pop, const VList* weights, long long k);
void vor_rng_shuffle(VList* l);
VList* vor_rng_sample(const VList* pop, long long k);

// ========== thread 模块转发（P4）==========
// 线程句柄与互斥锁/原子均为不透明指针，句柄生命周期归运行时管理。
void* vor_thread_run(void (*fn)(void)); // 启动线程执行无参函数
void  vor_thread_join(void* h);         // 等待线程结束
void  vor_thread_yield(void);
void  vor_thread_sleep(long long ms);
long long vor_thread_hardware(void);
void* vor_thread_mutex(void);
void  vor_thread_lock(void* m);
void  vor_thread_unlock(void* m);
int   vor_thread_trylock(void* m);
void* vor_thread_atomic(long long v);
long long vor_thread_atomic_get(void* a);
void  vor_thread_atomic_set(void* a, long long v);
long long vor_thread_atomic_add(void* a, long long v);
// 通道（int 负载子集）与线程池（P4）
void* vor_thread_channel(long long cap);
void  vor_thread_channel_send(void* h, long long v);
long long vor_thread_channel_recv(void* h);
void  vor_thread_channel_close(void* h);
long long vor_thread_channel_len(void* h);
void* vor_thread_pool(long long n);
void  vor_thread_pool_submit(void* h, void (*fn)(void));
long long vor_thread_pool_size(void* h);
void  vor_thread_pool_shutdown(void* h);
// 闭包值（P3）：分配 (n+1) 个 i64，[0]=唤起指针，[1..]=捕获槽地址
void* vor_closure_new(long long n);

#ifdef __cplusplus
}
#endif

#endif // VORTEX_RUNTIME_H