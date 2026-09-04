// ============================================================
// thread_module.cpp — 多线程模块实现
// ============================================================
#include "thread_module.h"
#include "interpreter.h"
#include <chrono>
#include <sstream>
#include <stdexcept>

namespace vortex {

// 将资源句柄包装为 Opaque Value（namespace 级，避免功能 lambda 捕获局部变量导致悬空引用）
template <class T>
inline ValuePtr thread_wrap(const char* kind, T ptr) {
    auto res = std::make_shared<OpaqueResource>(kind, ptr);
    return Value::make_opaque(res);
}

// 解释器执行互斥锁：解释器的 current_env_ 等为共享可变状态，
// 多线程并发执行用户函数会导致 current_env_ 互相覆盖（读到别的线程栈上的变量，
// 引发死循环/崩溃）。用递归互斥锁序列化工作线程对解释器的访问，
// 保证原子操作等仍并发，仅串行化解释器状态访问。
static std::recursive_mutex g_interp_exec_mutex;

void register_thread_module(std::unordered_map<std::string, ValuePtr>& std_modules) {
    auto mod = Value::make_module();
    auto& u = *mod->module_rep;

    // 注册函数的辅助
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
    auto add = [&](const std::string& n, size_t a0, size_t a1,
                   std::function<ValuePtr(const ValueVec&)> f) {
        u[n] = mk_fn("thread", n, a0, a1, std::move(f));
    };

    // ==== 线程管理 ====
    add("run", 1, 1, [&](const ValueVec& a) -> ValuePtr {
        if (a[0]->type != ValueType::Function)
            throw RuntimeError("thread.run: expects a function");
        auto fn = a[0]->fn_rep;
        extern Interpreter* g_thread_active_interpreter;
        auto interp = g_thread_active_interpreter;
        if (!interp) throw RuntimeError("thread.run: no interpreter bound");
        auto th = std::make_shared<ThreadHandle>();
        // 将解释器指针和函数捕获到线程
        th->thread = std::thread([interp, fn]() {
            // 序列化对解释器共享状态（current_env_ 等）的访问，避免并发崩溃
            std::lock_guard<std::recursive_mutex> lk(g_interp_exec_mutex);
            // 在子线程中执行函数
            try {
                ValueVec empty;
                if (fn->builtin_fn) {
                    Environment env(&interp->globals());
                    fn->builtin_fn(empty, env);
                } else if (fn->def) {
                    interp->call_user_function(fn.get(), empty);
                }
            } catch (...) {
                // 静默吞掉异常
            }
        });
        return thread_wrap("thread", th);
    });

    add("join", 1, 1, [&](const ValueVec& a) -> ValuePtr {
        auto th = std::static_pointer_cast<OpaqueResource>(a[0]->opaque_rep);
        if (!th || th->kind != "thread")
            throw RuntimeError("thread.join: not a thread handle");
        auto handle = std::any_cast<ThreadPtr>(th->payload);
        if (handle->joined || handle->detached)
            throw RuntimeError("thread.join: already joined or detached");
        if (handle->thread.joinable()) handle->thread.join();
        handle->joined = true;
        return Value::make_none();
    });

    add("detach", 1, 1, [&](const ValueVec& a) -> ValuePtr {
        auto th = std::static_pointer_cast<OpaqueResource>(a[0]->opaque_rep);
        if (!th || th->kind != "thread")
            throw RuntimeError("thread.detach: not a thread handle");
        auto handle = std::any_cast<ThreadPtr>(th->payload);
        if (handle->joined || handle->detached)
            throw RuntimeError("thread.detach: already joined or detached");
        if (handle->thread.joinable()) handle->thread.detach();
        handle->detached = true;
        return Value::make_none();
    });

    add("sleep", 1, 1, [&](const ValueVec& a) -> ValuePtr {
        long long ms = value_to_int(a[0])->int_val;
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return Value::make_none();
    });

    add("yield", 0, 0, [&](const ValueVec&) -> ValuePtr {
        std::this_thread::yield();
        return Value::make_none();
    });

    add("id", 0, 0, [&](const ValueVec&) -> ValuePtr {
        std::ostringstream oss;
        oss << std::this_thread::get_id();
        return Value::make_str(oss.str());
    });

    add("hardware", 0, 0, [&](const ValueVec&) -> ValuePtr {
        unsigned n = std::thread::hardware_concurrency();
        return Value::make_int((long long)(n > 0 ? n : 0));
    });

    // ==== 互斥锁 ====
    add("mutex", 0, 0, [&](const ValueVec&) -> ValuePtr {
        auto m = std::make_shared<MutexHandle>();
        return thread_wrap("mutex", m);
    });
    add("lock", 1, 1, [&](const ValueVec& a) -> ValuePtr {
        auto res = a[0]->opaque_rep;
        if (!res || res->kind != "mutex")
            throw RuntimeError("thread.lock: not a mutex");
        std::any_cast<MutexPtr>(res->payload)->mtx.lock();
        return Value::make_none();
    });
    add("unlock", 1, 1, [&](const ValueVec& a) -> ValuePtr {
        auto res = a[0]->opaque_rep;
        if (!res || res->kind != "mutex")
            throw RuntimeError("thread.unlock: not a mutex");
        std::any_cast<MutexPtr>(res->payload)->mtx.unlock();
        return Value::make_none();
    });
    add("trylock", 1, 1, [&](const ValueVec& a) -> ValuePtr {
        auto res = a[0]->opaque_rep;
        if (!res || res->kind != "mutex")
            throw RuntimeError("thread.trylock: not a mutex");
        bool ok = std::any_cast<MutexPtr>(res->payload)->mtx.try_lock();
        return Value::make_bool(ok);
    });

    // ==== 通道 ====
    add("channel", 0, 1, [&](const ValueVec& a) -> ValuePtr {
        size_t cap = a.empty() ? 0 : (size_t)value_to_int(a[0])->int_val;
        auto ch = std::make_shared<ChannelHandle>();
        ch->capacity = cap;
        return thread_wrap("channel", ch);
    });
    add("send", 2, 2, [&](const ValueVec& a) -> ValuePtr {
        auto res = a[0]->opaque_rep;
        if (!res || res->kind != "channel")
            throw RuntimeError("thread.send: not a channel");
        auto ch = std::any_cast<ChannelPtr>(res->payload);
        std::unique_lock<std::mutex> lk(ch->mtx);
        if (ch->closed) throw RuntimeError("thread.send: channel closed");
        if (ch->capacity > 0) {
            ch->cv.wait(lk, [&]{ return ch->queue.size() < ch->capacity || ch->closed; });
            if (ch->closed) throw RuntimeError("thread.send: channel closed");
        }
        ch->queue.push(a[1]);
        ch->cv.notify_one();
        return Value::make_none();
    });
    add("recv", 1, 1, [&](const ValueVec& a) -> ValuePtr {
        auto res = a[0]->opaque_rep;
        if (!res || res->kind != "channel")
            throw RuntimeError("thread.recv: not a channel");
        auto ch = std::any_cast<ChannelPtr>(res->payload);
        std::unique_lock<std::mutex> lk(ch->mtx);
        ch->cv.wait(lk, [&]{ return !ch->queue.empty() || ch->closed; });
        if (ch->queue.empty()) throw RuntimeError("thread.recv: channel closed and empty");
        auto val = ch->queue.front();
        ch->queue.pop();
        ch->cv.notify_one();
        return val;
    });
    add("try_recv", 1, 1, [&](const ValueVec& a) -> ValuePtr {
        auto res = a[0]->opaque_rep;
        if (!res || res->kind != "channel")
            throw RuntimeError("thread.try_recv: not a channel");
        auto ch = std::any_cast<ChannelPtr>(res->payload);
        std::lock_guard<std::mutex> lk(ch->mtx);
        if (ch->queue.empty()) return Value::make_none();
        auto val = ch->queue.front();
        ch->queue.pop();
        ch->cv.notify_one();
        return val;
    });
    add("close", 1, 1, [&](const ValueVec& a) -> ValuePtr {
        auto res = a[0]->opaque_rep;
        if (!res || res->kind != "channel")
            throw RuntimeError("thread.close: not a channel");
        auto ch = std::any_cast<ChannelPtr>(res->payload);
        {
            std::lock_guard<std::mutex> lk(ch->mtx);
            ch->closed = true;
        }
        ch->cv.notify_all();
        return Value::make_none();
    });
    add("len", 1, 1, [&](const ValueVec& a) -> ValuePtr {
        auto res = a[0]->opaque_rep;
        if (!res || res->kind != "channel")
            throw RuntimeError("thread.len: not a channel");
        auto ch = std::any_cast<ChannelPtr>(res->payload);
        std::lock_guard<std::mutex> lk(ch->mtx);
        return Value::make_int((long long)ch->queue.size());
    });

    // ==== 原子变量 ====
    add("atomic", 1, 1, [&](const ValueVec& a) -> ValuePtr {
        long long v = value_to_int(a[0])->int_val;
        auto at = std::make_shared<AtomicHandle>();
        at->value.store(v);
        return thread_wrap("atomic", at);
    });
    add("atomic_get", 1, 1, [&](const ValueVec& a) -> ValuePtr {
        auto res = a[0]->opaque_rep;
        if (!res || res->kind != "atomic")
            throw RuntimeError("thread.atomic_get: not an atomic");
        return Value::make_int(std::any_cast<AtomicPtr>(res->payload)->value.load());
    });
    add("atomic_set", 2, 2, [&](const ValueVec& a) -> ValuePtr {
        auto res = a[0]->opaque_rep;
        if (!res || res->kind != "atomic")
            throw RuntimeError("thread.atomic_set: not an atomic");
        std::any_cast<AtomicPtr>(res->payload)->value.store(value_to_int(a[1])->int_val);
        return Value::make_none();
    });
    add("atomic_add", 2, 2, [&](const ValueVec& a) -> ValuePtr {
        auto res = a[0]->opaque_rep;
        if (!res || res->kind != "atomic")
            throw RuntimeError("thread.atomic_add: not an atomic");
        long old = std::any_cast<AtomicPtr>(res->payload)->value.fetch_add(value_to_int(a[1])->int_val);
        return Value::make_int(old);
    });
    add("atomic_cas", 3, 3, [&](const ValueVec& a) -> ValuePtr {
        auto res = a[0]->opaque_rep;
        if (!res || res->kind != "atomic")
            throw RuntimeError("thread.atomic_cas: not an atomic");
        auto& at = std::any_cast<AtomicPtr>(res->payload)->value;
        long long expect = value_to_int(a[1])->int_val;
        long long newv = value_to_int(a[2])->int_val;
        bool ok = at.compare_exchange_strong(expect, newv);
        return Value::make_bool(ok);
    });

    // ==== 线程池 ====
    add("pool", 1, 1, [&](const ValueVec& a) -> ValuePtr {
        size_t n = (size_t)std::max(1LL, value_to_int(a[0])->int_val);
        auto pool = std::make_shared<ThreadPoolHandle>();
        for (size_t i = 0; i < n; ++i) {
            pool->workers.emplace_back([pool]() {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lk(pool->mtx);
                        pool->cv.wait(lk, [pool]{ return pool->stop.load() || !pool->tasks.empty(); });
                        if (pool->stop.load() && pool->tasks.empty()) return;
                        task = std::move(pool->tasks.front());
                        pool->tasks.pop();
                        pool->pending.fetch_sub(1);
                    }
                    pool->active.fetch_add(1);
                    task();
                    pool->active.fetch_sub(1);
                }
            });
        }
        return thread_wrap("pool", pool);
    });
    add("pool_submit", 2, 2, [&](const ValueVec& a) -> ValuePtr {
        auto res = a[0]->opaque_rep;
        if (!res || res->kind != "pool")
            throw RuntimeError("thread.pool_submit: not a pool");
        if (a[1]->type != ValueType::Function)
            throw RuntimeError("thread.pool_submit: expects a function");
        auto pool = std::any_cast<ThreadPoolPtr>(res->payload);
        auto fn = a[1]->fn_rep;
        extern Interpreter* g_thread_active_interpreter;
        auto interp = g_thread_active_interpreter;
        if (!interp) throw RuntimeError("thread.pool_submit: no interpreter bound");
        {
            std::lock_guard<std::mutex> lk(pool->mtx);
            pool->tasks.push([interp, fn]() {
                // 序列化对解释器共享状态的访问
                std::lock_guard<std::recursive_mutex> lk(g_interp_exec_mutex);
                try {
                    ValueVec empty;
                    if (fn->builtin_fn) {
                        Environment env(&interp->globals());
                        fn->builtin_fn(empty, env);
                    } else if (fn->def) {
                        interp->call_user_function(fn.get(), empty);
                    }
                } catch (...) {}
            });
            pool->pending.fetch_add(1);
        }
        pool->cv.notify_one();
        return Value::make_none();
    });
    add("pool_size", 1, 1, [&](const ValueVec& a) -> ValuePtr {
        auto res = a[0]->opaque_rep;
        if (!res || res->kind != "pool")
            throw RuntimeError("thread.pool_size: not a pool");
        auto pool = std::any_cast<ThreadPoolPtr>(res->payload);
        return Value::make_int((long long)(pool->active.load() + pool->pending.load()));
    });
    add("pool_shutdown", 1, 1, [&](const ValueVec& a) -> ValuePtr {
        auto res = a[0]->opaque_rep;
        if (!res || res->kind != "pool")
            throw RuntimeError("thread.pool_shutdown: not a pool");
        auto pool = std::any_cast<ThreadPoolPtr>(res->payload);
        {
            std::lock_guard<std::mutex> lk(pool->mtx);
            pool->stop.store(true);
        }
        pool->cv.notify_all();
        for (auto& w : pool->workers) {
            if (w.joinable()) w.join();
        }
        return Value::make_none();
    });

    std_modules["thread"] = mod;
}

// 解释器绑定
Interpreter* g_thread_active_interpreter = nullptr;

} // namespace vortex
