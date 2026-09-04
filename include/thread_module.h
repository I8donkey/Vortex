// ============================================================
// thread_module.h — 多线程模块（简洁设计，功能完备）
//
// API 概览（import thread）：
//   thread.run(fn)            — 启动线程执行函数，返回 Thread 句柄
//   thread.join(handle)       — 等待线程结束
//   thread.detach(handle)     — 分离线程
//   thread.sleep(ms)          — 当前线程休眠毫秒
//   thread.yield()            — 让出 CPU
//   thread.id()               — 获取当前线程 ID
//   thread.hardware()         — 获取 CPU 核心数
//
//   thread.mutex()            — 创建互斥锁
//   mutex.lock(m) / unlock(m) / trylock(m)
//
//   thread.channel(size=0)    — 创建有界/无界通道
//   channel.send(ch, value)   — 发送值
//   channel.recv(ch)          — 接收值（阻塞）
//   channel.try_recv(ch)      — 非阻塞接收
//   channel.close(ch)         — 关闭通道
//   channel.len(ch)           — 队列中元素数
//
//   thread.atomic(value)      — 创建原子变量
//   atomic.get(at)            — 读取值
//   atomic.set(at, v)         — 写入值
//   atomic.add(at, v)         — 原子加
//   atomic.cas(at, old, new)  — 比较并交换
//
//   thread.pool(size)         — 创建固定大小线程池
//   pool.submit(pool, fn)    — 提交任务
//   pool.size(pool)           — 待完成任务数
//   pool.shutdown(pool)       — 关闭线程池
// ============================================================
#ifndef VORTEX_THREAD_MODULE_H
#define VORTEX_THREAD_MODULE_H

#include "value.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <future>
#include <queue>
#include <functional>
#include <memory>

namespace vortex {

// 线程句柄
struct ThreadHandle {
    std::thread thread;
    bool joined = false;
    bool detached = false;
    // 析构时若线程仍可联接（未 join/detach），安全分离，避免 std::terminate
    ~ThreadHandle() {
        if (thread.joinable() && !joined && !detached) {
            thread.detach();
        }
    }
};
using ThreadPtr = std::shared_ptr<ThreadHandle>;

// 互斥锁句柄
struct MutexHandle {
    std::mutex mtx;
};
using MutexPtr = std::shared_ptr<MutexHandle>;

// 通道句柄
struct ChannelHandle {
    std::queue<ValuePtr> queue;
    std::mutex mtx;
    std::condition_variable cv;
    bool closed = false;
    size_t capacity = 0; // 0 = 无界
};
using ChannelPtr = std::shared_ptr<ChannelHandle>;

// 原子变量句柄
struct AtomicHandle {
    std::atomic<long long> value{0};
};
using AtomicPtr = std::shared_ptr<AtomicHandle>;

// 线程池
struct ThreadPoolHandle {
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> stop{false};
    std::atomic<int> active{0};
    std::atomic<int> pending{0};
};
using ThreadPoolPtr = std::shared_ptr<ThreadPoolHandle>;

void register_thread_module(std::unordered_map<std::string, ValuePtr>& std_modules);

} // namespace vortex

#endif // VORTEX_THREAD_MODULE_H
