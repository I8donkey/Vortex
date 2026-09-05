// ============================================================
// net_module.h — 网络模块
// url_encode / url_decode（纯函数）+ http_get / http_post
// ============================================================
#ifndef VORTEX_NET_MODULE_H
#define VORTEX_NET_MODULE_H

#include <unordered_map>
#include <string>
#include <memory>

namespace vortex {
struct Value; using ValuePtr = std::shared_ptr<Value>;

void register_net_module(std::unordered_map<std::string, ValuePtr>& std_modules);

} // namespace vortex
#endif // VORTEX_NET_MODULE_H