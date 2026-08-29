#ifndef NEWCODERING_VALUE_H
#define NEWCODERING_VALUE_H

#include <string>
#include <vector>
#include <deque>
#include <set>
#include <unordered_set>
#include <map>
#include <unordered_map>
#include <list>
#include <memory>
#include <functional>
#include <sstream>
#include <variant>
#include <any>
#include <utility>

namespace ncr {

// ========== 值类型枚举 ==========
enum class ValueType {
    None,
    Bool,
    Int,        // long long 内部表示（含 short/char 等）
    UInt,       // unsigned long long
    Float,      // double 内部表示
    Char,       // ASCII char
    Unichar,    // Unicode codepoint (int)
    Str,        // ASCII string
    UniStr,     // Unicode string (UTF-8 inside)
    Bin,        // bytes (vector<unsigned char>)
    MemAdr,     // memory address (reference to variable)
    List,       // doubly-linked list (use std::list<Value>)
    Stack,      // LIFO (vector as stack)
    Queue,      // FIFO (std::deque)
    Set,        // ordered set (std::set, key -> Value hash)
    UndSet,     // unordered set (std::unordered_set)
    Dict,       // hash map
    Pair,       // (a, b)
    Tuple,      // fixed tuple
    Function,   // user-defined / built-in
    Module,     // 标准库模块（math / time / random）
};

struct Value;
using ValuePtr = std::shared_ptr<Value>;
using ValueVec = std::vector<ValuePtr>;

// ========== 前向声明 ==========
class Environment;
struct Stmt;
struct FunctionDefStmt;

// ========== 函数对象 ==========
struct FunctionValue {
    std::string name;
    std::vector<std::pair<std::string, std::string>> params; // name, type (unused dynamic)
    std::vector<std::string> param_names;
    ValueVec default_args; // parallel
    bool has_vararg = false;
    bool is_builtin = false;
    const FunctionDefStmt* def = nullptr; // AST
    std::function<ValuePtr(const ValueVec&, Environment&)> builtin_fn;
};

// ========== MemAdr: 引用环境中的变量 ==========
struct MemAdrValue {
    std::string var_name;  // 引用的变量名
    Environment* env = nullptr; // 所属环境
};

// ========== 复合/容器 ==========
using ListRep   = std::list<ValuePtr>;
using StackRep  = std::vector<ValuePtr>;
using QueueRep  = std::deque<ValuePtr>;

// 为 set / undset / dict 提供可哈希比较的 Value 包装
struct ValueHash;
struct ValueEqual;
struct ValueCompare;

using SetRep    = std::set<ValuePtr, ValueCompare>;
using UndSetRep = std::unordered_set<ValuePtr, ValueHash, ValueEqual>;
using DictRep   = std::unordered_map<ValuePtr, ValuePtr, ValueHash, ValueEqual>;

struct PairRep { ValuePtr first; ValuePtr second; };
using TupleRep  = std::vector<ValuePtr>;
using ModuleRep = std::unordered_map<std::string, ValuePtr>; // 模块成员：常量或函数

// ========== 哈希与比较 functors ==========
struct ValueHash {
    size_t operator()(const ValuePtr& v) const;
};
struct ValueEqual {
    bool operator()(const ValuePtr& a, const ValuePtr& b) const;
};
struct ValueCompare {
    bool operator()(const ValuePtr& a, const ValuePtr& b) const;
};

// ========== Value ==========
struct Value : std::enable_shared_from_this<Value> {
    ValueType type;

    // 存储
    bool bool_val = false;
    long long int_val = 0;
    unsigned long long uint_val = 0;
    double float_val = 0.0;
    int char_val = 0;
    std::string str_val; // for Str/UniStr
    std::vector<unsigned char> bin_val;

    std::shared_ptr<ListRep> list_rep;
    std::shared_ptr<StackRep> stack_rep;
    std::shared_ptr<QueueRep> queue_rep;
    std::shared_ptr<SetRep> set_rep;
    std::shared_ptr<UndSetRep> undset_rep;
    std::shared_ptr<DictRep> dict_rep;
    std::shared_ptr<PairRep> pair_rep;
    std::shared_ptr<TupleRep> tuple_rep;
    std::shared_ptr<FunctionValue> fn_rep;
    std::shared_ptr<MemAdrValue> adr_rep;
    std::shared_ptr<ModuleRep> module_rep;

    bool is_const = false;

    explicit Value(ValueType t = ValueType::None) : type(t) {}

