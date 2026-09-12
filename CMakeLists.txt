cmake_minimum_required(VERSION 3.15)
project(vortex LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ==================== 依赖管理：CPM.cmake ====================
# 用 CPM 自动拉取可下载的小型第三方依赖（沿用 CPM 惯例，不含 Qt）。
# 若无法联网下载，则自动回退到系统 find_package / 本地预编译依赖。
option(VORTEX_USE_CPM "Fetch small optional dependencies automatically via CPM" ON)
if(VORTEX_USE_CPM)
    set(_CPM_VER "v0.40.8")
    # 完整 CPM.cmake 的最小可信大小（字节）；小于它即视为被截断/损坏，直接离线回退。
    set(_CPM_MIN_SIZE 40000)

    # 校验某文件是否是一份“可信完整”的 CPM.cmake；结果写回 _CPM_VALID(1/0)
    function(_cpm_probe _file)
        set(__valid 0)
        if(EXISTS "${_file}")
            file(SIZE "${_file}" __sz)
            if(__sz GREATER ${_CPM_MIN_SIZE})
                file(STRINGS "${_file}" __has LIMIT_COUNT 1 REGEX "CPMAddPackage")
                if(__has)
                    set(__valid 1)
                endif()
            endif()
        endif()
        set(_CPM_VALID ${__valid} PARENT_SCOPE)
    endfunction()

    set(_CPM_OK 0)
    set(_CPM_DL "")

    # 1) 优先用仓库内置的完整副本（离线 / 无外网也可用）
    set(_CPM_BUILTIN "${CMAKE_SOURCE_DIR}/cmake/CPM.cmake")
    if(EXISTS "${_CPM_BUILTIN}")
        _cpm_probe("${_CPM_BUILTIN}")
        if(_CPM_VALID)
            set(_CPM_DL "${_CPM_BUILTIN}")
            set(_CPM_OK 1)
        endif()
    endif()

    # 2) 否则从多个源依次下载，且每次均严格校验；全损坏则离线
    if(NOT _CPM_OK)
        set(_CPM_DL "${CMAKE_BINARY_DIR}/cmake/CPM.cmake")
        foreach(_url
            "https://github.com/cpm-cmake/CPM.cmake/releases/download/${_CPM_VER}/CPM.cmake"
            "https://raw.githubusercontent.com/cpm-cmake/CPM.cmake/v0.40.8/cmake/CPM.cmake"
            "https://gitlab.com/cpm-cmake/CPM.cmake/-/raw/v0.40.8/cmake/CPM.cmake")
            file(REMOVE "${_CPM_DL}")
            file(DOWNLOAD "${_url}" "${_CPM_DL}" STATUS _st LOG _lg)
            _cpm_probe("${_CPM_DL}")
            if(_CPM_VALID)
                set(_CPM_OK 1)
                break()
            endif()
        endforeach()
        if(NOT _CPM_OK)
            file(REMOVE "${_CPM_DL}")    # 残留损坏文件，丢弃，绝不被 include
        endif()
    endif()

    if(_CPM_OK)
        set(CPM_SOURCE_CACHE "${CMAKE_SOURCE_DIR}/.vortex-deps/cpm")
        set(CPM_DOWNLOAD_LOCATION "${_CPM_DL}")
        include("${_CPM_DL}")
        message(STATUS "Vortex CPM: dependency manager ready (${_CPM_DL})")
        # 由 CPM 自动获取的小型依赖：zlib（zip/file 模块需要用）
        # 拉取失败会在 CPM 内报错；如需彻底离线可改 -DVORTEX_USE_CPM=OFF 走系统 ZLIB。
        CPMAddPackage(NAME zlib VERSION 1.3.1 GITHUB_REPOSITORY madler/zlib)
    else()
        message(STATUS "Vortex CPM: unavailable (offline/损坏), falling back to system/installed deps")
    endif()
endif()

# 输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 编译选项 - GCC/MinGW
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-unused-variable
                        -Wno-unused-but-set-variable -Wno-missing-field-initializers
                        -O2 -static-libgcc -static-libstdc++)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wno-unused-parameter -Wno-unused-variable -O2)
