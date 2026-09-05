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
// 比较两个 str（strcmp 语义）：<0 / 0 / >0
int vor_str_cmp(const VStr* a, const VStr* b);
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
VStr*  vor_list_get_str(const VList* l, int idx);
int    vor_list_len(const VList* l);
long long vor_list_sum(const VList* l);
long long vor_list_prod(const VList* l);

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

// 更多 math.* 转发
double vor_cbrt(double a);
double vor_exp(double a);
double vor_log(double a);
double vor_logbase(double a, double base);
double vor_log2(double a);
double vor_log10(double a);
double vor_log1p(double a);
double vor_expm1(double a);
double vor_erf(double a);
double vor_tgamma(double a);
double vor_lgamma(double a);
double vor_asin(double a);
double vor_acos(double a);
double vor_atan(double a);
double vor_atan2(double a, double b);
double vor_sinh(double a);
double vor_cosh(double a);
double vor_tanh(double a);
double vor_asinh(double a);
double vor_acosh(double a);
double vor_atanh(double a);
double vor_hypot(double a, double b);
long long vor_trunc(double a);
double vor_roundn(double a, long long nd);
double vor_fmod(double a, double b);
double vor_fmin(double a, double b);
double vor_fmax(double a, double b);
double vor_remainder(double a, double b);
long long vor_factorial(long long n);
long long vor_isqrt(long long n);
long long vor_gcd(long long a, long long b);
long long vor_lcm(long long a, long long b);
long long vor_isinf(double a);
long long vor_isnan(double a);
long long vor_isfinite(double a);
long long vor_isclose(double a, double b, double rel_tol, double abs_tol);
long long vor_iabs(long long a);
long long vor_imin(long long a, long long b);
long long vor_imax(long long a, long long b);
long long vor_comb(long long n, long long k);
long long vor_perm(long long n, long long k);
double vor_radians(double a);
double vor_degrees(double a);
double vor_copysign(double a, double b);
double vor_remainder(double a, double b);

// ========== time 模块转发（P4 批A） ==========
double vor_time_now(void);
void   vor_sleep(double sec);
double vor_counter(void);
void   vor_counter_reset(void);
double vor_process_time(void);

// ========== sys 模块转发 ==========
VStr*  vor_sys_version(void);
long long vor_sys_time_ms(void);
double vor_sys_clock(void);
long long vor_sys_sleep(long long ms);
void   vor_sys_exit(long long code);

// ========== random 模块转发（P4 批A） ==========
void vor_rng_seed(unsigned long long seed);
double vor_rng_random(void);
double vor_rng_uniform(double a, double b);
long long vor_rng_randint(long long lo, long long hi);
long long vor_rng_getrandbits(long long k);
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

// ========== file 模块转发 ==========
VStr*  vor_file_read(VStr* path);
VList* vor_file_readlines(VStr* path);
VList* vor_file_listdir(VStr* path);
void   vor_file_write(VStr* path, VStr* data);
void   vor_file_append(VStr* path, VStr* data);
int    vor_file_exists(VStr* path);
int    vor_file_remove(VStr* path);
int    vor_file_rename(VStr* from, VStr* to);
long long vor_file_size(VStr* path);
int    vor_file_isdir(VStr* path);
int    vor_file_isfile(VStr* path);
int    vor_file_mkdir(VStr* path);
int    vor_file_rmdir(VStr* path);
VList* vor_file_listdir(VStr* path);

// ========== zip 模块转发（zlib） ==========
void   vor_zip_add(VStr* path, VStr* name, VStr* data);
VStr*  vor_zip_extract(VStr* path, VStr* name);
long long vor_zip_count(VStr* path);
VList* vor_zip_names(VStr* path);
int    vor_zip_has(VStr* path, VStr* name);

// ========== xml 模块转发 ==========
VStr* vor_xml_escape(VStr* s);
VStr* vor_xml_unescape(VStr* s);
VStr* vor_xml_parse_text(VStr* xml, VStr* tag);

// ========== html 模块转发 ==========
VStr* vor_html_escape(VStr* s);
VStr* vor_html_unescape(VStr* s);
VStr* vor_html_strip_tags(VStr* s);

// ========== sql 模块转发（sqlite3；句柄以不透明指针传递） ==========
void*    vor_sql_open(VStr* path);
void     vor_sql_close(void* h);
long long vor_sql_execute(void* h, VStr* sql);
long long vor_sql_table_exists(void* h, VStr* name);

// ========== os 模块转发 ==========
VStr*    vor_os_getenv(VStr* name);
int      vor_os_hasenv(VStr* name);
int      vor_os_setenv(VStr* name, VStr* val);
int      vor_os_unsetenv(VStr* name);
VStr*    vor_os_cwd(void);
int      vor_os_chdir(VStr* path);
long long vor_os_pid(void);
VStr*    vor_os_platform(void);
VStr*    vor_os_home(void);
VStr*    vor_os_tempdir(void);
VStr*    vor_os_path_join(VStr* a, VStr* b);