    // ===== factory helpers =====
    static ValuePtr make_none();
    static ValuePtr make_bool(bool b);
    static ValuePtr make_int(long long v);
    static ValuePtr make_uint(unsigned long long v);
    static ValuePtr make_float(double v);
    static ValuePtr make_char(int c);
    static ValuePtr make_unichar(int c);
    static ValuePtr make_str(std::string s);
    static ValuePtr make_unistr(std::string s);
    static ValuePtr make_bin(std::vector<unsigned char> b);
    static ValuePtr make_list();
    static ValuePtr make_stack();
    static ValuePtr make_queue();
    static ValuePtr make_set();
    static ValuePtr make_undset();
    static ValuePtr make_dict();
    static ValuePtr make_pair(ValuePtr a, ValuePtr b);
    static ValuePtr make_tuple(std::vector<ValuePtr> items);
    static ValuePtr make_adr(std::string name, Environment* env);
    static ValuePtr make_module();

    // clone（深拷贝容器，浅拷贝元素）
    ValuePtr clone() const;

    // 类型名
    std::string type_name() const;
    // 转成可打印字符串
    std::string to_string() const;
    // 真值判断
    bool truthy() const;
    // 类型提升为数值（计算用）- 返回 (is_float, d, i, u)
    void promote_to_numeric(bool& is_float, double& d, long long& i, unsigned long long& u) const;
};

// ========== 操作：返回新 ValuePtr，异常用字符串抛 ==========
ValuePtr value_add(const ValuePtr& a, const ValuePtr& b);
ValuePtr value_sub(const ValuePtr& a, const ValuePtr& b);
ValuePtr value_mul(const ValuePtr& a, const ValuePtr& b);
ValuePtr value_div(const ValuePtr& a, const ValuePtr& b);
ValuePtr value_floordiv(const ValuePtr& a, const ValuePtr& b);
ValuePtr value_mod(const ValuePtr& a, const ValuePtr& b);
ValuePtr value_pow(const ValuePtr& a, const ValuePtr& b);

ValuePtr value_bit_and(const ValuePtr& a, const ValuePtr& b);
ValuePtr value_bit_or(const ValuePtr& a, const ValuePtr& b);
ValuePtr value_bit_xor(const ValuePtr& a, const ValuePtr& b);
ValuePtr value_shl(const ValuePtr& a, const ValuePtr& b);
ValuePtr value_shr(const ValuePtr& a, const ValuePtr& b);
ValuePtr value_bit_not(const ValuePtr& a);
ValuePtr value_neg(const ValuePtr& a);
ValuePtr value_pos(const ValuePtr& a);
ValuePtr value_not(const ValuePtr& a);

bool value_eq(const ValuePtr& a, const ValuePtr& b);
bool value_ne(const ValuePtr& a, const ValuePtr& b);
bool value_lt(const ValuePtr& a, const ValuePtr& b);
bool value_le(const ValuePtr& a, const ValuePtr& b);
bool value_gt(const ValuePtr& a, const ValuePtr& b);
bool value_ge(const ValuePtr& a, const ValuePtr& b);

// 集合运算
ValuePtr value_intersection(const ValuePtr& a, const ValuePtr& b);
ValuePtr value_union(const ValuePtr& a, const ValuePtr& b);
ValuePtr value_difference(const ValuePtr& a, const ValuePtr& b);
ValuePtr value_symmetric_diff(const ValuePtr& a, const ValuePtr& b);

// 类型转换
ValuePtr value_to_int(const ValuePtr& a);
ValuePtr value_to_long(const ValuePtr& a);
ValuePtr value_to_ulong(const ValuePtr& a);
ValuePtr value_to_uint(const ValuePtr& a);
ValuePtr value_to_float(const ValuePtr& a);
ValuePtr value_to_bool(const ValuePtr& a);
ValuePtr value_to_str(const ValuePtr& a);
ValuePtr value_to_unistr(const ValuePtr& a);
ValuePtr value_to_bin(const ValuePtr& a);

// 格式化输出
std::string format_value(const ValuePtr& v, const std::string& spec);

// 异常类
class RuntimeError : public std::exception {
public:
    explicit RuntimeError(std::string m) : msg_(std::move(m)) {}
    const char* what() const noexcept override { return msg_.c_str(); }
    const std::string& message() const { return msg_; }
private:
    std::string msg_;
};

} // namespace ncr

#endif // NEWCODERING_VALUE_H
