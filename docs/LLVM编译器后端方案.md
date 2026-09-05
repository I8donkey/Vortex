# vortex 原生编译器（LLVM 后端）技术方案

> 目标：将 `.vt` 源码直接编译为原生机器码可执行文件（`/bin/ 下独立 exe`），
> 不依赖 `.vt` 源文件、不依赖解释器启动，**不使用脚本转 C++ 打包**的替代方案。
> 本文档界定技术路线、架构、范围、分期与风险，供评审后实施。

---

## 1. 背景与目标

vortex 目前是**树遍历解释器**：`Lexer → Parser → AST → Interpreter`（`src/interpreter.cpp`
对 AST 递归求值）。现有可执行程序 `vortex.exe` 是解释器入口；GUI 的"编译"按钮仅做
词法/语法检查，不产出文件。

用户要求的"编译为 exe"含义：**像 gcc 之于 C 那样，把 vortex 源码编译成原生可执行文件**。
并经确认，采用 **LLVM 写一个真正的编译器后端**（不是解释器打包，也不是转 C++）。

### 本方案范围（Phase 决策）

LLVM 完整后端工程规模极大（动/静态混合类型、闭包、引用计数、扩展模块转发等），
一次性交付不现实。本方案采用**分层可扩展**设计，**第一批先支撑核心子集**，后续按
支持矩阵不断扩面，最终趋近解释器全功能。

---

## 2. 总体架构

```
.vt 源码
   │  Lexer（复用现有 src/lexer.cpp）
   ▼
Token 流
   │  Parser（复用现有 src/parser.cpp）
   ▼
AST（复用 include/ast.h）
   │  ⭐ 新增：CodeGenerator（src/codegen/）
   ▼
LLVM IR（类型化，基于运行时 ABI）
   │  LLVM pass manager（优化）
   ▼
优化后 IR
   │  TargetMachine（x86_64 Windows/目标平台）
   ▼
目标文件 (.o/.obj)  →  链接（运行时库 + CRT）  →  原生 exe
```

- **复用**：Lexer、Parser、AST 完全复用现有实现，前端不动。
- **新增（核心）**：`src/codegen/` 下的 LLVM 代码生成器。
- **新增（配套）**：`runtime/` 一份类型化运行时库，用于承载动态值与内建操作。

---

## 3. 关键设计：类型化运行时（Type-Erased Value）

vortex 是动态类型语言（`Value` 持有 `type` + 多态负载）。原生编译无法像解释器那样
每次 `shared_ptr<Value>` 分配。方案采用**非装箱标量 + 间接装箱**混合：

### 3.1 IR 层的值类型（LLVM 侧）

| vortex 类型 | LLVM IR 表示 | 说明 |
|---|---|---|
| int / char / bool | `i64` / `i64` / `i1`（扩展 i64） | 常态寄存器/栈上，零分配 |
| float | `double` | 标量 |
| str | `VStr*`（运行时结构） | 惰性标量，需要时入栈 |
| list/dict/set/... | 句柄指针 | 指向运行时容器 |
| 泛型/未知 | `VVal { i64 tag; union }` | 任何处兜底 |

### 3.2 运行时 ABI（runtime/）

一个静态链接的小型运行时库，C 风格导出，供 IR 调用：

```
struct VObject;          // 堆对象基（引用计数头）
struct VStr   { VObject hdr; i64 len; char* data; };
struct VList  { VObject hdr; ... 链表/向量实现 };
...
VStr*  vor_str_new();
VList* vor_list_new();
void   vor_obj_retain(i64* rc);
void   vor_obj_release(...);
double vor_pow(double a,double b);
i64    vor_str_len(VStr*);
...    // math/time/random/thread/log/gui 等的转发入口
```

关键点：
- **标量（int/float/bool）走原生，零 GC 压力**；只有容器/字符串走 RC 计数。
- 运行时**可选链接**：编译时不 import 扩展模块，则对应符号不引，可裁剪体积。

### 3.3 为什么不用"整树值对象装箱"

解释器对每个节点都 `shared_ptr` 分配 + 多态析构。原生若照搬，性能不优于解释器且
背负编译复杂度。本方案**标量直通、容器装箱**，是编译语言代价最小且可逐步扩展的取舍。

---

## 4. LLVM 代码生成器设计

### 4.1 目录与模块

```
src/codegen/
  CodeGen.h / CodeGen.cpp     # 入口：Program → Module(LLVM)
  CGExpr.cpp                  # 表达式 → LLVM Value
  CGStmt.cpp                  # 语句块 → BB/phi/分支
  CGClass.cpp                 # 环境解析与作用域、闭包捕获
  CGRuntime.cpp               # 内建函数/模块 → 运行时调用
  VortexModule.cpp            # 把 Program AST 产出一个 llvm::Module
runtime/
  runtime.h / runtime.cpp     # 类型化运行时（见 §3）
```

### 4.2 编译流程（对应现有 `compile` 命令扩展）

新增 CLI 子命令与 GUI 后端调用共用同一套：

```
vortex build <file.vt> [-o out.exe] [-O2] [--target x86_64-pc-windows-msvc|mingw]
  → lex/parse 校验
  → CodeGenerator 产出 llvm::Module（函数按需消除）
  → ModulePassManager 优化（-O0..-O3）
  → TargetMachine 汇编
  → 链接 runtime + WinCRT（MinGW: -static）
  → 输出独立 exe
```

