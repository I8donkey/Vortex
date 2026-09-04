// ============================================================
// log_module.h — 日志模块（参考 Python logging）
//
// API（import log）：
//   log.debug(msg) / log.info(msg) / log.warn(msg) / log.error(msg) / log.fatal(msg)
//   log.level("DEBUG")              — 设置全局日志级别
//   log.format(fmt)                — 设置格式字符串
//       可用占位符: %(time) %(level) %(name) %(message)
//   log.file(path)                 — 添加文件输出目标
//   log.console(on)                — 开关控制台输出
//   log.get_level()                — 获取当前级别
//
// 级别（从低到高）：DEBUG=10 INFO=20 WARN=30 ERROR=40 FATAL=50
// ============================================================
#ifndef VORTEX_LOG_MODULE_H
#define VORTEX_LOG_MODULE_H

#include "value.h"
#include <string>
#include <fstream>
#include <memory>
#include <mutex>

namespace vortex {

struct LoggerState {
    int min_level = 20; // INFO
    std::string format = "[%(level)] %(message)";
    bool console_on = true;
    std::vector<std::string> file_paths;
    std::vector<std::shared_ptr<std::ofstream>> file_streams;
    std::mutex mtx;
};

void register_log_module(std::unordered_map<std::string, ValuePtr>& std_modules);

} // namespace vortex

#endif // VORTEX_LOG_MODULE_H
