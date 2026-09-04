// ============================================================
// cuda_module.h / cuda_module.cpp
// Vortex CUDA 加速计算模块 — 设备管理、张量、并行矩阵乘、
// 向量运算与 Reduce。所有 API 在无 CUDA SDK 时自动退化为
// 多线程 CPU 实现，保证脚本层接口完全一致。
// ============================================================
#ifndef VORTEX_CUDA_MODULE_H
#define VORTEX_CUDA_MODULE_H

#include "value.h"
#include <unordered_map>
#include <string>
#include <vector>
#include <memory>
#include <mutex>

namespace vortex {
namespace cuda_ext {

// ---------- 数据类型 tag（与 Vortex 值类型对齐） ----------
enum class DType { F32, F64, I32, I64, U32, U64 };
inline size_t dtype_bytes(DType t) {
    switch (t) {
        case DType::F32: return 4; case DType::F64: return 8;
        case DType::I32: return 4; case DType::I64: return 8;
        case DType::U32: return 4; case DType::U64: return 8;
    }
    return 0;
}
inline const char* dtype_name(DType t) {
    switch (t) {
        case DType::F32: return "f32"; case DType::F64: return "f64";
        case DType::I32: return "i32"; case DType::I64: return "i64";
        case DType::U32: return "u32"; case DType::U64: return "u64";
    }
    return "?";
}

// ---------- Tensor 对象（CPU fallback 实现） ----------
// 真实 CUDA 版本会在此结构中增加 device_ptr / cudaStream 等成员。
struct Tensor {
    DType dtype = DType::F32;
    std::vector<size_t> shape;
    std::vector<size_t> strides;      // row-major 步长（字节）
    std::vector<unsigned char> data;  // host-side storage (fallback)

    // CUDA 专用字段（未启用 CUDA 时不分配）
    bool on_gpu = false;
    void* device_ptr = nullptr;

    Tensor() = default;
    Tensor(DType t, std::vector<size_t> sh);
    ~Tensor();

    size_t ndim() const { return shape.size(); }
    size_t nelem() const;
    size_t nbytes() const { return nelem() * dtype_bytes(dtype); }

    // 按多维下标计算线性索引
    size_t index_of(const std::vector<size_t>& idx) const;

    // 元素访问（模板按类型）
    template<class T> T& at(const std::vector<size_t>& idx) {
        return *reinterpret_cast<T*>(data.data() + index_of(idx));
    }
    template<class T> const T& at(const std::vector<size_t>& idx) const {
        return *reinterpret_cast<const T*>(data.data() + index_of(idx));
    }

    // 拷贝 / 转置 / 重塑（浅语义，实际复制数据）
    std::shared_ptr<Tensor> clone() const;
    std::shared_ptr<Tensor> reshape(std::vector<size_t> new_shape) const;
    std::shared_ptr<Tensor> t() const; // 2D transpose

    // 同步 GPU <-> CPU（CUDA 禁用时为空操作，返回 true）
    bool to_host();
    bool to_device();
};

using TensorPtr = std::shared_ptr<Tensor>;

// ---------- Runtime 状态 ----------
struct RuntimeState {
    bool cuda_available = false;
    int  device_count = 0;
    int  current_device = 0;
    std::string driver_version;
    std::string runtime_version;
    std::mutex mu;

    static RuntimeState& instance();
    // 在首次调用时探测 CUDA 能力；探测失败即切换为 CPU fallback
    void ensure_initialized();
};

// ---------- 算子（CPU fallback 多线程实现） ----------
TensorPtr tensor_add(const TensorPtr& a, const TensorPtr& b);
TensorPtr tensor_sub(const TensorPtr& a, const TensorPtr& b);
TensorPtr tensor_mul(const TensorPtr& a, const TensorPtr& b); // elementwise
TensorPtr tensor_div(const TensorPtr& a, const TensorPtr& b);
TensorPtr tensor_scale(const TensorPtr& a, double s);

// 矩阵乘法：C[M,N] = A[M,K] * B[K,N]
TensorPtr tensor_matmul(const TensorPtr& a, const TensorPtr& b);

// 归约：sum / max / min / mean （沿轴或全部）
ValuePtr tensor_reduce_sum (const TensorPtr& t, int axis = -1);
ValuePtr tensor_reduce_max (const TensorPtr& t, int axis = -1);
ValuePtr tensor_reduce_min (const TensorPtr& t, int axis = -1);
ValuePtr tensor_reduce_mean(const TensorPtr& t, int axis = -1);

// 向量化应用：sqrt / exp / log / sin / cos / relu / sigmoid
TensorPtr tensor_elem_apply(const TensorPtr& t, const std::string& op);

} // namespace cuda_ext

// 模块注册入口（放入 interpreter 的 std_modules_）
void register_cuda_module(std::unordered_map<std::string, ValuePtr>& std_modules);

} // namespace vortex
#endif