// ========== regex 模块转发 ==========
VStr*    vor_regex_escape(VStr* s);
VList*   vor_regex_split(VStr* pat, VStr* s);
int      vor_regex_valid(VStr* pat);
int      vor_regex_match(VStr* pat, VStr* s);
int      vor_regex_search(VStr* pat, VStr* s);
VStr*    vor_regex_find(VStr* pat, VStr* s);
VStr*    vor_regex_find_all(VStr* pat, VStr* s);
VStr*    vor_regex_replace(VStr* pat, VStr* s, VStr* repl);
long long vor_regex_count(VStr* pat, VStr* s);

// ========== json 模块转发 ==========
int      vor_json_valid(VStr* s);
VList*   vor_json_parse_array(VStr* s);
VStr*    vor_json_get(VStr* s, VStr* key);
VStr*    vor_json_parse_str(VStr* s);
long long vor_json_parse_int(VStr* s);
double   vor_json_parse_float(VStr* s);
int      vor_json_parse_bool(VStr* s);
VStr*    vor_json_stringify_str(VStr* s);
VStr*    vor_json_stringify_int(long long v);
VStr*    vor_json_stringify_float(double v);
VStr*    vor_json_stringify_bool(int b);

// ========== base64 模块转发 ==========
VStr* vor_base64_encode(VStr* s);
VStr* vor_base64_decode(VStr* s);

// ========== datetime 模块转发 ==========
VStr*    vor_datetime_ymd(long long y, long long mo, long long d);
VStr*    vor_datetime_to_iso(long long epoch);
long long vor_datetime_from_iso(VStr* iso);
VStr*    vor_datetime_today(void);
VStr*    vor_datetime_add_days(VStr* iso, long long n);
long long vor_datetime_days_between(VStr* a, VStr* b);

// ========== csv 模块转发（to_line 由前端用 quote+concat 拼，这里实现剩余标量；sep 为空则默认为逗号） ==========
VStr*    vor_csv_quote(VStr* f, VStr* sep);
long long vor_csv_count_fields(VStr* line, VStr* sep);
VStr* vor_csv_field_at(VStr* line, long long index, VStr* sep);
VList* vor_csv_parse_row(VStr* line, VStr* sep);

// ========== hash 模块转发（md5 / sha1 / sha256，输出小写十六进制） ==========
VStr* vor_hash_md5(VStr* s);
VStr* vor_hash_sha1(VStr* s);
VStr* vor_hash_sha256(VStr* s);

// ========== str 模块转发（字符串工具，纯标量/字符串返回） ==========
VStr*    vor_str_upper(VStr* s);
VStr*    vor_str_lower(VStr* s);
VStr*    vor_str_trim(VStr* s);
VStr*    vor_str_ltrim(VStr* s);
VStr*    vor_str_rtrim(VStr* s);
long long vor_str_contains(VStr* s, VStr* sub);
long long vor_str_starts_with(VStr* s, VStr* pre);
long long vor_str_ends_with(VStr* s, VStr* suf);
VStr*    vor_str_removeprefix(VStr* s, VStr* pre);
VStr*    vor_str_removesuffix(VStr* s, VStr* suf);
long long vor_str_find(VStr* s, VStr* sub);
long long vor_str_rfind(VStr* s, VStr* sub);
long long vor_str_count(VStr* s, VStr* sub);
VStr*    vor_str_replace(VStr* s, VStr* a, VStr* b);
VStr*    vor_str_slice(VStr* s, long long start, long long end);
long long vor_str_char_at(VStr* s, long long i);
VStr*    vor_str_repeat(VStr* s, long long n);
VStr* vor_str_pad_left(VStr* s, long long w, long long pad);
VStr* vor_str_pad_right(VStr* s, long long w, long long pad);
long long vor_str_first_byte(VStr* s, long long def);
VStr* vor_str_format(VStr* fmt, VStr** args, long long n);
VStr* vor_str_join(VStr* sep, VStr** parts, long long n);
VList* vor_str_split(VStr* s, VStr* sep);
long long vor_str_ord(VStr* s);
VStr* vor_str_chr(long long c);
VStr* vor_str_zfill(VStr* s, long long w);
VStr* vor_str_center(VStr* s, long long w, long long pad);
VStr* vor_str_title(VStr* s);
VStr* vor_str_swapcase(VStr* s);
VStr*    vor_str_capitalize(VStr* s);
long long vor_str_isalpha(VStr* s);
long long vor_str_isdigit(VStr* s);
long long vor_str_isalnum(VStr* s);
long long vor_str_isspace(VStr* s);
long long vor_str_isupper(VStr* s);
long long vor_str_islower(VStr* s);

// ========== net 模块转发（url_encode/decode + http_get/post） ==========
VStr* vor_net_url_encode(VStr* s);
VStr* vor_net_url_decode(VStr* s);
VStr*    vor_net_http_get(VStr* url);
VStr*    vor_net_http_post(VStr* url, VStr* body);
VStr* vor_os_basename(VStr* p);
VStr* vor_os_dirname(VStr* p);
VStr* vor_os_extname(VStr* p);
VList* vor_os_listdir(VStr* path);
void vor_set_args(int argc, const char** argv);
long long vor_os_argc(void);
VStr* vor_os_arg(long long i);

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