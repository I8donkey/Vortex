cmake_minimum_required(VERSION 3.15)
project(newcodering LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 编译选项 - GCC/MinGW
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -O2)
endif()

# 头文件目录
include_directories(${CMAKE_SOURCE_DIR}/include)

# 源文件
set(NEWCODERING_SOURCES
    src/lexer.cpp
    src/value.cpp
    src/parser.cpp
    src/interpreter.cpp
)

# 静态库：编译器核心
add_library(newcodering_core STATIC ${NEWCODERING_SOURCES})
target_include_directories(newcodering_core PUBLIC ${CMAKE_SOURCE_DIR}/include)

# 可执行文件：编译器&解释器入口
add_executable(newcodering src/main.cpp)
target_link_libraries(newcodering PRIVATE newcodering_core)

# 若为 MinGW，静态链接标准库（避免依赖 libgcc/libstdc++ dll）
if(MINGW)
    target_link_options(newcodering PRIVATE -static -static-libgcc -static-libstdc++)
endif()
