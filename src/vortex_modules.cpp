// ============================================================
// vortex_modules.cpp — 扩展模块统一入口
// ============================================================
#include "vortex_modules.h"
#include "game2d_module.h"
#include "render3d_module.h"
#include "thread_module.h"
#include "log_module.h"
#include "gui_module.h"
#include "file_module.h"
#include "zip_module.h"
#include "xml_module.h"
#include "html_module.h"
#include "sql_module.h"
#include "os_module.h"
#include "regex_module.h"
#include "json_module.h"
#include "base64_module.h"
#include "datetime_module.h"
#include "csv_module.h"
#include "hash_module.h"
#include "str_module.h"
#include "net_module.h"
#include "sys_module.h"

namespace vortex {

void register_extension_modules(std::unordered_map<std::string, ValuePtr>& std_modules) {
    // 按依赖自底向上注册
    register_game2d_module(std_modules);
    register_render3d_module(std_modules);
    register_thread_module(std_modules);
    register_log_module(std_modules);
    register_gui_module(std_modules);
    register_file_module(std_modules);
    register_zip_module(std_modules);
    register_xml_module(std_modules);
    register_html_module(std_modules);
    register_sql_module(std_modules);
    register_os_module(std_modules);
    register_regex_module(std_modules);
    register_json_module(std_modules);
    register_base64_module(std_modules);
    register_datetime_module(std_modules);
    register_csv_module(std_modules);
    register_hash_module(std_modules);
    register_str_module(std_modules);
    register_net_module(std_modules);
    register_sys_module(std_modules);
}

} // namespace vortex
