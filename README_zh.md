# Vortex

> English version: [English](README.md)

Vortex 是一门借鉴 Python 的脚本语言，提供三种互通使用方式：**树遍历解释器**、**LLVM 原生编译器 `vortexcc`**（将 `.vt` 源码直接编译为独立机器码可执行文件）、以及整合两者的 **Qt 编辑器**。所有语言特性在「解释器」与「编译器」**两端均已实现**，并通过 golden 回归测试保证两种后端语义完全一致。

Vortex 自带完整脚本能力：动态标量、容器、支持闭包的函数、引用语义、异常处理、标准模块（`math`、`time`、`random`、`thread`、`log`、`game2d`、`render3d`）以及软件渲染的 2D/3D 图形。

---

## 目录

- [语法参考](#语法参考)（完整语法，参考 Python 组织）
- [标准库模块](#标准库模块)
- [安装](#安装)
- [快速开始](#快速开始)
- [原生编译器 vortexcc](#原生编译器-vortexcc)
- [完整使用指南](#完整使用指南)

---

## 语法参考

> 打勾的行表示支持。**解释器** = interpreter，**编译器** = `vortexcc`（LLVM 后端）。下列全部语法元素在两种后端中均已实现，并通过 golden 回归测试验证。

### 注释

```vt
# 行注释（# 之后到行尾被忽略）
```

- **解释器** ✓　**编译器** ✓

### 字面量

- 整数：十进制 `42`、二进制 `0b1010`、八进制 `0o17`、十六进制 `0x2A`
- 浮点数：`3.14`、`1e3`、`-2.5`
- 字符串：双引号 `"hi"` 或单引号 `'hi'`；转义 `\n \t \r \\ \" \' \0 \xHH`
- 布尔：`true`、`false`
- 空值：`None`

```vt
int d = 42;
int h = 0x2A
float f = 1e3;
str s = "line\n\t"
str t = '单引号'
bool flag = true
object nil = None
```

- **解释器** ✓　**编译器** ✓

> 说明：Vortex 在声明时使用显式类型（`int x = 42;`），区别于 Python 的隐式绑定。见[变量](#变量)。

### 类型

标量类型：`int`、`uint`、`short`、`ushort`、`long`、`ulong`、`float`、`double`、`char`、`unichar`、`memadr`、`bool`、`str`、`unistr`、`bin`。

容器类型：`list`、`stack`、`queue`、`set`、`undset`、`dict`、`pair`、`tuple`。

特殊类型：`object`（任意）、`function`（可调用对象）、`None`。

- **解释器** ✓　**编译器** ✓

### 变量

显式类型 + 可选初值；`const` 声明常量；`global` 声明模块级；`del` 删除绑定。

```vt
int x = 1;            # 类型声明 + 初值
float f;              # 仅声明，不初始化
str s = "hi";
list l = [1, 2, 3];
pair p = pair(1, "a");

const double PI = 3.14159;   # 常量
global counter = 0;          # 模块级

del x;                # 删除绑定
```

复合赋值与 Python 一致：`x += 1`、`x -= 2`、`x *= 3`、`x /= 4`、`x //= 2`、`x %= 3`、`x &= m`、`x |= m`、`x ^= m`、`x <<= n`、`x >>= n`。

- **解释器** ✓　**编译器** ✓

### 运算符（优先级从低到高）

| 优先级 | 运算符 | 含义 |
|---|---|---|
| 1 | `c ? a : b` | 三目运算 |
| 2 | `\|\|` | 逻辑或 |
| 3 | `&&` | 逻辑与 |
| 4 | `\|` | 按位或 |
| 5 | `^` | 按位异或 |
| 6 | `&` | 按位与 |
| 7 | `==` `!=` | 相等判断 |
| 8 | `<` `>` `<=` `>=` | 关系比较 |
| 9 | `<<` `>>` | 移位 |
| 10 | `+` `-` | 加减法 |
| 11 | `*` `/` `//` `%` | 乘除取模（`//` 整除） |
| 12 | `**` | 幂运算（右结合） |
| 13 | 一元 `!  -  +  ~  @` | 取反 / 取负 / 正 / 解引用与取地址（`not` 等价 `!`，`~` 取值、`@` 槽地址） |
| 14 | `.` `[]` `()` | 成员访问 / 下标 / 调用（后缀） |

关键字 `not`、`and`、`or` 同时等价于 `!`、`&&`、`||`。

```vt
int a = (1 + 2) * 3 ** 2;   # 27
bool b = a > 20 && a < 30;  # true
int c = 7 // 2;             # 3
```

- **解释器** ✓　**编译器** ✓

### 控制流

```vt
if (x > 0) {
} elif (x < 0) {
} else {
}

for (item in items) {
    break;
    continue;
}

while (条件) {
    break;
}

return;      # 或 return 值;
```

- **解释器** ✓　**编译器** ✓

### 函数、Lambda 与闭包

`def` 定义具名函数；`lambda` 创建匿名函数。闭包按**引用**实时捕获外层变量（后续修改会即时反映）。参数可带默认值，支持可变参数尾巴。

```vt
def add(a, b) {
    return a + b;
}
print(add(2, 3));

def greet(name, greeting = "Hello") {
    print(greeting + ", " + name);
}
greet("World");

object inc = lambda(x) { return x + 1; };   # 匿名闭包
print(inc(9));

int base = 40;
object bump = lambda(x) { return x + base; };
base = 100;
print(bump(2));          # 102 —— 按引用实时捕获
```

调用时可用命名参数：`name = value`。

- **解释器** ✓　**编译器** ✓

### 容器与下标访问

```vt
list a = [10, 20, 30];
print(a[0]);
a[1] = 99;

dict d = {"k": 1, "n": 2};        # 或 [(k, v), ...]
print(d["k"]);

set s = set([1, 2, 3]);
pair p = pair(1, "one");
tuple t = tuple(...);
```

另有 `stack`、`queue`、`undset`。`a.b` 进行成员访问。

- **解释器** ✓　**编译器** ✓

### 字符串操作

```vt
str s = "hello world";
int n = s.length;              # 11
str t = "a" + "b";             # 字符串拼接
bool has = s.contains("world");
```

- **解释器** ✓　**编译器** ✓

### 引用语义（`@` / `~`）

`@x` 取具名变量的地址（存储槽）；`~p` 解引用（读或写），支持标量与字符串。

```vt
int x = 5;
~@x = 10;      # 通过槽写入
print(~@x);    # 10
int y = x;
~@y += 1;      # 通过引用自增
```

- **解释器** ✓　**编译器** ✓

### 异常（`try` / `catch` / `finally`）

```vt
try {
    int v = int("bad");       # 内建转换可能抛异常
} catch(e) {
    print("caught: " + e);    # e 为异常消息
} finally {
    print("finally ok");
}
```

`throw` 不是面向用户的关键字；库操作（如 `int("bad")`）内部抛异常，用 `try/catch` 捕获。

- **解释器** ✓　**编译器** ✓

### import / 模块

```vt
import math;
import time as t;
from random import seed;
```

更多见[标准库模块](#标准库模块)。

- **解释器** ✓　**编译器** ✓

### 内建函数

- `print(...)` —— 输出一个或多个值，以空格分隔。
- 类型转换：`int()`、`long()`、`float()`、`double()`、`str()`、`bool()`、`uint()`、`ulong()`、`char()`。
- `range(start, stop, step)` —— 数值区间迭代。

### 并发

`thread` 模块提供线程创建/join、互斥锁、原子量、channel、线程池。函数签名见使用文档。

- **解释器** ✓　**编译器** ✓

### 与 Python 的差异

- Vortex **类型内嵌**：声明带显式类型（`int x = 1;`），不同于 Python 隐式类型。
- 代码块使用 `{ }`，语句以 `;` 结束，不用缩进。
- 逻辑运算符为 `&&`/`||`/`!`（Python 的 `and`/`or`/`not` 也等价接受）。
- `#` 注释、`for`/`while`/`if`/`def`/`lambda` 与 Python 一一对应。

---

## 标准库模块

| 模块 | 用途 |
|---|---|
| `math` | 常量 `pi` `e` `tau` `inf` `nan`；`sqrt cbrt pow exp log log2 log10 sin cos tan asin acos atan atan2 sinh cosh tanh floor ceil round fmod gcd lcm isinf isnan hypot` |
| `time` | `time sleep gmtime localtime mktime strftime strptime counter reset_counter process_time` |
| `random` | `seed getstate setstate random uniform randint randrange choice choices shuffle sample gauss expovariate triangular` |
| `thread` | 线程、互斥锁、原子量、channel、线程池 |
| `log` | 日志 |
| `file` | 读写/追加/存在/删除/改名/大小/目录判断/增删/listdir |
| `zip` | add/extract/count/has/names（zlib） |
| `xml` | escape/unescape/parse_text |
| `html` | escape/unescape/strip_tags |
| `sql` | open/close/execute/table_exists/query/query_one（SQLite） |
| `os` | getenv/hasenv/setenv/unsetenv/cwd/chdir/pid/platform/home/tempdir/path_join |
| `regex` | valid/match/search/find/find_all/replace/count |
| `json` | valid/parse_str/parse_int/parse_float/parse_bool/stringify_* |
| `base64` | encode/decode |
| `datetime` | ymd/to_iso/from_iso/today/add_days/days_between |
| `csv` | to_line/count_fields/field_at/quote/parse_line（转义） |
| `hash` | md5/sha1/sha256（十六进制摘要） |
| `text` | 大小写/裁剪/查找/替换/format 插值/字符串工具 |
| `net` | url 编解码/http_get/http_post（WinHTTP） |
| `sys` | version/platform/time_ms/clock/sleep/exit |
| `gui` | 基于 Qt 的窗口/控件/布局/事件/msgbox |
| `game2d` | 2D 图像、精灵、碰撞与绘制（软件渲染） |
| `render3d` | 软件光栅化：场景、相机、网格、材质、光源、`render` |

---

## 安装

前置依赖：

- C++17 工具链（本项目使用 MinGW）。
- 原生编译器后端所需的 LLVM 库（>= 14），通过 `LLVM_DIR` 发现。
- **可选**：LLD（经 CPM 自动拉取，用于让 `vortexcc` 免外部 g++ 链接）；LLDB（编辑器调试功能，从 PATH 或 `VORTEX_DEBUGGER` 获取）。
- **可选**：Qt5/Qt6 Widgets（构建 `vortex_editor` 与 `gui` 模块，需设 `CMAKE_PREFIX_PATH`）。

### 从源码构建

```sh
# 用 CMake + Ninja 配置（启用 LLVM 后端）
cmake -S . -B build_ninja -G Ninja -DVORTEX_WITH_LLVM=ON

# 构建解释器、编译器与编辑器
ninja -C build_ninja vortex vortexcc vortex_editor
```

### 获取二进制

构建成功后，`vortexcc.exe`、`vortex.exe`、`vortex_editor.exe` 与 `libvortex_runtime.a` 会发布到 `bin/` 目录。

---

## 快速开始

```vt
# hello.vt
import math;
print("hello vortex, sqrt(16) =", math.sqrt(16));
```

```sh
# 用解释器运行
vortex hello.vt

# 编译为独立可执行文件并运行
vortexcc build hello.vt -o hello.exe
./hello.exe
```

---

## 原生编译器 vortexcc

`vortexcc` 将 `.vt` 源码直接编译为原生可执行文件——运行时不再需要 `.vt` 源文件。

```
vortexcc build <file.vt> [-o out.exe] [-O0|-O1|-O2|-O3] [--asm] [--debug]
```

- `-o out.exe` —— 输出路径（默认 `<file>.exe`）。
- `-O0..-O3` —— LLVM 优化级别（默认 `-O2`）。
- `--asm` —— 同时输出汇编清单。
- `--debug` —— 生成 DWARF 调试信息（函数断点、调用栈、近似行号）。

编译器复用解释器的词法、语法分析与 AST，再经类型化 IR 通过运行时 ABI 降至原生代码。golden 测试保证解释器与编译后可执行文件行为一致。

---

## Qt 编辑器

`vortex_editor` 是仿 Python IDLE 的集成开发界面，打开 `.vt` 文件即进入。

- **编辑**：多标签、语法高亮、行号跳转、括号匹配、自动缩进、查找/替换（含全文高亮）、复制行/注释、撤销重做。
- **Shell**：底部内置 REPL，输入表达式直接求值，支持多行续行与自动缩进。
- **运行**：`Ctrl+R` 用**解释器**运行当前文件；`Ctrl+F5` 编译为 exe 并启动运行；`Ctrl+B` 仅编译（词法/语法检查）。编译/运行前自动保存当前文档。
- **调试**：`F5` 以 `--debug` 编译后交 **LLDB** 调试（批处理 `run; bt`），崩溃时打印调用栈。从 PATH 找 `lldb`，可由环境变量 `VORTEX_DEBUGGER` 指定调试器。
- **其他**：中/英双语界面、亮/暗主题，语言与主题偏好自动持久化。

---

## 完整使用指南

完整的详细使用参考（命令行、模块 API 签名、错误处理、回归测试）：[USAGE_zh.md](USAGE_zh.md)

---

## 许可证

许可证信息见项目仓库。