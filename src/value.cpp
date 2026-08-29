#include "value.h"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <iomanip>

namespace vortex {

// ================ Value factory ================
ValuePtr Value::make_none()     { auto p = std::make_shared<Value>(ValueType::None); return p; }
ValuePtr Value::make_bool(bool b) { auto p = std::make_shared<Value>(ValueType::Bool); p->bool_val = b; return p; }
ValuePtr Value::make_int(long long v) { auto p = std::make_shared<Value>(ValueType::Int); p->int_val = v; return p; }
ValuePtr Value::make_uint(unsigned long long v) { auto p = std::make_shared<Value>(ValueType::UInt); p->uint_val = v; return p; }
ValuePtr Value::make_float(double v) { auto p = std::make_shared<Value>(ValueType::Float); p->float_val = v; return p; }
ValuePtr Value::make_char(int c) { auto p = std::make_shared<Value>(ValueType::Char); p->char_val = c & 0xFF; p->int_val = c & 0xFF; return p; }
ValuePtr Value::make_unichar(int c) { auto p = std::make_shared<Value>(ValueType::Unichar); p->char_val = c; p->int_val = c; return p; }
ValuePtr Value::make_str(std::string s) { auto p = std::make_shared<Value>(ValueType::Str); p->str_val = std::move(s); return p; }
ValuePtr Value::make_unistr(std::string s) { auto p = std::make_shared<Value>(ValueType::UniStr); p->str_val = std::move(s); return p; }
ValuePtr Value::make_bin(std::vector<unsigned char> b) { auto p = std::make_shared<Value>(ValueType::Bin); p->bin_val = std::move(b); return p; }
ValuePtr Value::make_list() { auto p = std::make_shared<Value>(ValueType::List); p->list_rep = std::make_shared<ListRep>(); return p; }
ValuePtr Value::make_stack() { auto p = std::make_shared<Value>(ValueType::Stack); p->stack_rep = std::make_shared<StackRep>(); return p; }
ValuePtr Value::make_queue() { auto p = std::make_shared<Value>(ValueType::Queue); p->queue_rep = std::make_shared<QueueRep>(); return p; }
ValuePtr Value::make_set() { auto p = std::make_shared<Value>(ValueType::Set); p->set_rep = std::make_shared<SetRep>(); return p; }
ValuePtr Value::make_undset() { auto p = std::make_shared<Value>(ValueType::UndSet); p->undset_rep = std::make_shared<UndSetRep>(); return p; }
ValuePtr Value::make_dict() { auto p = std::make_shared<Value>(ValueType::Dict); p->dict_rep = std::make_shared<DictRep>(); return p; }
ValuePtr Value::make_pair(ValuePtr a, ValuePtr b) { auto p = std::make_shared<Value>(ValueType::Pair); p->pair_rep = std::make_shared<PairRep>(); p->pair_rep->first = std::move(a); p->pair_rep->second = std::move(b); return p; }
ValuePtr Value::make_tuple(std::vector<ValuePtr> items) { auto p = std::make_shared<Value>(ValueType::Tuple); p->tuple_rep = std::make_shared<TupleRep>(std::move(items)); return p; }
ValuePtr Value::make_adr(std::string name, Environment* env) { auto p = std::make_shared<Value>(ValueType::MemAdr); p->adr_rep = std::make_shared<MemAdrValue>(); p->adr_rep->var_name = std::move(name); p->adr_rep->env = env; return p; }
ValuePtr Value::make_module() { auto p = std::make_shared<Value>(ValueType::Module); p->module_rep = std::make_shared<ModuleRep>(); return p; }

std::string Value::type_name() const {
    switch (type) {
        case ValueType::None: return "None";
        case ValueType::Bool: return "bool";
        case ValueType::Int: return "int";
        case ValueType::UInt: return "uint";
        case ValueType::Float: return "double";
        case ValueType::Char: return "char";
        case ValueType::Unichar: return "unichar";
        case ValueType::Str: return "str";
        case ValueType::UniStr: return "unistr";
        case ValueType::Bin: return "bin";
        case ValueType::List: return "list";
        case ValueType::Stack: return "stack";
        case ValueType::Queue: return "queue";
        case ValueType::Set: return "set";
        case ValueType::UndSet: return "undset";
        case ValueType::Dict: return "dict";
        case ValueType::Pair: return "pair";
        case ValueType::Tuple: return "tuple";
        case ValueType::MemAdr: return "memadr";
        case ValueType::Function: return "function";
        case ValueType::Module: return "module";
    }
    return "unknown";
}

std::string Value::to_string() const {
    std::ostringstream oss;
    switch (type) {
        case ValueType::None: return "None";
        case ValueType::Bool: return bool_val ? "true" : "false";
        case ValueType::Int: oss << int_val; return oss.str();
        case ValueType::UInt: oss << uint_val; return oss.str();
        case ValueType::Float: oss << float_val; return oss.str();
        case ValueType::Char: return std::string(1, (char)char_val);
        case ValueType::Unichar: {
            // 简易转 UTF-8
            if (char_val < 0x80) return std::string(1, (char)char_val);
            if (char_val < 0x800) {
                char b[2]; b[0] = 0xC0 | ((char_val >> 6) & 0x1F); b[1] = 0x80 | (char_val & 0x3F);
                return std::string(b, 2);
            }
            char b[3]; b[0] = 0xE0 | ((char_val >> 12) & 0x0F);
            b[1] = 0x80 | ((char_val >> 6) & 0x3F);
            b[2] = 0x80 | (char_val & 0x3F);
            return std::string(b, 3);
        }
        case ValueType::Str:
        case ValueType::UniStr: return str_val;
        case ValueType::Bin: {
            std::string r;
            for (unsigned char c : bin_val) { r += (char)c; }
            return r;
        }
        case ValueType::List: {
            oss << "["; bool first = true;
            for (auto& e : *list_rep) {
                if (!first) oss << ", ";
                if (e->type == ValueType::Str) oss << "\"" << e->str_val << "\"";
                else oss << e->to_string();
                first = false;
            }
            oss << "]"; return oss.str();
        }
        case ValueType::Stack: {
            oss << "["; bool first = true;
            for (auto& e : *stack_rep) { if (!first) oss << ", "; oss << e->to_string(); first = false; }
            oss << "]"; return oss.str();
        }
        case ValueType::Queue: {
            oss << "["; bool first = true;
            for (auto& e : *queue_rep) { if (!first) oss << ", "; oss << e->to_string(); first = false; }
            oss << "]"; return oss.str();
        }
        case ValueType::Set: {
            oss << "{"; bool first = true;
            for (auto& e : *set_rep) { if (!first) oss << ", "; oss << e->to_string(); first = false; }
            oss << "}"; return oss.str();
        }
        case ValueType::UndSet: {
            oss << "{"; bool first = true;
            for (auto& e : *undset_rep) { if (!first) oss << ", "; oss << e->to_string(); first = false; }
            oss << "}"; return oss.str();
        }
        case ValueType::Dict: {
            oss << "{"; bool first = true;
            for (auto& [k, v] : *dict_rep) {
                if (!first) oss << ", ";
                if (k->type == ValueType::Str) oss << "\"" << k->str_val << "\": ";
                else oss << k->to_string() << ": ";
                if (v->type == ValueType::Str) oss << "\"" << v->str_val << "\"";
                else oss << v->to_string();
                first = false;
            }
            oss << "}"; return oss.str();
        }
        case ValueType::Pair:
            oss << "(" << pair_rep->first->to_string() << ", " << pair_rep->second->to_string() << ")";
            return oss.str();
        case ValueType::Tuple: {
            oss << "("; bool first = true;
            for (auto& e : *tuple_rep) { if (!first) oss << ", "; oss << e->to_string(); first = false; }
            oss << ")"; return oss.str();
        }
        case ValueType::MemAdr: return "<memadr " + adr_rep->var_name + ">";
        case ValueType::Function: return "<fn " + fn_rep->name + ">";
        case ValueType::Module: return "<module " + std::to_string(module_rep->size()) + " members>";
    }
    return "?";
}

bool Value::truthy() const {
    switch (type) {
        case ValueType::None: return false;
        case ValueType::Bool: return bool_val;
        case ValueType::Int: return int_val != 0;
        case ValueType::UInt: return uint_val != 0;
        case ValueType::Float: return float_val != 0.0;
        case ValueType::Char: case ValueType::Unichar: return char_val != 0;
        case ValueType::Str: case ValueType::UniStr: return !str_val.empty();
        case ValueType::Bin: return !bin_val.empty();
        case ValueType::List: return !list_rep->empty();
        case ValueType::Stack: return !stack_rep->empty();
        case ValueType::Queue: return !queue_rep->empty();
        case ValueType::Set: return !set_rep->empty();
        case ValueType::UndSet: return !undset_rep->empty();
        case ValueType::Dict: return !dict_rep->empty();
        case ValueType::Pair: case ValueType::Tuple: return true;
        case ValueType::MemAdr: case ValueType::Function: return true;
    }
    return false;
}

void Value::promote_to_numeric(bool& is_float, double& d, long long& i, unsigned long long& u) const {
    is_float = false; d = 0.0; i = 0; u = 0;
    switch (type) {
        case ValueType::Float: is_float = true; d = float_val; break;
        case ValueType::Int: i = int_val; break;
        case ValueType::UInt: u = uint_val; break;
        case ValueType::Char: case ValueType::Unichar: i = char_val; break;
        case ValueType::Bool: i = bool_val ? 1 : 0; break;
        default:
            throw RuntimeError("Type '" + type_name() + "' is not numeric");
    }
}

ValuePtr Value::clone() const {
    auto v = std::make_shared<Value>(type);
    v->bool_val = bool_val; v->int_val = int_val; v->uint_val = uint_val;
    v->float_val = float_val; v->char_val = char_val; v->str_val = str_val;
    v->bin_val = bin_val;
    if (list_rep) v->list_rep = std::make_shared<ListRep>(*list_rep);
    if (stack_rep) v->stack_rep = std::make_shared<StackRep>(*stack_rep);
    if (queue_rep) v->queue_rep = std::make_shared<QueueRep>(*queue_rep);
    if (set_rep) v->set_rep = std::make_shared<SetRep>(*set_rep);
    if (undset_rep) v->undset_rep = std::make_shared<UndSetRep>(*undset_rep);
    if (dict_rep) v->dict_rep = std::make_shared<DictRep>(*dict_rep);
    if (pair_rep) {
        v->pair_rep = std::make_shared<PairRep>();
        v->pair_rep->first = pair_rep->first;
        v->pair_rep->second = pair_rep->second;
    }
    if (tuple_rep) v->tuple_rep = std::make_shared<TupleRep>(*tuple_rep);
    if (fn_rep) v->fn_rep = fn_rep;
    if (adr_rep) v->adr_rep = adr_rep;
    return v;
}

// ================ Hash / Equal / Compare ================
size_t ValueHash::operator()(const ValuePtr& v) const {
    if (!v) return 0;
    std::hash<std::string> hs;
    std::hash<long long> hi;
    std::hash<double> hd;
    switch (v->type) {
        case ValueType::None: return 0x12345;
        case ValueType::Bool: return std::hash<bool>{}(v->bool_val);
        case ValueType::Int: case ValueType::Char: case ValueType::Unichar:
            return hi(v->int_val);
        case ValueType::UInt: return std::hash<unsigned long long>{}(v->uint_val);
        case ValueType::Float: return hd(v->float_val);
        case ValueType::Str: case ValueType::UniStr: return hs(v->str_val);
        default: return hs(v->to_string());
    }
}
bool ValueEqual::operator()(const ValuePtr& a, const ValuePtr& b) const { return value_eq(a, b); }
bool ValueCompare::operator()(const ValuePtr& a, const ValuePtr& b) const { return value_lt(a, b); }

// ================ 数值二元运算辅助 ================
struct Numeric {
    bool is_float; double d; long long i; unsigned long long u;
    explicit Numeric(const ValuePtr& v) {
        v->promote_to_numeric(is_float, d, i, u);
    }
};

static ValuePtr apply_arith(const ValuePtr& a, const ValuePtr& b,
                            std::function<double(double,double)> fopd,
                            std::function<long long(long long,long long)> fopi,
                            std::function<unsigned long long(unsigned long long,unsigned long long)> fopu) {
    Numeric A(a), B(b);
    if (A.is_float || B.is_float) {
        double av = A.is_float ? A.d : (A.u ? (double)A.u : (double)A.i);
        double bv = B.is_float ? B.d : (B.u ? (double)B.u : (double)B.i);
        return Value::make_float(fopd(av, bv));
    }
    // 若任一为 unsigned，按 unsigned 算
    bool au = a->type == ValueType::UInt || b->type == ValueType::UInt;
    if (a->type == ValueType::UInt || b->type == ValueType::UInt) {
        unsigned long long av = A.u ? A.u : (unsigned long long)A.i;
        unsigned long long bv = B.u ? B.u : (unsigned long long)B.i;
        return Value::make_uint(fopu(av, bv));
    }
    (void)au;
    return Value::make_int(fopi(A.i, B.i));
}

// 解决上面引用 ULong/ULong 未枚举问题，简化：只要是 UInt 就算 unsigned

ValuePtr value_add(const ValuePtr& a, const ValuePtr& b) {
    // 字符串拼接
    if ((a->type == ValueType::Str && b->type == ValueType::Str) ||
        (a->type == ValueType::UniStr && b->type == ValueType::UniStr)) {
        return Value::make_str(a->str_val + b->str_val);
    }
    if (a->type == ValueType::List && b->type == ValueType::List) {
        auto r = Value::make_list();
        for (auto& e : *a->list_rep) r->list_rep->push_back(e);
        for (auto& e : *b->list_rep) r->list_rep->push_back(e);
        return r;
    }
    if (a->type == ValueType::Tuple && b->type == ValueType::Tuple) {
        auto v = *a->tuple_rep;
        v.insert(v.end(), b->tuple_rep->begin(), b->tuple_rep->end());
        return Value::make_tuple(v);
    }
    return apply_arith(a, b, std::plus<double>{}, std::plus<long long>{}, std::plus<unsigned long long>{});
}

ValuePtr value_sub(const ValuePtr& a, const ValuePtr& b) {
    return apply_arith(a, b, std::minus<double>{}, std::minus<long long>{}, std::minus<unsigned long long>{});
}

ValuePtr value_mul(const ValuePtr& a, const ValuePtr& b) {
    // 字符串/列表重复
    if ((a->type == ValueType::Str || a->type == ValueType::UniStr) &&
        (b->type == ValueType::Int || b->type == ValueType::UInt)) {
        long long cnt = (b->type == ValueType::Int) ? b->int_val : (long long)b->uint_val;
        if (cnt < 0) cnt = 0;
        std::string r; r.reserve(a->str_val.size() * cnt);
        for (long long i = 0; i < cnt; ++i) r += a->str_val;
        return Value::make_str(r);
    }
    if (a->type == ValueType::List && (b->type == ValueType::Int || b->type == ValueType::UInt)) {
        long long cnt = (b->type == ValueType::Int) ? b->int_val : (long long)b->uint_val;
        if (cnt < 0) cnt = 0;
        auto r = Value::make_list();
        for (long long i = 0; i < cnt; ++i)
            for (auto& e : *a->list_rep) r->list_rep->push_back(e);
        return r;
    }
    return apply_arith(a, b, std::multiplies<double>{}, std::multiplies<long long>{}, std::multiplies<unsigned long long>{});
}

ValuePtr value_div(const ValuePtr& a, const ValuePtr& b) {
    Numeric A(a), B(b);
    double av = A.is_float ? A.d : (A.u ? (double)A.u : (double)A.i);
    double bv = B.is_float ? B.d : (B.u ? (double)B.u : (double)B.i);
    if (bv == 0) throw RuntimeError("Division by zero");
    return Value::make_float(av / bv);
}

ValuePtr value_floordiv(const ValuePtr& a, const ValuePtr& b) {
    Numeric A(a), B(b);
    if (A.is_float || B.is_float) {
        double av = A.is_float ? A.d : (A.u ? (double)A.u : (double)A.i);
        double bv = B.is_float ? B.d : (B.u ? (double)B.u : (double)B.i);
        if (bv == 0) throw RuntimeError("Division by zero");
        return Value::make_float(std::floor(av / bv));
    }
    if (a->type == ValueType::UInt || b->type == ValueType::UInt) {
        unsigned long long av = A.u ? A.u : (unsigned long long)A.i;
        unsigned long long bv = B.u ? B.u : (unsigned long long)B.i;
        if (bv == 0) throw RuntimeError("Division by zero");
        return Value::make_uint(av / bv);
    }
    if (B.i == 0) throw RuntimeError("Division by zero");
    // floor div
    long long q = A.i / B.i;
    if ((A.i ^ B.i) < 0 && A.i % B.i != 0) q -= 1;
    return Value::make_int(q);
}

ValuePtr value_mod(const ValuePtr& a, const ValuePtr& b) {
    Numeric A(a), B(b);
    if (A.is_float || B.is_float) {
        double av = A.is_float ? A.d : (A.u ? (double)A.u : (double)A.i);
        double bv = B.is_float ? B.d : (B.u ? (double)B.u : (double)B.i);
        if (bv == 0) throw RuntimeError("Modulo by zero");
        return Value::make_float(std::fmod(av, bv));
    }
    if (a->type == ValueType::UInt || b->type == ValueType::UInt) {
        unsigned long long av = A.u ? A.u : (unsigned long long)A.i;
        unsigned long long bv = B.u ? B.u : (unsigned long long)B.i;
        if (bv == 0) throw RuntimeError("Modulo by zero");
        return Value::make_uint(av % bv);
    }
    if (B.i == 0) throw RuntimeError("Modulo by zero");
    long long r = A.i % B.i;
    if ((B.i > 0 && r < 0) || (B.i < 0 && r > 0)) r += B.i;
    return Value::make_int(r);
}

ValuePtr value_pow(const ValuePtr& a, const ValuePtr& b) {
    Numeric A(a), B(b);
    double av = A.is_float ? A.d : (A.u ? (double)A.u : (double)A.i);
    double bv = B.is_float ? B.d : (B.u ? (double)B.u : (double)B.i);
    return Value::make_float(std::pow(av, bv));
}

// ================ 位运算 ================
static void as_ull(const ValuePtr& a, const ValuePtr& b, unsigned long long& x, unsigned long long& y) {
    if (a->type == ValueType::Float || b->type == ValueType::Float)
        throw RuntimeError("Bitwise operator on float");
    x = (a->type == ValueType::UInt) ? a->uint_val : (unsigned long long)a->int_val;
    y = (b->type == ValueType::UInt) ? b->uint_val : (unsigned long long)b->int_val;
    (void)x; (void)y;
}

ValuePtr value_bit_and(const ValuePtr& a, const ValuePtr& b) {
    // 集合交集
    if (a->type == ValueType::Set && b->type == ValueType::Set) return value_intersection(a, b);
    if (a->type == ValueType::UndSet && b->type == ValueType::UndSet) return value_intersection(a, b);
    unsigned long long x, y; as_ull(a, b, x, y);
    return a->type == ValueType::UInt || b->type == ValueType::UInt ?
        Value::make_uint(x & y) : Value::make_int((long long)(x & y));
}
ValuePtr value_bit_or(const ValuePtr& a, const ValuePtr& b) {
    if (a->type == ValueType::Set && b->type == ValueType::Set) return value_union(a, b);
    if (a->type == ValueType::UndSet && b->type == ValueType::UndSet) return value_union(a, b);
    unsigned long long x, y; as_ull(a, b, x, y);
    return a->type == ValueType::UInt || b->type == ValueType::UInt ?
        Value::make_uint(x | y) : Value::make_int((long long)(x | y));
}
ValuePtr value_bit_xor(const ValuePtr& a, const ValuePtr& b) {
    if (a->type == ValueType::Set && b->type == ValueType::Set) return value_symmetric_diff(a, b);
    if (a->type == ValueType::UndSet && b->type == ValueType::UndSet) return value_symmetric_diff(a, b);
    unsigned long long x, y; as_ull(a, b, x, y);
    return a->type == ValueType::UInt || b->type == ValueType::UInt ?
        Value::make_uint(x ^ y) : Value::make_int((long long)(x ^ y));
}
ValuePtr value_shl(const ValuePtr& a, const ValuePtr& b) {
    unsigned long long x, y; as_ull(a, b, x, y);
    return a->type == ValueType::UInt ?
        Value::make_uint(x << y) : Value::make_int((long long)(x << y));
}
ValuePtr value_shr(const ValuePtr& a, const ValuePtr& b) {
    unsigned long long x, y; as_ull(a, b, x, y);
    return a->type == ValueType::UInt ?
        Value::make_uint(x >> y) : Value::make_int((long long)(x >> y));
}
ValuePtr value_bit_not(const ValuePtr& a) {
    if (a->type == ValueType::Float) throw RuntimeError("Bitwise NOT on float");
    if (a->type == ValueType::UInt) return Value::make_uint(~a->uint_val);
    return Value::make_int(~a->int_val);
}
ValuePtr value_neg(const ValuePtr& a) {
    if (a->type == ValueType::Float) return Value::make_float(-a->float_val);
    if (a->type == ValueType::UInt) return Value::make_int(-(long long)a->uint_val);
    return Value::make_int(-a->int_val);
}
ValuePtr value_pos(const ValuePtr& a) { return a->clone(); }
ValuePtr value_not(const ValuePtr& a) { return Value::make_bool(!a->truthy()); }

// ================ 比较 ================
bool value_eq(const ValuePtr& a, const ValuePtr& b) {
    if (!a && !b) return true; if (!a || !b) return false;
    if (a->type == ValueType::None && b->type == ValueType::None) return true;
    if (a->type == ValueType::Bool || b->type == ValueType::Bool) {
        return a->truthy() == b->truthy() && (a->type == b->type ||
            (a->type == ValueType::None && b->type == ValueType::None));
    }
    if (a->type == ValueType::Float || b->type == ValueType::Float) {
        Numeric A(a), B(b);
        double av = A.is_float ? A.d : (A.u ? (double)A.u : (double)A.i);
        double bv = B.is_float ? B.d : (B.u ? (double)B.u : (double)B.i);
        return av == bv;
    }
    if ((a->type == ValueType::Int || a->type == ValueType::UInt || a->type == ValueType::Char || a->type == ValueType::Unichar) &&
        (b->type == ValueType::Int || b->type == ValueType::UInt || b->type == ValueType::Char || b->type == ValueType::Unichar)) {
        long long ai = (a->type == ValueType::UInt) ? (long long)a->uint_val : (a->type == ValueType::Int) ? a->int_val : a->char_val;
        long long bi = (b->type == ValueType::UInt) ? (long long)b->uint_val : (b->type == ValueType::Int) ? b->int_val : b->char_val;
        if (a->type == ValueType::UInt && b->type == ValueType::UInt)
            return a->uint_val == b->uint_val;
        return ai == bi;
    }
    if ((a->type == ValueType::Str || a->type == ValueType::UniStr) && (b->type == ValueType::Str || b->type == ValueType::UniStr))
        return a->str_val == b->str_val;
    if (a->type == ValueType::Bin && b->type == ValueType::Bin) return a->bin_val == b->bin_val;
    if (a->type == ValueType::List && b->type == ValueType::List) {
        if (a->list_rep->size() != b->list_rep->size()) return false;
        auto ia = a->list_rep->begin(), ib = b->list_rep->begin();
        for (; ia != a->list_rep->end(); ++ia, ++ib) if (!value_eq(*ia, *ib)) return false;
        return true;
    }
    if (a->type == ValueType::Tuple && b->type == ValueType::Tuple) {
        if (a->tuple_rep->size() != b->tuple_rep->size()) return false;
        for (size_t i = 0; i < a->tuple_rep->size(); ++i)
            if (!value_eq((*a->tuple_rep)[i], (*b->tuple_rep)[i])) return false;
        return true;
    }
    if (a->type == ValueType::Pair && b->type == ValueType::Pair) {
        return value_eq(a->pair_rep->first, b->pair_rep->first) &&
               value_eq(a->pair_rep->second, b->pair_rep->second);
    }
    if (a->type == ValueType::Set && b->type == ValueType::Set) {
        if (a->set_rep->size() != b->set_rep->size()) return false;
        for (auto& ea : *a->set_rep) if (!b->set_rep->count(ea)) return false;
        return true;
    }
    if (a->type == ValueType::UndSet && b->type == ValueType::UndSet) {
        if (a->undset_rep->size() != b->undset_rep->size()) return false;
        for (auto& ea : *a->undset_rep) if (!b->undset_rep->count(ea)) return false;
        return true;
    }
    if (a->type == ValueType::Dict && b->type == ValueType::Dict) {
        if (a->dict_rep->size() != b->dict_rep->size()) return false;
        for (auto& [k, va] : *a->dict_rep) {
            auto it = b->dict_rep->find(k);
            if (it == b->dict_rep->end()) return false;
            if (!value_eq(va, it->second)) return false;
        }
        return true;
    }
    return a->to_string() == b->to_string();
}
bool value_ne(const ValuePtr& a, const ValuePtr& b) { return !value_eq(a, b); }

static int numeric_cmp(const ValuePtr& a, const ValuePtr& b) {
    Numeric A(a), B(b);
    double av = A.is_float ? A.d : (A.u ? (double)A.u : (double)A.i);
    double bv = B.is_float ? B.d : (B.u ? (double)B.u : (double)B.i);
    if (av < bv) return -1;
    if (av > bv) return 1;
    return 0;
}

static bool has_string(const ValuePtr& v) {
    return v->type == ValueType::Str || v->type == ValueType::UniStr ||
           v->type == ValueType::Char || v->type == ValueType::Unichar;
}
static std::string as_string(const ValuePtr& v) {
    if (v->type == ValueType::Char || v->type == ValueType::Unichar) return Value::make_char(v->char_val)->to_string();
    return v->str_val;
}

bool value_lt(const ValuePtr& a, const ValuePtr& b) {
    if (!a || !b) return false;
    if (a->type == ValueType::None) return b->type != ValueType::None;
    if (has_string(a) && has_string(b)) return as_string(a) < as_string(b);
    if (a->type == ValueType::List && b->type == ValueType::List) {
        auto ia = a->list_rep->begin(), ib = b->list_rep->begin();
        while (ia != a->list_rep->end() && ib != b->list_rep->end()) {
            if (value_lt(*ia, *ib)) return true;
            if (value_lt(*ib, *ia)) return false;
            ++ia; ++ib;
        }
        return ia == a->list_rep->end() && ib != b->list_rep->end();
    }
    if (a->type == ValueType::Tuple && b->type == ValueType::Tuple) {
        size_t n = std::min(a->tuple_rep->size(), b->tuple_rep->size());
        for (size_t i = 0; i < n; ++i) {
            if (value_lt((*a->tuple_rep)[i], (*b->tuple_rep)[i])) return true;
            if (value_lt((*b->tuple_rep)[i], (*a->tuple_rep)[i])) return false;
        }
        return a->tuple_rep->size() < b->tuple_rep->size();
    }
    return numeric_cmp(a, b) < 0;
}
bool value_le(const ValuePtr& a, const ValuePtr& b) { return value_lt(a, b) || value_eq(a, b); }
bool value_gt(const ValuePtr& a, const ValuePtr& b) { return value_lt(b, a); }
bool value_ge(const ValuePtr& a, const ValuePtr& b) { return value_lt(b, a) || value_eq(a, b); }

// ================ 集合运算 ================
ValuePtr value_intersection(const ValuePtr& a, const ValuePtr& b) {
    if (a->type == ValueType::Set) {
        auto r = Value::make_set();
        for (auto& e : *a->set_rep) if (b->set_rep->count(e)) r->set_rep->insert(e);
        return r;
    }
    if (a->type == ValueType::UndSet) {
        auto r = Value::make_undset();
        for (auto& e : *a->undset_rep) if (b->undset_rep->count(e)) r->undset_rep->insert(e);
        return r;
    }
    throw RuntimeError("Intersection on non-set");
}
ValuePtr value_union(const ValuePtr& a, const ValuePtr& b) {
    if (a->type == ValueType::Set) {
        auto r = Value::make_set();
        for (auto& e : *a->set_rep) r->set_rep->insert(e);
        for (auto& e : *b->set_rep) r->set_rep->insert(e);
        return r;
    }
    if (a->type == ValueType::UndSet) {
        auto r = Value::make_undset();
        for (auto& e : *a->undset_rep) r->undset_rep->insert(e);
        for (auto& e : *b->undset_rep) r->undset_rep->insert(e);
        return r;
    }
    throw RuntimeError("Union on non-set");
}
ValuePtr value_difference(const ValuePtr& a, const ValuePtr& b) {
    if (a->type == ValueType::Set) {
        auto r = Value::make_set();
        for (auto& e : *a->set_rep) if (!b->set_rep->count(e)) r->set_rep->insert(e);
        return r;
    }
    if (a->type == ValueType::UndSet) {
        auto r = Value::make_undset();
        for (auto& e : *a->undset_rep) if (!b->undset_rep->count(e)) r->undset_rep->insert(e);
        return r;
    }
    throw RuntimeError("Difference on non-set");
}
ValuePtr value_symmetric_diff(const ValuePtr& a, const ValuePtr& b) {
    if (a->type == ValueType::Set) {
        auto r = Value::make_set();
        for (auto& e : *a->set_rep) if (!b->set_rep->count(e)) r->set_rep->insert(e);
        for (auto& e : *b->set_rep) if (!a->set_rep->count(e)) r->set_rep->insert(e);
        return r;
    }
    if (a->type == ValueType::UndSet) {
        auto r = Value::make_undset();
        for (auto& e : *a->undset_rep) if (!b->undset_rep->count(e)) r->undset_rep->insert(e);
        for (auto& e : *b->undset_rep) if (!a->undset_rep->count(e)) r->undset_rep->insert(e);
        return r;
    }
    throw RuntimeError("Symmetric difference on non-set");
}

// ================ 类型转换 ================
ValuePtr value_to_int(const ValuePtr& a) {
    if (a->type == ValueType::Int) return a;
    if (a->type == ValueType::UInt) return Value::make_int((long long)a->uint_val);
    if (a->type == ValueType::Float) return Value::make_int((long long)a->float_val);
    if (a->type == ValueType::Bool) return Value::make_int(a->bool_val ? 1 : 0);
    if (a->type == ValueType::Char || a->type == ValueType::Unichar) return Value::make_int(a->char_val);
    if (a->type == ValueType::Str || a->type == ValueType::UniStr) {
        try { return Value::make_int(std::stoll(a->str_val)); }
        catch (...) { throw RuntimeError("Cannot convert '" + a->str_val + "' to int"); }
    }
    throw RuntimeError("Cannot convert " + a->type_name() + " to int");
}
ValuePtr value_to_long(const ValuePtr& a) { return value_to_int(a); } // 简化：long = long long
ValuePtr value_to_uint(const ValuePtr& a) {
    if (a->type == ValueType::UInt) return a;
    if (a->type == ValueType::Int) return Value::make_uint((unsigned long long)a->int_val);
    if (a->type == ValueType::Float) return Value::make_uint((unsigned long long)a->float_val);
    if (a->type == ValueType::Bool) return Value::make_uint(a->bool_val ? 1ull : 0ull);
    if (a->type == ValueType::Char) return Value::make_uint(a->char_val);
    if (a->type == ValueType::Str || a->type == ValueType::UniStr) {
        try { return Value::make_uint(std::stoull(a->str_val)); }
        catch (...) { throw RuntimeError("Cannot convert '" + a->str_val + "' to uint"); }
    }
    throw RuntimeError("Cannot convert " + a->type_name() + " to uint");
}
ValuePtr value_to_ulong(const ValuePtr& a) { return value_to_uint(a); }
ValuePtr value_to_float(const ValuePtr& a) {
    if (a->type == ValueType::Float) return a;
    if (a->type == ValueType::Int) return Value::make_float((double)a->int_val);
    if (a->type == ValueType::UInt) return Value::make_float((double)a->uint_val);
    if (a->type == ValueType::Bool) return Value::make_float(a->bool_val ? 1.0 : 0.0);
    if (a->type == ValueType::Char) return Value::make_float(a->char_val);
    if (a->type == ValueType::Str || a->type == ValueType::UniStr) {
        try { return Value::make_float(std::stod(a->str_val)); }
        catch (...) { throw RuntimeError("Cannot convert '" + a->str_val + "' to float"); }
    }
    throw RuntimeError("Cannot convert " + a->type_name() + " to float");
}
ValuePtr value_to_bool(const ValuePtr& a) { return Value::make_bool(a->truthy()); }
ValuePtr value_to_str(const ValuePtr& a) {
    if (a->type == ValueType::Str) return a;
    return Value::make_str(a->to_string());
}
ValuePtr value_to_unistr(const ValuePtr& a) {
    return Value::make_unistr(a->to_string());
}
ValuePtr value_to_bin(const ValuePtr& a) {
    if (a->type == ValueType::Bin) return a;
    if (a->type == ValueType::Str) {
        std::vector<unsigned char> v(a->str_val.begin(), a->str_val.end());
        return Value::make_bin(v);
    }
    if (a->type == ValueType::Int) {
        std::vector<unsigned char> v;
        long long x = a->int_val;
        for (int i = 0; i < 8; ++i) { v.push_back(x & 0xFF); x >>= 8; }
        return Value::make_bin(v);
    }
    throw RuntimeError("Cannot convert " + a->type_name() + " to bin");
}

// ================ format ================
std::string format_value(const ValuePtr& v, const std::string& spec) {
    std::ostringstream oss;
    if (spec.empty()) return v->to_string();
    // 简易处理：.2f / X (hex) / d / o / b
    char last = spec.back();
    bool is_hex_upper = (last == 'X');
    if (is_hex_upper) {
        long long n;
        if (v->type == ValueType::Int) n = v->int_val;
        else if (v->type == ValueType::UInt) n = (long long)v->uint_val;
        else return v->to_string();
        oss << std::uppercase << std::hex << n;
        return oss.str();
    }
    if (last == 'x') {
        long long n;
        if (v->type == ValueType::Int) n = v->int_val;
        else if (v->type == ValueType::UInt) n = (long long)v->uint_val;
        else return v->to_string();
        oss << std::hex << n;
        return oss.str();
    }
    if (spec.size() >= 2 && spec[0] == '.') {
        try {
            int prec = std::stoi(spec.substr(1, spec.size() - 1 - (last == 'f' || last == 'F' ? 1 : 0)));
            double d;
            if (v->type == ValueType::Float) d = v->float_val;
            else if (v->type == ValueType::Int) d = (double)v->int_val;
            else if (v->type == ValueType::UInt) d = (double)v->uint_val;
            else return v->to_string();
            oss << std::fixed << std::setprecision(prec) << d;
            return oss.str();
        } catch (...) {}
    }
    return v->to_string();
}

} // namespace vortex