- **GUI 集成**：`MainWindow.cpp` 的 `compileCurrent()` 增加"产出 exe"路径——
  在保留词法/语法检查的同时，调用 `buildVortexExe(src, outPath)`（内部走上述管线）。
- 语言/主题设置不受影响；编译按钮行为由设置二选一或"编译 / 编译为 exe"两个按钮。

### 4.3 作用域与闭包

- 局部标量变量 → `alloca + store/load`，按词法作用域管理 `current_env` 表。
- 顶层函数 → 标准函数符号（`i64(i64,...)` 等签名）。
- 闭包（lambda 捕获）→ 用 `llvm.struct` 打包捕获变量 + 函数指针；后续阶段实现。

### 4.4 控制流

- `if / while / for`、`break/continue` 由 `br` + BB + `PHI` 实现，短路 `&& / ||` 用
  基本块链化，**保持与解释器相同的求值语义**。

---

## 5. 首批支持矩阵（Phase 1）

| 类别 | 支持 | 备注 |
|---|---|---|
| 类型 | int / float / bool / str | 容器后续 |
| 字面量 | 整数/浮点/布尔/字符串 | |
| 运算 | `+ - * / // % **`、比较、`&& \|\| !` | 局部变量 |
| 语句 | 变量声名、赋值、`if/elif/else`、`while`、`for`、`def`、`return` | |
| 函数 | 用户 `def`、递归、简单调用 | 数组/命名参数 |
| 输出 | `print`、顶层表达式副作用 | |
| 模块 | `math`（初值集合） | 后续 time/random/thread/log/gui |
| 优化 | `-O0/-O1/-O2/-O3` | LLVM pass manager |

**暂不（Phase 1+）**：list/dict/set/pair/tuple 容器、闭包捕获、`@`/`~` 引用、try/catch、
import 扩展模块的多数、内存地址语义。这些列作 Phase 2/3。

---

## 6. 分期路线图

| 阶段 | 内容 | 交付 |
|---|---|---|
| **P0 环境** | 获取 LLVM（见 §7）、搭建 `codegen` 骨架、CMake 集成、`build` 命令框架 | 空工程可跑 |
| **P1 核心子集** | 标量类型 + 控制流 + 函数 + print + math 初值 | 简单 `.vt` 编译成可运行 exe |
| **P2 容器** | list/set/dict/pair/tuple + 下标/迭代 | 容器程序可编译 |
| **P3 高级语义** | 闭包/捕获、引用 `@`/`~`、try/catch、debug 信息 | 接近解释器 |
| **P4 模块全转发** | time/random/thread/log/gui 运行时桥接 | 模块化程序可编译 |
| **P5 GUI 集成** | "编译为 exe"按钮落地、错误回显、产物路径 | 全流程可用 |

每阶段验收：同一份源码，解释器运行结果与原生 exe 运行结果一致。

---

## 7. 工具链与 LLVM 获取（无管理员权限）

- **系统编译器**：已检测到 `g++ 16.1.0 (MinGW)` at `D:\gcc\bin\g++.exe`（用于链接 CRT 与运行时）。
- **LLVM 库**：当前未安装。无管理员权限下，采用**用户目录内预编译包**：
  - 下载 LLVM **Pre-built binaries**（Windows，x64，静态库版 `llvm-x.y.z-win64-mingw`）到
    `~/.local/vortex/llvm`；
  - 通过 `cmake -DLLVM_DIR=<path>\lib\cmake\llvm` 或 `-DLLVM_INCLUDE_DIRS/-DLLVM_LIBRARY_DIRS`
    集成，**无需系统级安装**。
  - 备选：`vcpkg install llvm`（install 至用户目录，不需要管理员）。
- 交叉影响：`gui_module`（Qt）仍独立依赖 Qt；编译器后端本体**不依赖 Qt**，仅 GUI 按钮触发它。

> 落地时先在 CI/本机验证 `llvm-config --libdir` 与静态库可用，避免版本 ABI 不匹配。

---

## 8. 风险与代价（诚实说明）

| 风险 | 说明 | 缓解 |
|---|---|---|
| **LLVM 初版依赖成本** | 需要下载/集成 LLVM 库 | 用户目录预编译，P0 单独验证 |
| **动态类型编译难度** | 动态 `Value`、混合装箱、goBN 引用计数需精细设计 | 标量直通 + 容器装箱 + 运行时 ABI |
| **扩展模块桥接量** | game2d/render3d/gui/thread 大量内建函数需转发 | P4 阶段、可选链接、按需裁减 |
| **行为一致性** | 原生与解释器语义必须一致 | 每阶段 golden 对比测试 |
| **工期规模** | 完整后端为跨月工程 | 分期交付、P1 先行验证价值 |

---

## 9. 结论与建议

采用 **LLVM 真后端子集方案**，从 **P0+P1（核心标量子集可编译成 exe）** 起步，
用最小可行验证打通管道，再按矩阵扩展。本方案"标量直通 + 容器装箱 + 可选运行时"
既保持 vortex 动态语义，又把编译出原生 exe 的代价控制在可推进范围。

评审确认后，从 **P0 环境搭建** 开始实施。