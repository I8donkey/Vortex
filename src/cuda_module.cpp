// ============================================================
// cuda_module.cpp — 实现见顶部注释
// ============================================================
#include "cuda_module.h"
#include "interpreter.h"
#include <thread>
#include <atomic>
#include <cmath>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <numeric>
#include <future>

namespace vortex {
namespace cuda_ext {

static inline size_t product(const std::vector<size_t>& v) {
    size_t r = 1; for (auto x : v) r *= x; return r;
}

// ---------- Tensor ----------
Tensor::Tensor(DType t, std::vector<size_t> sh)
    : dtype(t), shape(std::move(sh))
{
    // row-major strides (字节步长)
    const size_t elem_sz = dtype_bytes(dtype);
    strides.resize(shape.size());
    if (!shape.empty()) {
        strides.back() = elem_sz;
        for (int i = (int)shape.size() - 2; i >= 0; --i)
            strides[i] = strides[i + 1] * shape[i + 1];
    }
    data.resize(nbytes(), 0);
}

Tensor::~Tensor() {
    // 真实 CUDA 场景这里 cudaFree(device_ptr)
    device_ptr = nullptr;
}

size_t Tensor::nelem() const { return product(shape); }

size_t Tensor::index_of(const std::vector<size_t>& idx) const {
    if (idx.size() != shape.size())
        throw RuntimeError("Tensor index dim mismatch: expected " +
            std::to_string(shape.size()) + ", got " + std::to_string(idx.size()));
    size_t off = 0;
    for (size_t i = 0; i < idx.size(); ++i) {
        if (idx[i] >= shape[i])
            throw RuntimeError("Tensor index out of range at dim " + std::to_string(i));
        off += idx[i] * strides[i];
    }
    return off;
}

TensorPtr Tensor::clone() const {
    auto r = std::make_shared<Tensor>(dtype, shape);
    r->strides = strides;
    r->data = data;
    r->on_gpu = on_gpu;
    // device_ptr 在 CPU fallback 中忽略
    return r;
}

TensorPtr Tensor::reshape(std::vector<size_t> new_shape) const {
    // 允许一个维度为 -1 让其自动推导
    long long unknown = -1;
    long long known = 1;
    for (size_t i = 0; i < new_shape.size(); ++i) {
        if ((long long)new_shape[i] == -1) {
            if (unknown != -1) throw RuntimeError("reshape: only one -1 allowed");
            unknown = (long long)i;
        } else known *= (long long)new_shape[i];
    }
    if (unknown != -1) {
        if (known <= 0 || nelem() % (size_t)known != 0)
            throw RuntimeError("reshape: total size mismatch");
        new_shape[(size_t)unknown] = nelem() / (size_t)known;
    } else if (product(new_shape) != nelem())
        throw RuntimeError("reshape: total size mismatch");
    auto r = std::make_shared<Tensor>(dtype, new_shape);
    r->data = data;
    return r;
}

TensorPtr Tensor::t() const {
    if (shape.size() != 2) throw RuntimeError("transpose requires 2D tensor");
    const size_t M = shape[0], N = shape[1];
    auto r = std::make_shared<Tensor>(dtype, std::vector<size_t>{N, M});
    const size_t esz = dtype_bytes(dtype);
    for (size_t i = 0; i < M; ++i)
        for (size_t j = 0; j < N; ++j)
            std::memcpy(r->data.data() + (j * M + i) * esz,
                        data.data() + (i * N + j) * esz, esz);
    return r;
}

bool Tensor::to_host()   { return true; }
bool Tensor::to_device() { on_gpu = RuntimeState::instance().cuda_available; return true; }

// ---------- Runtime ----------
RuntimeState& RuntimeState::instance() {
    static RuntimeState s;
    static std::once_flag f;
    std::call_once(f, [&]{ s.ensure_initialized(); });
    return s;
}

void RuntimeState::ensure_initialized() {
    // 尝试探测真实 CUDA 环境（弱绑定，不直接依赖 cuda_runtime.h）。
    // 这里我们采用保守策略：默认未启用 CUDA，标记使用 CPU fallback。
    // 如果 CMake 开启 VORTEX_WITH_CUDA，可在此处改为调用 cudaGetDeviceCount。
    cuda_available = false;
#ifdef VORTEX_WITH_CUDA_REAL
    // 真实 CUDA 初始化占位
#endif
    device_count = cuda_available ? device_count : 0;
    driver_version  = cuda_available ? "12.x" : "n/a (CPU fallback)";
    runtime_version = cuda_available ? "12.x" : "n/a (CPU fallback)";
}

// ---------- 内部数值操作辅助（类型派发） ----------
template<class Fn>
static TensorPtr elemwise_apply(const TensorPtr& a, const TensorPtr& b, Fn fn, const char* op_name) {
    if (a->shape != b->shape)
        throw RuntimeError(std::string(op_name) + ": shape mismatch");
    auto r = std::make_shared<Tensor>(a->dtype, a->shape);
    const size_t n = a->nelem();
    const size_t esz = dtype_bytes(a->dtype);
    auto* srcA = a->data.data();
    auto* srcB = b->data.data();
    auto* dst  = r->data.data();

    unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    size_t chunk = (n + hw - 1) / hw;
    if (chunk == 0) chunk = n;
    std::vector<std::future<void>> futs;
    auto launch = [&](size_t s, size_t e) {
        for (size_t k = s; k < e; ++k) {
            size_t off = k * esz;
            switch (a->dtype) {
                case DType::F32: { float va, vb; std::memcpy(&va, srcA+off, 4); std::memcpy(&vb, srcB+off, 4); float v = fn(va,vb); std::memcpy(dst+off,&v,4); } break;
                case DType::F64: { double va, vb; std::memcpy(&va, srcA+off, 8); std::memcpy(&vb, srcB+off, 8); double v = fn(va,vb); std::memcpy(dst+off,&v,8); } break;
                case DType::I32: { int32_t va, vb; std::memcpy(&va, srcA+off, 4); std::memcpy(&vb, srcB+off, 4); int32_t v = (int32_t)fn(va,vb); std::memcpy(dst+off,&v,4); } break;
                case DType::I64: { int64_t va, vb; std::memcpy(&va, srcA+off, 8); std::memcpy(&vb, srcB+off, 8); int64_t v = (int64_t)fn(va,vb); std::memcpy(dst+off,&v,8); } break;
                case DType::U32: { uint32_t va, vb; std::memcpy(&va, srcA+off, 4); std::memcpy(&vb, srcB+off, 4); uint32_t v = (uint32_t)fn(va,vb); std::memcpy(dst+off,&v,4); } break;
                case DType::U64: { uint64_t va, vb; std::memcpy(&va, srcA+off, 8); std::memcpy(&vb, srcB+off, 8); uint64_t v = (uint64_t)fn(va,vb); std::memcpy(dst+off,&v,8); } break;
            }
        }
    };
    for (size_t s = 0; s < n; s += chunk) {
        size_t e = std::min(n, s + chunk);
        futs.push_back(std::async(std::launch::async, launch, s, e));
    }
    for (auto& f : futs) f.get();
    return r;
}

TensorPtr tensor_add(const TensorPtr& a, const TensorPtr& b) { return elemwise_apply(a,b,[](auto x,auto y){return x+y;},"tensor_add"); }
TensorPtr tensor_sub(const TensorPtr& a, const TensorPtr& b) { return elemwise_apply(a,b,[](auto x,auto y){return x-y;},"tensor_sub"); }
TensorPtr tensor_mul(const TensorPtr& a, const TensorPtr& b) { return elemwise_apply(a,b,[](auto x,auto y){return x*y;},"tensor_mul"); }
TensorPtr tensor_div(const TensorPtr& a, const TensorPtr& b) { return elemwise_apply(a,b,[](auto x,auto y){return x/y;},"tensor_div"); }

TensorPtr tensor_scale(const TensorPtr& a, double s) {
    auto r = std::make_shared<Tensor>(a->dtype, a->shape);
    const size_t n = a->nelem();
    const size_t esz = dtype_bytes(a->dtype);
    auto* src = a->data.data(); auto* dst = r->data.data();
    for (size_t k = 0; k < n; ++k) {
        size_t off = k * esz;
        switch (a->dtype) {
            case DType::F32: { float v; std::memcpy(&v,src+off,4); v = (float)(v * s); std::memcpy(dst+off,&v,4); } break;
            case DType::F64: { double v; std::memcpy(&v,src+off,8); v = v * s; std::memcpy(dst+off,&v,8); } break;
            case DType::I32: { int32_t v; std::memcpy(&v,src+off,4); v = (int32_t)(v * s); std::memcpy(dst+off,&v,4); } break;
            case DType::I64: { int64_t v; std::memcpy(&v,src+off,8); v = (int64_t)(v * s); std::memcpy(dst+off,&v,8); } break;
            case DType::U32: { uint32_t v; std::memcpy(&v,src+off,4); v = (uint32_t)(v * s); std::memcpy(dst+off,&v,4); } break;
            case DType::U64: { uint64_t v; std::memcpy(&v,src+off,8); v = (uint64_t)(v * s); std::memcpy(dst+off,&v,8); } break;
        }
    }
    return r;
}

// ---------- matmul ----------
template<class T>
static void matmul_impl(const T* A, const T* B, T* C, size_t M, size_t K, size_t N) {
    // 多线程阻塞矩阵乘法（CPU fallback，已足够演示 GPU 加速接口）
    std::vector<std::future<void>> futs;
    unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    size_t row_chunk = std::max<size_t>(1, (M + hw - 1) / hw);
    for (size_t rs = 0; rs < M; rs += row_chunk) {
        size_t re = std::min(M, rs + row_chunk);
        futs.push_back(std::async(std::launch::async, [=] {
            const size_t BLOC = 64;
            for (size_t kk = 0; kk < K; kk += BLOC) {
                size_t ke = std::min(K, kk + BLOC);
                for (size_t i = rs; i < re; ++i) {
                    for (size_t k = kk; k < ke; ++k) {
                        T a = A[i * K + k];
                        for (size_t j = 0; j < N; ++j) {
                            C[i * N + j] += a * B[k * N + j];
                        }
                    }
                }
            }
        }));
    }
    for (auto& f : futs) f.get();
}

TensorPtr tensor_matmul(const TensorPtr& a, const TensorPtr& b) {
    if (a->shape.size() != 2 || b->shape.size() != 2)
        throw RuntimeError("matmul requires 2D tensors");
    size_t M = a->shape[0], K = a->shape[1];
    size_t Bk = b->shape[0], N = b->shape[1];
    if (K != Bk) throw RuntimeError("matmul inner dim mismatch");
    if (a->dtype != b->dtype) throw RuntimeError("matmul dtype mismatch");
    auto r = std::make_shared<Tensor>(a->dtype, std::vector<size_t>{M, N});
    const size_t esz = dtype_bytes(a->dtype);
    auto* A = a->data.data();
    auto* B = b->data.data();
    auto* C = r->data.data();
    // 清 0
    std::memset(C, 0, r->nbytes());
    switch (a->dtype) {
        case DType::F32: matmul_impl<float>   ((const float*)A,   (const float*)B,   (float*)C,   M, K, N); break;
        case DType::F64: matmul_impl<double>  ((const double*)A,  (const double*)B,  (double*)C,  M, K, N); break;
        case DType::I32: matmul_impl<int32_t> ((const int32_t*)A, (const int32_t*)B, (int32_t*)C, M, K, N); break;
        case DType::I64: matmul_impl<int64_t> ((const int64_t*)A, (const int64_t*)B, (int64_t*)C, M, K, N); break;
        default: throw RuntimeError("matmul: unsupported dtype");
    }
    return r;
}

// ---------- reduce ----------
template<class T, class Sum, class Zero>
static T reduce_all(const TensorPtr& t, Sum sum, Zero zero) {
    const T* p = reinterpret_cast<const T*>(t->data.data());
    size_t n = t->nelem();
    T acc = zero();
    unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    size_t chunk = (n + hw - 1) / hw;
    if (chunk == 0) chunk = n;
    std::vector<std::future<T>> futs;
    for (size_t s = 0; s < n; s += chunk) {
        size_t e = std::min(n, s + chunk);
        futs.push_back(std::async(std::launch::async, [=] {
            T a = zero();
            for (size_t k = s; k < e; ++k) a = sum(a, p[k]);
            return a;
        }));
    }
    for (auto& f : futs) acc = sum(acc, f.get());
    return acc;
}

static ValuePtr as_value(DType t, double d, long long i, unsigned long long u) {
    switch (t) {
        case DType::F32: case DType::F64: return Value::make_float(d);
        case DType::I32: case DType::I64: return Value::make_int(i);
        case DType::U32: case DType::U64: return Value::make_uint(u);
    }
    return Value::make_none();
}

ValuePtr tensor_reduce_sum(const TensorPtr& t, int /*axis*/) {
    switch (t->dtype) {
        case DType::F32: { auto v = reduce_all<float>(t, [](float a,float b){return a+b;}, []{return 0.f;}); return Value::make_float(v); }
        case DType::F64: { auto v = reduce_all<double>(t, [](double a,double b){return a+b;}, []{return 0.0;}); return Value::make_float(v); }
        case DType::I32: { auto v = reduce_all<int32_t>(t, [](int32_t a,int32_t b){return a+b;}, []{return 0;}); return Value::make_int(v); }
        case DType::I64: { auto v = reduce_all<int64_t>(t, [](int64_t a,int64_t b){return a+b;}, []{return 0;}); return Value::make_int(v); }
        case DType::U32: { auto v = reduce_all<uint32_t>(t, [](uint32_t a,uint32_t b){return a+b;}, []{return 0u;}); return Value::make_uint(v); }
        case DType::U64: { auto v = reduce_all<uint64_t>(t, [](uint64_t a,uint64_t b){return a+b;}, []{return 0ull;}); return Value::make_uint(v); }
    }
    return Value::make_none();
}
ValuePtr tensor_reduce_max(const TensorPtr& t, int) {
    switch (t->dtype) {
        case DType::F32: { auto v = reduce_all<float>(t,  [](float a,float b){return std::max(a,b);},   []{return -HUGE_VALF;}); return Value::make_float(v); }
        case DType::F64: { auto v = reduce_all<double>(t, [](double a,double b){return std::max(a,b);},  []{return -HUGE_VAL;}); return Value::make_float(v); }
        case DType::I32: { auto v = reduce_all<int32_t>(t,[](int32_t a,int32_t b){return std::max(a,b);},[]{return INT32_MIN;}); return Value::make_int(v); }
        case DType::I64: { auto v = reduce_all<int64_t>(t,[](int64_t a,int64_t b){return std::max(a,b);},[]{return INT64_MIN;}); return Value::make_int(v); }
        default: return tensor_reduce_sum(t, 0);
    }
}
ValuePtr tensor_reduce_min(const TensorPtr& t, int) {
    switch (t->dtype) {
        case DType::F32: { auto v = reduce_all<float>(t,  [](float a,float b){return std::min(a,b);},   []{return HUGE_VALF;}); return Value::make_float(v); }
        case DType::F64: { auto v = reduce_all<double>(t, [](double a,double b){return std::min(a,b);},  []{return HUGE_VAL;}); return Value::make_float(v); }
        case DType::I32: { auto v = reduce_all<int32_t>(t,[](int32_t a,int32_t b){return std::min(a,b);},[]{return INT32_MAX;}); return Value::make_int(v); }
        case DType::I64: { auto v = reduce_all<int64_t>(t,[](int64_t a,int64_t b){return std::min(a,b);},[]{return INT64_MAX;}); return Value::make_int(v); }
        default: return tensor_reduce_sum(t, 0);
    }
}
ValuePtr tensor_reduce_mean(const TensorPtr& t, int) {
    auto s = tensor_reduce_sum(t, 0);
    double n = (double)t->nelem();
    if (s->type == ValueType::Float) return Value::make_float(s->float_val / n);
    if (s->type == ValueType::Int)   return Value::make_float((double)s->int_val / n);
    if (s->type == ValueType::UInt)  return Value::make_float((double)s->uint_val / n);
    return Value::make_float(0.0);
}

// ---------- element-wise apply ----------
TensorPtr tensor_elem_apply(const TensorPtr& t, const std::string& op) {
    auto r = std::make_shared<Tensor>(t->dtype, t->shape);
    const size_t n = t->nelem();
    const size_t esz = dtype_bytes(t->dtype);
    auto* src = t->data.data(); auto* dst = r->data.data();
    // 数值操作仅对浮点类型，整数先转 float 再计算再存回
    auto apply = [&](auto f) {
        for (size_t k = 0; k < n; ++k) {
            size_t off = k * esz;
            double v = 0.0;
            switch (t->dtype) {
                case DType::F32: { float x; std::memcpy(&x,src+off,4); v = x; } break;
                case DType::F64: { double x; std::memcpy(&x,src+off,8); v = x; } break;
                case DType::I32: { int32_t x; std::memcpy(&x,src+off,4); v = x; } break;
                case DType::I64: { int64_t x; std::memcpy(&x,src+off,8); v = x; } break;
                default: break;
            }
            double y = f(v);
            switch (t->dtype) {
                case DType::F32: { float x = (float)y; std::memcpy(dst+off,&x,4); } break;
                case DType::F64: { std::memcpy(dst+off,&y,8); } break;
                case DType::I32: { int32_t x = (int32_t)y; std::memcpy(dst+off,&x,4); } break;
                case DType::I64: { int64_t x = (int64_t)y; std::memcpy(dst+off,&x,8); } break;
                default: break;
            }
        }
    };
    if      (op == "sqrt")   apply([](double x){ return std::sqrt(x); });
    else if (op == "exp")    apply([](double x){ return std::exp(x); });
    else if (op == "log")    apply([](double x){ return std::log(x); });
    else if (op == "sin")    apply([](double x){ return std::sin(x); });
    else if (op == "cos")    apply([](double x){ return std::cos(x); });
    else if (op == "tan")    apply([](double x){ return std::tan(x); });
    else if (op == "abs")    apply([](double x){ return std::fabs(x); });
    else if (op == "relu")   apply([](double x){ return x > 0 ? x : 0; });
    else if (op == "sigmoid")apply([](double x){ return 1.0 / (1.0 + std::exp(-x)); });
    else if (op == "tanh")   apply([](double x){ return std::tanh(x); });
    else throw RuntimeError("unknown element-wise op: " + op);
    return r;
}

} // namespace cuda_ext

// ============================================================
// 模块注册：cuda 模块
// ============================================================
void register_cuda_module(std::unordered_map<std::string, ValuePtr>& std_modules) {
    using namespace cuda_ext;
    auto mod = Value::make_module();
    auto& u = *mod->module_rep;

    auto mk_fn = [](const std::string& mname, const std::string& name, size_t min_a, size_t max_a,
                    std::function<ValuePtr(const ValueVec&)> fn) {
        auto fv = std::make_shared<FunctionValue>();
        fv->name = name; fv->is_builtin = true;
        fv->builtin_fn = [mname, name, min_a, max_a, fn](const ValueVec& args, Environment&) -> ValuePtr {
            if (args.size() < min_a || (max_a != (size_t)-1 && args.size() > max_a))
                throw RuntimeError(mname + "." + name + " expects " +
                    std::to_string(min_a) + "~" + std::to_string(max_a) + " args, got " +
                    std::to_string(args.size()));
            return fn(args);
        };
        auto v = Value::make_none(); v->type = ValueType::Function; v->fn_rep = fv;
        return v;
    };
    auto as_tensor = [](const ValuePtr& v) -> TensorPtr {
        if (v->type != ValueType::Opaque || !v->opaque_rep)
            throw RuntimeError("expected tensor (opaque<cuda_tensor>)");
        if (v->opaque_rep->kind != "cuda_tensor")
            throw RuntimeError("expected cuda_tensor, got " + v->opaque_rep->kind);
        auto* tp = v->opaque_rep->try_as<TensorPtr>();
        if (!tp) throw RuntimeError("invalid tensor payload");
        return *tp;
    };
    auto wrap_tensor = [](TensorPtr t) {
        auto res = std::make_shared<OpaqueResource>("cuda_tensor", std::move(t));
        return Value::make_opaque(res);
    };
    auto dtype_from_tag = [](const std::string& s) -> DType {
        if (s == "f32" || s == "float") return DType::F32;
        if (s == "f64" || s == "double") return DType::F64;
        if (s == "i32" || s == "int") return DType::I32;
        if (s == "i64" || s == "long") return DType::I64;
        if (s == "u32" || s == "uint") return DType::U32;
        if (s == "u64" || s == "ulong") return DType::U64;
        throw RuntimeError("unknown dtype tag: " + s);
    };

    RuntimeState::instance().ensure_initialized();

    // ---- 常量 ----
    u["available"] = Value::make_bool(RuntimeState::instance().cuda_available);
    u["device_count"] = Value::make_int(RuntimeState::instance().device_count);
    u["current_device"] = Value::make_int(RuntimeState::instance().current_device);
    u["backend"] = Value::make_str(std::string(RuntimeState::instance().cuda_available ? "cuda" : "cpu_threads"));
    u["driver_version"]  = Value::make_str(RuntimeState::instance().driver_version);
    u["runtime_version"] = Value::make_str(RuntimeState::instance().runtime_version);
    // dtype 字符串常量："f32"/"f64"/"i32"/"i64"/"u32"/"u64"
    u["F32"]  = Value::make_str("f32");
    u["F64"]  = Value::make_str("f64");
    u["I32"]  = Value::make_str("i32");
    u["I64"]  = Value::make_str("i64");
    u["U32"]  = Value::make_str("u32");
    u["U64"]  = Value::make_str("u64");

    // ---- 函数 ----
    auto add = [&](const std::string& n, size_t a0, size_t a1,
                   std::function<ValuePtr(const ValueVec&)> f) {
        u[n] = mk_fn("cuda", n, a0, a1, std::move(f));
    };
    // device_count/get_device_count 函数风格查询
    add("get_device_count", 0, 0, [&](const ValueVec&) {
        return Value::make_int(RuntimeState::instance().device_count);
    });

    // device_affinity() — 当前使用设备号
    add("device_id", 0, 0, [&](const ValueVec&) {
        return Value::make_int(RuntimeState::instance().current_device);
    });

    // tensor(dtype, shape_list, [init_list])
    add("tensor", 2, 3, [&](const ValueVec& a) -> ValuePtr {
        std::string tag = a[0]->to_string();
        DType dt = dtype_from_tag(tag);
        if (a[1]->type != ValueType::List) throw RuntimeError("cuda.tensor: shape must be list");
        std::vector<size_t> sh;
        for (auto& e : *a[1]->list_rep) sh.push_back((size_t)value_to_int(e)->int_val);
        auto t = std::make_shared<Tensor>(dt, sh);
        if (a.size() >= 3) {
            // 可选初始化：一维展平列表
            if (a[2]->type != ValueType::List)
                throw RuntimeError("cuda.tensor: init data must be flat list");
            // 将 list 复制到 vector 以便随机访问（list 不支持 operator[]）
            std::vector<ValuePtr> vec(a[2]->list_rep->begin(), a[2]->list_rep->end());
            size_t esz = dtype_bytes(dt);
            size_t n = t->nelem();
            if (vec.size() != n)
                throw RuntimeError("cuda.tensor: init list size mismatch (" +
                    std::to_string(vec.size()) + " vs " + std::to_string(n) + ")");
            for (size_t i = 0; i < n; ++i) {
                size_t off = i * esz;
                switch (dt) {
                    case DType::F32: { float v = (float)value_to_float(vec[i])->float_val; std::memcpy(t->data.data()+off, &v, 4); } break;
                    case DType::F64: { double v = value_to_float(vec[i])->float_val; std::memcpy(t->data.data()+off, &v, 8); } break;
                    case DType::I32: { int32_t v = (int32_t)value_to_int(vec[i])->int_val; std::memcpy(t->data.data()+off, &v, 4); } break;
                    case DType::I64: { int64_t v = (int64_t)value_to_int(vec[i])->int_val; std::memcpy(t->data.data()+off, &v, 8); } break;
                    case DType::U32: { uint32_t v = (uint32_t)value_to_uint(vec[i])->uint_val; std::memcpy(t->data.data()+off, &v, 4); } break;
                    case DType::U64: { uint64_t v = (uint64_t)value_to_uint(vec[i])->uint_val; std::memcpy(t->data.data()+off, &v, 8); } break;
                }
            }
        }
        return wrap_tensor(t);
    });

    // zeros / ones / full(dtype, shape, value)
    add("zeros", 2, 2, [&](const ValueVec& a) {
        std::vector<size_t> sh;
        for (auto& e : *a[1]->list_rep) sh.push_back((size_t)value_to_int(e)->int_val);
        auto t = std::make_shared<Tensor>(dtype_from_tag(a[0]->to_string()), sh);
        return wrap_tensor(t);
    });
    add("ones", 2, 2, [&](const ValueVec& a) {
        std::vector<size_t> sh;
        for (auto& e : *a[1]->list_rep) sh.push_back((size_t)value_to_int(e)->int_val);
        DType dt = dtype_from_tag(a[0]->to_string());
        auto t = std::make_shared<Tensor>(dt, sh);
        size_t n = t->nelem();
        size_t esz = dtype_bytes(dt);
        for (size_t i = 0; i < n; ++i) {
            size_t off = i * esz;
            switch (dt) {
                case DType::F32: { float v = 1.f; std::memcpy(t->data.data()+off,&v,4);} break;
                case DType::F64: { double v = 1.0; std::memcpy(t->data.data()+off,&v,8);} break;
                case DType::I32: { int32_t v = 1; std::memcpy(t->data.data()+off,&v,4);} break;
                case DType::I64: { int64_t v = 1; std::memcpy(t->data.data()+off,&v,8);} break;
                case DType::U32: { uint32_t v = 1; std::memcpy(t->data.data()+off,&v,4);} break;
                case DType::U64: { uint64_t v = 1; std::memcpy(t->data.data()+off,&v,8);} break;
            }
        }
        return wrap_tensor(t);
    });
    add("full", 3, 3, [&](const ValueVec& a) {
        std::vector<size_t> sh;
        for (auto& e : *a[1]->list_rep) sh.push_back((size_t)value_to_int(e)->int_val);
        DType dt = dtype_from_tag(a[0]->to_string());
        auto t = std::make_shared<Tensor>(dt, sh);
        double fv = value_to_float(a[2])->float_val;
        long long iv = value_to_int(a[2])->int_val;
        unsigned long long uv = value_to_uint(a[2])->uint_val;
        size_t n = t->nelem();
        size_t esz = dtype_bytes(dt);
        for (size_t i = 0; i < n; ++i) {
            size_t off = i * esz;
            switch (dt) {
                case DType::F32: { float v = (float)fv; std::memcpy(t->data.data()+off,&v,4);} break;
                case DType::F64: { std::memcpy(t->data.data()+off,&fv,8);} break;
                case DType::I32: { int32_t v = (int32_t)iv; std::memcpy(t->data.data()+off,&v,4);} break;
                case DType::I64: { int64_t v = (int64_t)iv; std::memcpy(t->data.data()+off,&v,8);} break;
                case DType::U32: { uint32_t v = (uint32_t)uv; std::memcpy(t->data.data()+off,&v,4);} break;
                case DType::U64: { std::memcpy(t->data.data()+off,&uv,8);} break;
            }
        }
        return wrap_tensor(t);
    });

    // shape(tensor) -> list
    add("shape", 1, 1, [&](const ValueVec& a) {
        auto t = as_tensor(a[0]);
        auto lst = Value::make_list();
        for (auto s : t->shape) lst->list_rep->push_back(Value::make_uint((unsigned long long)s));
        return lst;
    });
    // dtype(tensor) -> str
    add("dtype", 1, 1, [&](const ValueVec& a) {
        auto t = as_tensor(a[0]);
        return Value::make_str(dtype_name(t->dtype));
    });
    // to_list(tensor) -> list (flatten)
    add("to_list", 1, 1, [&](const ValueVec& a) {
        auto t = as_tensor(a[0]);
        auto lst = Value::make_list();
        size_t n = t->nelem();
        size_t esz = dtype_bytes(t->dtype);
        for (size_t i = 0; i < n; ++i) {
            size_t off = i * esz;
            switch (t->dtype) {
                case DType::F32: { float v; std::memcpy(&v, t->data.data()+off, 4); lst->list_rep->push_back(Value::make_float(v));} break;
                case DType::F64: { double v; std::memcpy(&v, t->data.data()+off, 8); lst->list_rep->push_back(Value::make_float(v));} break;
                case DType::I32: { int32_t v; std::memcpy(&v, t->data.data()+off, 4); lst->list_rep->push_back(Value::make_int(v));} break;
                case DType::I64: { int64_t v; std::memcpy(&v, t->data.data()+off, 8); lst->list_rep->push_back(Value::make_int(v));} break;
                case DType::U32: { uint32_t v; std::memcpy(&v, t->data.data()+off, 4); lst->list_rep->push_back(Value::make_uint(v));} break;
                case DType::U64: { uint64_t v; std::memcpy(&v, t->data.data()+off, 8); lst->list_rep->push_back(Value::make_uint(v));} break;
            }
        }
        return lst;
    });

    // 数学运算
    add("add", 2, 2, [&](const ValueVec& a) { return wrap_tensor(tensor_add(as_tensor(a[0]), as_tensor(a[1]))); });
    add("sub", 2, 2, [&](const ValueVec& a) { return wrap_tensor(tensor_sub(as_tensor(a[0]), as_tensor(a[1]))); });
    add("mul", 2, 2, [&](const ValueVec& a) { return wrap_tensor(tensor_mul(as_tensor(a[0]), as_tensor(a[1]))); });
    add("div", 2, 2, [&](const ValueVec& a) { return wrap_tensor(tensor_div(as_tensor(a[0]), as_tensor(a[1]))); });
    add("scale", 2, 2, [&](const ValueVec& a) { return wrap_tensor(tensor_scale(as_tensor(a[0]), value_to_float(a[1])->float_val)); });
    add("matmul", 2, 2, [&](const ValueVec& a) { return wrap_tensor(tensor_matmul(as_tensor(a[0]), as_tensor(a[1]))); });

    // Reduce
    add("sum",  1, 2, [&](const ValueVec& a) { return tensor_reduce_sum (as_tensor(a[0]), a.size()>=2 ? (int)value_to_int(a[1])->int_val : -1); });
    add("max",  1, 2, [&](const ValueVec& a) { return tensor_reduce_max (as_tensor(a[0]), a.size()>=2 ? (int)value_to_int(a[1])->int_val : -1); });
    add("min",  1, 2, [&](const ValueVec& a) { return tensor_reduce_min (as_tensor(a[0]), a.size()>=2 ? (int)value_to_int(a[1])->int_val : -1); });
    add("mean", 1, 2, [&](const ValueVec& a) { return tensor_reduce_mean(as_tensor(a[0]), a.size()>=2 ? (int)value_to_int(a[1])->int_val : -1); });

    // Element-wise transforms
    add("sqrt", 1, 1, [&](const ValueVec& a) { return wrap_tensor(tensor_elem_apply(as_tensor(a[0]), "sqrt")); });
    add("exp",  1, 1, [&](const ValueVec& a) { return wrap_tensor(tensor_elem_apply(as_tensor(a[0]), "exp"));  });
    add("log",  1, 1, [&](const ValueVec& a) { return wrap_tensor(tensor_elem_apply(as_tensor(a[0]), "log"));  });
    add("sin",  1, 1, [&](const ValueVec& a) { return wrap_tensor(tensor_elem_apply(as_tensor(a[0]), "sin"));  });
    add("cos",  1, 1, [&](const ValueVec& a) { return wrap_tensor(tensor_elem_apply(as_tensor(a[0]), "cos"));  });
    add("relu", 1, 1, [&](const ValueVec& a) { return wrap_tensor(tensor_elem_apply(as_tensor(a[0]), "relu")); });
    add("sigmoid", 1, 1, [&](const ValueVec& a) { return wrap_tensor(tensor_elem_apply(as_tensor(a[0]), "sigmoid")); });
    add("tanh", 1, 1, [&](const ValueVec& a) { return wrap_tensor(tensor_elem_apply(as_tensor(a[0]), "tanh")); });
    add("abs",  1, 1, [&](const ValueVec& a) { return wrap_tensor(tensor_elem_apply(as_tensor(a[0]), "abs"));  });

    // 结构操作
    add("reshape", 2, 2, [&](const ValueVec& a) {
        auto t = as_tensor(a[0]);
        std::vector<size_t> sh;
        for (auto& e : *a[1]->list_rep) {
            long long v = value_to_int(e)->int_val;
            sh.push_back(v == -1 ? (size_t)-1 : (size_t)v);
        }
        return wrap_tensor(t->reshape(sh));
    });
    add("transpose", 1, 1, [&](const ValueVec& a) { return wrap_tensor(as_tensor(a[0])->t()); });
    add("clone", 1, 1, [&](const ValueVec& a) { return wrap_tensor(as_tensor(a[0])->clone()); });
    add("to_device", 1, 1, [&](const ValueVec& a) { auto t = as_tensor(a[0]); t->to_device(); return a[0]; });
    add("to_host",   1, 1, [&](const ValueVec& a) { auto t = as_tensor(a[0]); t->to_host();   return a[0]; });

    // synchronize — 占位（真实 CUDA 会等待流）
    add("synchronize", 0, 0, [](const ValueVec&) { return Value::make_none(); });

    std_modules["cuda"] = mod;
}

} // namespace vortex