endif()
if(MSVC)
    add_compile_options(/wd4100 /wd4189 /wd4201 /wd4244 /wd4267 /wd4458 /wd4838 /O2)
endif()

# 头文件目录
include_directories(${CMAKE_SOURCE_DIR}/include)

# ============= 扩展模块开关（默认 ON，即使依赖缺失仍可编译为 fallback） =============
option(VORTEX_WITH_GAME2D  "Build 2D game module (real window requires SDL2)"          ON)
option(VORTEX_WITH_RENDER3D "Build 3D rendering module (GPU path requires OpenGL/GLEW)" ON)

# 尝试查找真实 GPU / 多媒体 SDK（找到则启用真实路径宏，否则继续 fallback 编译）
set(VORTEX_HAS_SDL2 OFF)
set(VORTEX_HAS_GL OFF)

if(VORTEX_WITH_GAME2D)
    # 1. 先单独查找 SDL2，使用 CONFIG 模式和组件
    find_package(SDL2 CONFIG QUIET COMPONENTS SDL2)

    if(SDL2_FOUND)
        set(VORTEX_HAS_SDL2 ON)
        message(STATUS "Vortex Game2D: SDL2 found, real window backend enabled")

        # 2. SDL2 找到后，再查找扩展库
        find_package(SDL2_image CONFIG QUIET)
        find_package(SDL2_mixer CONFIG QUIET)
        find_package(SDL2_ttf   CONFIG QUIET)

        # 3. 为每个扩展库单独设置标志，便于后续按需启用功能
        if(SDL2_image_FOUND)
            set(VORTEX_HAS_SDL2_IMAGE ON)
            message(STATUS "  - SDL2_image found")
        else()
            message(STATUS "  - SDL2_image not found, image loading disabled")
        endif()

        if(SDL2_mixer_FOUND)
            set(VORTEX_HAS_SDL2_MIXER ON)
            message(STATUS "  - SDL2_mixer found")
        else()
            message(STATUS "  - SDL2_mixer not found, audio disabled")
        endif()

        if(SDL2_ttf_FOUND)
            set(VORTEX_HAS_SDL2_TTF ON)
            message(STATUS "  - SDL2_ttf found")
        else()
            message(STATUS "  - SDL2_ttf not found, font rendering disabled")
        endif()
    else()
        message(STATUS "Vortex Game2D: SDL2 not found, using headless software-rendered fallback")
        # 可选：如果用户显式要求 Game2D，找不到 SDL2 时直接报错而不是静默降级
        # message(FATAL_ERROR "VORTEX_WITH_GAME2D is ON but SDL2 was not found.")
    endif()
endif()

if(VORTEX_WITH_RENDER3D)
    # 尝试 OpenGL / GLEW
    find_package(OpenGL QUIET)
    find_package(GLEW QUIET)
    if(OpenGL_FOUND AND GLEW_FOUND)
        set(VORTEX_HAS_GL ON)
        message(STATUS "Vortex Render3D: OpenGL+GLEW found, GPU pipeline enabled")
    else()
        message(STATUS "Vortex Render3D: OpenGL/GLEW not fully available, using CPU rasterizer fallback")
    endif()
endif()

# 源文件
set(VORTEX_SOURCES
    src/lexer.cpp
    src/value.cpp
    src/parser.cpp
    src/interpreter.cpp
    src/vortex_modules.cpp
)

# 扩展模块源文件
if(VORTEX_WITH_GAME2D)
    list(APPEND VORTEX_SOURCES src/game2d_module.cpp)
endif()
if(VORTEX_WITH_RENDER3D)
    list(APPEND VORTEX_SOURCES src/render3d_module.cpp)
endif()
# 新模块：thread / log / gui（始终编译，gui 需要 Qt）
list(APPEND VORTEX_SOURCES src/thread_module.cpp src/log_module.cpp src/gui_module.cpp src/file_module.cpp src/zip_module.cpp src/xml_module.cpp src/html_module.cpp src/sql_module.cpp src/os_module.cpp src/regex_module.cpp src/json_module.cpp src/base64_module.cpp src/datetime_module.cpp src/csv_module.cpp src/hash_module.cpp src/str_module.cpp src/net_module.cpp src/sys_module.cpp)

# 静态库：编译器核心
add_library(vortex_core STATIC ${VORTEX_SOURCES})
target_include_directories(vortex_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_compile_features(vortex_core PUBLIC cxx_std_17)

# zip / file 模块使用 zlib：优先用 CPM 提供的 zlib 目标，否则回退系统 ZLIB / -lz
find_package(ZLIB QUIET)
if(TARGET zlib)
    target_link_libraries(vortex_core PUBLIC zlib)
elseif(ZLIB_FOUND)
    target_link_libraries(vortex_core PUBLIC ZLIB::ZLIB)
else()
    target_link_libraries(vortex_core PUBLIC z)
endif()

# sql 模块使用 sqlite3（MinGW opt 自带头/库）
set(_sqlite_lib ${CMAKE_SOURCE_DIR}/.vortex-deps/sqlite)
if(EXISTS "D:/gcc/opt/include/sqlite3.h")
    target_include_directories(vortex_core PRIVATE "D:/gcc/opt/include")
    target_link_directories(vortex_core PRIVATE "D:/gcc/opt/lib")
    target_link_libraries(vortex_core PRIVATE sqlite3)
    set(VORTEX_SQLITE_INCLUDE "D:/gcc/opt/include")
    set(VORTEX_SQLITE_LIB "D:/gcc/opt/lib")
endif()

# net 模块使用 WinHTTP（仅 Windows）
if(WIN32)
    target_link_libraries(vortex_core PUBLIC winhttp)
endif()

# 扩展功能宏：真实 SDK 存在时开启
if(VORTEX_HAS_SDL2)
    target_compile_definitions(vortex_core PUBLIC VORTEX_WITH_SDL2_REAL)
    target_link_libraries(vortex_core PUBLIC SDL2::SDL2)
    if(TARGET SDL2_image::SDL2_image)
        target_link_libraries(vortex_core PUBLIC SDL2_image::SDL2_image)
    endif()
    if(TARGET SDL2_mixer::SDL2_mixer)
        target_link_libraries(vortex_core PUBLIC SDL2_mixer::SDL2_mixer)
    endif()
    if(TARGET SDL2_ttf::SDL2_ttf)
        target_link_libraries(vortex_core PUBLIC SDL2_ttf::SDL2_ttf)
    endif()
endif()
if(VORTEX_HAS_GL)
    target_compile_definitions(vortex_core PUBLIC VORTEX_WITH_OPENGL_REAL)
    target_link_libraries(vortex_core PUBLIC OpenGL::GL GLEW::GLEW)
endif()

# 给 MinGW 下的多线程链接支持（game2d / render3d / thread 模块用到 std::thread 等）
if(MINGW)
    find_package(Threads REQUIRED)
    target_link_libraries(vortex_core PUBLIC Threads::Threads)
    # posix 线程模型下 std::thread 依赖 winpthread，显式链接以确保符号可用
    target_link_libraries(vortex_core PUBLIC winpthread)
endif()

# 可执行文件：编译器&解释器入口
add_executable(vortex src/main.cpp)
target_link_libraries(vortex PRIVATE vortex_core)
if(EXISTS "D:/gcc/opt/include/sqlite3.h")
    target_link_directories(vortex PRIVATE "D:/gcc/opt/lib")
endif()

# 若为 MinGW，静态链接标准库（避免依赖 libgcc/libstdc++ dll）
if(MINGW)
    target_link_options(vortex PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# ============ 原生编译器运行时（引用计数，C ABI） ============
# vortexcc 链路生成的可执行文件会链接此库。
add_library(vortex_runtime STATIC runtime/runtime.cpp
    runtime/g2d_rt.cpp runtime/r3d_rt.cpp runtime/r3d_g2d.h)
target_include_directories(vortex_runtime PUBLIC ${CMAKE_SOURCE_DIR}/runtime)
if(MSVC)
    target_compile_options(vortex_runtime PRIVATE /wd4201)
endif()
if(TARGET zlib)
    target_link_libraries(vortex_runtime PUBLIC zlib)
elseif(ZLIB_FOUND)
    target_link_libraries(vortex_runtime PUBLIC ZLIB::ZLIB)
else()
    target_link_libraries(vortex_runtime PUBLIC z)
endif()
if(EXISTS "D:/gcc/opt/include/sqlite3.h" AND EXISTS "${VORTEX_SQLITE_INCLUDE}")
    target_include_directories(vortex_runtime PRIVATE "D:/gcc/opt/include")
    target_link_directories(vortex_runtime PUBLIC "D:/gcc/opt/lib")
    target_link_libraries(vortex_runtime PUBLIC sqlite3)
endif()
if(WIN32)
    target_link_libraries(vortex_runtime PUBLIC winhttp)
endif()

# ==================== Qt 编辑器 ====================
option(VORTEX_BUILD_EDITOR "Build the Qt-based Vortex code editor" ON)

if(VORTEX_BUILD_EDITOR)
    # 查找 Qt5/Qt6 Widgets 模块
    find_package(QT NAMES Qt6 Qt5 COMPONENTS Widgets)
    find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Widgets)

    if(Qt${QT_VERSION_MAJOR}Widgets_FOUND)
        message(STATUS "Vortex Editor: Found Qt${QT_VERSION_MAJOR} Widgets, building editor...")

        # gui_module.cpp 依赖 Qt Widgets，需让 vortex_core 链接 Qt
        target_link_libraries(vortex_core PUBLIC Qt${QT_VERSION_MAJOR}::Widgets)

        set(CMAKE_AUTOMOC ON)
        set(CMAKE_AUTORCC ON)
        set(CMAKE_AUTOUIC ON)

        set(VORTEX_EDITOR_SOURCES
            editor/editor_main.cpp
            editor/MainWindow.cpp
            editor/CodeEditor.cpp
            editor/SyntaxHighlighter.cpp
        )
        set(VORTEX_EDITOR_HEADERS
            editor/MainWindow.h
            editor/CodeEditor.h
            editor/SyntaxHighlighter.h
        )

        add_executable(vortex_editor
            ${VORTEX_EDITOR_SOURCES}
            ${VORTEX_EDITOR_HEADERS}
        )
        target_include_directories(vortex_editor PRIVATE ${CMAKE_SOURCE_DIR}/editor)
        target_link_libraries(vortex_editor PRIVATE
            vortex_core
            Qt${QT_VERSION_MAJOR}::Widgets
        )
        if(EXISTS "D:/gcc/opt/include/sqlite3.h")
            target_link_directories(vortex_editor PRIVATE "D:/gcc/opt/lib")
        endif()
        if(MINGW)
            # GUI 子系统：不弹控制台黑窗口。用 -mwindows 而非 WIN32 属性，
            # 避免 Qt6 自动插入的 EntryPoint(WinMain) 与 mingw32 产生 __argc 顺序冲突；
            # MinGW 下 -mwindows 保留 main() 入口即可。
            target_link_options(vortex_editor PRIVATE -static -static-libgcc -static-libstdc++ -mwindows)
        endif()
    else()
        message(STATUS "Vortex Editor: Qt Widgets NOT found, skipping editor build.")
        message(STATUS "  Install Qt5 or Qt6 with Widgets module and set CMAKE_PREFIX_PATH to its location.")
    endif()
endif()

# ==================== 原生编译器后端（LLVM） ====================
# vortexcc：把 .vt 编译为原生 exe。需 LLVM 库可用。
option(VORTEX_WITH_LLVM "Build the LLVM-based native compiler backend (vortexcc)" OFF)

if(VORTEX_WITH_LLVM)
    if(NOT DEFINED LLVM_DIR)
        set(LLVM_DIR ${CMAKE_SOURCE_DIR}/.vortex-deps/llvm-build/lib/cmake/llvm)
    endif()
    find_package(LLVM CONFIG PATHS ${LLVM_DIR} NO_DEFAULT_PATH QUIET)
    if(NOT LLVM_FOUND)
        find_package(LLVM CONFIG QUIET)
    endif()
    if(LLVM_FOUND)
        message(STATUS "Vortex LLVM: found LLVM ${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR} at ${LLVM_DIR}")
        set(VORTEX_LLVM_SRC_INCLUDE ${CMAKE_SOURCE_DIR}/.vortex-deps/llvm-src/llvm-project-23.1.0.src/llvm/include)
        set(VORTEX_LLVM_BUILD_INCLUDE ${CMAKE_SOURCE_DIR}/.vortex-deps/llvm-build/include)
        add_executable(vortexcc
            src/cc/main.cpp
            src/codegen/CodeGen.cpp
        )
        target_include_directories(vortexcc PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${VORTEX_LLVM_SRC_INCLUDE}
            ${VORTEX_LLVM_BUILD_INCLUDE}
        )
        # vortexcc 编译产物链接 vortex_runtime 库时使用其绝对路径
        target_compile_definitions(vortexcc PRIVATE VORTEX_RT_LIB="$<TARGET_FILE:vortex_runtime>")

        # ======= LLD：让 vortexcc 生成的 exe 不用外部 g++，改用 LLVM 的 lld 链接 =======
        option(VORTEX_USE_LLD "Use CPM to build LLD, so generated exes link via lld instead of external g++" ON)
        if(VORTEX_USE_LLD AND _CPM_OK AND NOT TARGET lld)
            # 复用本项目已编译的 LLVM 构建（LLVM_DIR），standalone 构建 lld；
            # 需要能访问网络（GIT_TAG 对应 llvm 23.1）。
            # 注意: CPMAddPackage 失败(如离线)会让配置报错; 彻底离线请 -DVORTEX_USE_LLD=OFF。
            CPMAddPackage(NAME lld
                          VERSION 23.1.0
                          GITHUB_REPOSITORY llvm/lld
                          GIT_TAG llvmorg-23.1.0
                          DOWNLOAD_ONLY NO
                          OPTIONS
                            "LLVM_DIR ${LLVM_DIR}"
                            "LLVM_INCLUDE_TESTS OFF"
                            "LLVM_INCLUDE_BENCHMARKS OFF"
                            "LLVM_INCLUDE_EXAMPLES OFF"
                            "LLVM_INCLUDE_DOCS OFF"
                            "LLVM_ENABLE_TERMINFO OFF"
                            "LLVM_ENABLE_ZLIB OFF")
        endif()
        if(TARGET lld)
            # 向 vortexcc 注入 lld 可执行文件路径；CodeGen 据此切换链接命令（不再调 g++）
            target_compile_definitions(vortexcc PRIVATE VORTEX_LLD="$<TARGET_FILE:lld>")
            message(STATUS "Vortex LLD: will link generated exes with LLD (no external g++)")
        else()
            message(STATUS "Vortex LLD: LLD unavailable (offline or not built), falling back to g++ for linking generated exes")
        endif()
        # 静态 LLVM 组件库存在循环依赖，用 --start-group/--end-group 包裹以整体解析
        file(GLOB VORTEX_LLVM_ARCHIVES "${CMAKE_SOURCE_DIR}/.vortex-deps/llvm-build/lib/libLLVM*.a")
        list(PREPEND VORTEX_LLVM_ARCHIVES "-Wl,--start-group")
        list(APPEND  VORTEX_LLVM_ARCHIVES "-Wl,--end-group")
        target_link_libraries(vortexcc PRIVATE
            vortex_core
            vortex_runtime
            ${VORTEX_LLVM_ARCHIVES}
            -lntdll -lpsapi -lshlwapi -lversion -lshell32 -lole32 -luuid -lws2_32
        )
        if(MINGW)
            target_link_options(vortexcc PRIVATE -static -static-libgcc -static-libstdc++)
        endif()
    else()
        message(STATUS "Vortex LLVM: LLVM NOT found. Set LLVM_DIR to the llvm cmake dir to enable vortexcc.")
    endif()
endif()
