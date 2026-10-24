# Vortex 使用指南

> English version: [USAGE.md](USAGE.md)

**版本：** 1.0

本文是 Vortex 的完整操作参考。README 负责语言参考；本文给出全部 CLI 命令、标准库模块签名、错误处理与回归测试流程。

***

## 1. 组件

| 程序                    | 作用                          |
| --------------------- | --------------------------- |
| `vortex`              | 树遍历**解释器**                  |
| `vtcc`            | **LLVM 原生编译器**（`.vt` → exe） |
| `vortex_editor`       | 集成运行 + 编译的 Qt 编辑器           |
| `vtpm`                | `.vt` → `.vtp` 打包工具（LZMA2）      |
| `libvortex_runtime.a` | 编译产物链接的静态运行时库               |

构建后所有二进制发布到 `bin/` 目录。

***

## 2. CLI 参考

### 2.1 `vortex` —— 解释器

```sh
vortex <file.vt> [args...]
```

运行 `.vt` 脚本（解释器执行），支持完整语言。

### 2.2 `vtcc` —— 原生编译器

```
vtcc build <file.vt> [-o out.exe] [-O0|-O1|-O2|-O3] [--asm] [--debug]
vtcc --help
```

**参数**

| 选项           | 说明                         |
| ------------ | -------------------------- |
| `<file.vt>`  | 待编译的源文件                    |
| `-o out.exe` | 输出可执行文件路径（默认 `<file>.exe`） |
| `-O0`        | 最小化优化                      |
| `-O1`        | 基础优化                       |
| `-O2`        | 默认优化级别（缺省使用）               |
| `-O3`        | 激进优化                       |
| `--asm`      | 额外产出 `.s` 汇编清单             |
| `--debug`    | 生成 DWARF 调试信息              |
| `--help`     | 打印帮助                       |

**产物** —— 编译 `foo.vt` 会得到：

- `foo.exe` —— 原生可执行文件（静态链接 `libvortex_runtime.a`）。

- `foo.exe.ll` —— 生成的 LLVM IR（保留用于诊断）。

- `foo.exe.o` —— 目标文件。

- `foo.exe.s` —— 仅 `--asm` 时产生。

**示例**

```sh
vtcc build hello.vt -o hello.exe
./hello.exe            # 直接运行编译产物
```

**调试会话（`--debug`）**

```sh
vtcc build prog.vt -o prog.exe --debug
# prog.exe 携带 DWARF 信息：函数断点、调用栈、近似行号（LLVM 23 格式）。
```

**优化示例**

```sh
vtcc build prog.vt -O3 --asm
```

***

### 2.3 `vtpm` —— 打包工具（.vt → .vtp，LZMA2/7z 压缩）

```
vtpm pack [-o out.vtp] <file.vt> [file2.vt ...]
vtpm pack [-o out.vtp] <vmake 工程目录>
vtpm list <pkg.vtp>
vtpm extract <pkg.vtp> [-o dir]
```

- `pack`：把一个或多个 `.vt` 打包为压缩的 `.vtp`。
- `list`：列出包内条目。
- `extract`：解压缩还原。

`.vtp` 是私有二进制容器（`VTPD` 头 + 条目表 + LZMA2 压缩负载），首页 Core/解释器/编译器共用 `liblzma` 读写。

**vmake 工程模式。** 当打包目标是 vmake 工程（含 `VmakeLists.txt` 的目录，或当前目录就是工程根）时，`pack` 会把工程脚本引用到的所有东西一并纳入：

- 脚本本身：`VmakeLists.txt`、`include(...)` 的 `.vmake` 模块、`add_subdirectory(...)` 的子目录（递归展开）；
- 脚本中出现的、确实存在的文件：源码（`add_executable`/`add_library`/`target_sources`）、`configure_file` 输入、`install(FILES ...)` 等；
- 脚本中出现的目录里的头文件（如 `target_include_directories` 指定的 include 目录）；
- 条目名保留相对工程根的路径，`extract` 会自动重建子目录。

```sh
vtpm pack . -o demo.vtp          # 在 vmake 工程内：打包整个工程
vtpm pack path/to/project -o demo.vtp
vtpm pack --project path/to/project
vtpm pack --no-project main.cpp  # 只打列出的文件，忽略所在工程
```

文本扫描不会展开 `file(GLOB ...)`（这由 `vmake` 负责），`${VAR}` 这类变量也会跳过。

**自动打包依赖。** `pack` 还会收集并打入程序依赖的文件：

- 对每个 `.vt` 输入，扫描 `extdef ... from "<库>"` 子句，把引用的库加入包（相对 `.vt` 所在目录优先，其次当前目录）。
- 对每个 `.exe`/`.dll` 输入，用 `objdump -p` 读取 PE 导入表，递归加入与输入同目录的导入 DLL（找不到的系统 DLL 跳过）。
- 显式列出的文件按文件名去重并保留原文件名；工程文件保留相对路径。

```sh
# 打入 mymath.vt 以及（经 extdef from 引用的）libzlib.dll
vtpm pack mymath.vt -o mymath.vtp
```

> 打包程序运行时，被带入的 DLL 仍需可解析（放到可执行文件同目录，或在 `PATH` 上）。

### 2.4 用户源码模块（`import <name>.vt` / `.vtp`）

`import` 不仅能导内置/扩展模块，还可导入用户源码模块：

```vt
# mymath.vt
def add(a, b) { return a + b; }
```

```vt
import mymath;
print(mymath.add(2, 3));
```

- 解析顺序：先内置/扩展模块，再按**脚本所在目录 / 当前目录**查找 `<name>.vt` 或 `<name>.vtp`。
- `.vtp` 里的条目名为 `<name>.vt`（即 `vtpm pack mymath.vt` 默认产物）。
- 模块顶层 `def`/常量/全局 成为模块成员；解释器每次 `import` 执行一次并缓存。
- 编译器（`vtcc`）同样支持：把模块源码内联进编译单元，模块函数以 `模块名.函数名` 静态链接。用户函数参数/返回支持 `int`/`float`/`str`/`bool`。注意编译端 `int / int` 为整除（与解释器真除语义不同）。

### 2.5 `vmake` — 构建工具

轻量的 CMake 风格配置/构建工具：读取 `VmakeLists.txt`，把选项存进 `VmakeCache.txt`，生成 **Ninja** 构建文件。

| 选项 | 含义 |
|---|---|
| `-S <dir>` | 源码目录（默认 `.`） |
| `-B <dir>` | 构建目录（默认 `.`） |
| `-D <var>[:<type>]=<value>` | 创建/更新缓存条目 |
| `-U <glob>` | 删除匹配的缓存条目 |
| `-G <name>` | 生成器，目前仅实现 `Ninja` |
| `-C <file>` | 在项目文件之前预加载脚本 |
| `--fresh` | 删除 `VmakeCache.txt`/`build.ninja` 后重新配置 |
| `-L[A][H]`、`-LR <regex>` | 列出缓存变量 |
| `--install-prefix <dir>` | 安装前缀（写入缓存） |
| `--project-file <name>` | 指定另一个项目文件名 |
| `-N` | 只解析并打印目标，不写文件 |
| `-P <file>` | 执行脚本 |
| `--build <dir> [--target <t>] [-j <n>]` | 调用 `ninja` |
| `--install <dir>` | 调用 `ninja install` |
| `-E <cmd> ...` | `echo`、`copy`、`copy_if_different`、`make_directory`、`remove`、`remove_directory`、`cat`、`touch`、`compare_files` |

```sh
vmake -S . -B build -G Ninja -DVMAKE_BUILD_TYPE=Debug
vmake --build build -j 8
vmake --install build
```

支持的脚本命令：`vmake_minimum_required`、`project`、`set`/`unset`/`list`、`add_executable`、`add_library`、`target_sources`、`target_include_directories`、`target_link_libraries`、`target_compile_definitions`、`target_compile_options`、`target_link_options`、`set_target_properties`、`if`/`elseif`/`else`、`foreach`、`include`、`add_subdirectory`、`message`、`file(GLOB/MAKE_DIRECTORY/WRITE)`、`get_filename_component`、`configure_file`、`install`。

暂未实现：`find_package`、`add_custom_command`、`add_custom_target`、`try_compile`（会提示并跳过）。相关变量：`VMAKE_CXX_COMPILER`、`VMAKE_C_COMPILER`、`VMAKE_CXX_FLAGS`、`VMAKE_C_FLAGS`、`VMAKE_CXX_STANDARD`、`VMAKE_BUILD_TYPE`、`VMAKE_INSTALL_PREFIX`，以及带 `<CONFIG>` 后缀的变体。

示例工程：`vmake/examples/hello`。

***

## 3. 语言速查

完整参考见 [README\_zh.md](README_zh.md#语法参考)。语法借鉴 Python，采用显式类型：

```vt
# 注释
import math;

int x = 42;
float f = 1e3;
str s = "hi";

def add(a, b) { return a + b; }
object inc = lambda(v) { return v + 1; };

if (x > 0) { print("pos"); }
for (i in [1, 2, 3]) { print(i); }

# 引用
int v = 5;
~@v = 10;

# 异常
try { int z = int("bad"); }
catch(e) { print("caught: " + e); }
```

### 3.1 类与继承

```vt
class Point {
    int x;
    int y;
    def init(self, x, y) { self.x = x; self.y = y; }   # 构造方法
    def sum(self) { return self.x + self.y; }          # 方法
}

Point p = Point(3, 4);      # 构造 —— 调用 init
print(p.sum());             # 7
p.x = 10;                   # 字段可读写

class Point3(Point) {                              # 单继承
    int z;
    def init(self, x, y, z) { self.x = x; self.y = y; self.z = z; }
    def sum(self) { return self.x + self.y + self.z; }   # 重写
}
class Point4(Point3) { }     # 不写自己的 init —— 继承 Point3 的
Point4 s = Point4(1, 1, 1);
print(s.sum());             # 3
```

- `class` 体内声明字段（`<类型> <名字>;`）与方法（`def 名(self, ...) { ... }`）。
- `class Child(Parent)` 表示**单继承**（最多一个父类）。
- `init(self, ...)` 是构造方法，由 `Child(...)` 调用；子类若不写 `init`，使用最近的祖先的 `init`。
- 方法沿父链查找（子类优先），故 `sum()` 可被重写。
- 字段按「根 → 子」初始化，用 `obj.field` 访问。
- `type(实例)` 返回类名。

### 3.2 运算符重载

```vt
class node {
    int a; int b; int c;
    def init(self, a, b, c) { self.a = a; self.b = b; self.c = c; }
    def merge(self, other: node) { return node(self.a + other.a, self.b + other.b, self.c + other.c); }
}

node operator node x + node y { return x.merge(y); }              # 二元，同类型
node operator node x + int  k { return x.merge(node(k, k, k)); }   # 二元，混合类型
node operator - node x        { return node(-x.a, -x.b, -x.c); }   # 一元前缀
node operator > node x        { return x.merge(x); }               # 一元前缀

node p = node(1, 2, 3);
node r = p + node(10, 20, 30);   # -> 11
node s = -r;                     # 一元
node u = p + 5;                  # 混合操作数
```

- 二元形式：`<返回类型> operator <左类型> <左名> <运算符> <右类型> <右名> { ... }` —— 为 `(左类型, 右类型)` 组合声明运算符。
- 一元形式：`<返回类型> operator <运算符> <类型> <名> { ... }` —— 前缀（`-`、`>`、`<` 等皆可）。
- 分派为**静态**（按操作数类型）；先试左操作数，再试右操作数。
- 声明必须在**顶层** —— 写在 `class` 体内是错误的。

### 3.3 类型推导（`auto`）与解构

```vt
auto i = 3;                 # int
auto s = "asd";             # str
auto l = [1, 2, 3];         # list
auto t = (1, 2, 3);         # 元组字面量
auto d = {1: 1, 2: 2};      # 字典字面量
auto n = node(7, 8, 9);     # 实例

auto [t0, t1, t2] = t;             # 元组元素，按位置
auto [p, q] = (10, 20);            # pair / 二元组
auto [m, k] = [2, 3];              # 列表元素
auto [x, y, z] = node(4, 5, 6);    # 实例字段（根 → 子）

int add(int a, int b) { return a + b; }   # 显式返回类型
auto twice(int a) { return a * 2; }       # 返回类型推导
```

- `auto` **必须**有初值，类型取自初值。
- 解构只允许与 `auto` 搭配（`auto [a, b] = 表达式;`）。
- 解构出的元素统一按 `int` 处理（无论原类型）。
- 名字数量必须匹配：元组/列表/pair 按长度，实例按沿父链的字段总数。
- 支持来源：元组、pair、列表、类实例；其他来源会报错。
- 函数与方法也可使用 `auto` 返回类型。

### 3.4 异常：`raise` 与类型化 `catch`

```vt
try {
    raise Error("boom");           # 内建错误对象
} catch (Error e) {                # 类型化 catch（匹配 Error 及派生类型）
    print("caught:" + e);          # caught:boom
} finally {
    print("finally");
}

try { raise "plain"; } catch (e) { print(e); }   # 无类型 catch = 全捕获

try {
    int x = int("bad");            # 运行时抛类型化的 ValueError
} catch (ValueError e) {
    print("value-error:" + e);
}
```

- `raise <值>` 抛出；`<值>` 可以是内建错误对象，也可以是任意值。
- 内建错误类型：`Error`（基类）、`TypeError`、`ValueError`、`SintaxError`、`IndexError`、`ZeroDivisionError`。
- `catch (<类型> e)` 匹配 `<类型>` **及其派生类型**；不匹配的异常继续向外层 `try` 传播。
- `catch (e)`（不带类型）捕获一切；`finally` 总会执行。
- 未捕获的异常以消息中止程序。

### 3.5 精确十进制（`extfloat`）

```vt
extfloat a = 0.1ext;
extfloat b = 0.2ext;

print(a + b);              # 0.3
print(0.1 + 0.2ext);       # 0.3
print(1ext / 3ext);        # 0.3333333333333333333333333（25 位有效数字）
print(7ext // 3ext);       # 2
print(7ext % 3ext);        # 1
print(0.1ext ** 3ext);     # 0.001
print(a == 0.1ext);        # true

extfloat d = extfloat("1.25");
print(type(a));            # extfloat
print(int(2.9ext));        # 2  （向零截断）
print(float(0.5ext));      # 0.5
print(str(a));             # 0.1
```

- 字面量带 `ext` 后缀：`1ext`、`0.1ext`。
- `+ - * / // % **`、一元 `-` 与比较（`== < > …`）在十进制上精确运算。
- 与 `int`/`float` 混合运算时另一操作数被精确提升（`a + 1`、`1 + a`、`0.1 + 0.2ext`、`a + 0.5`）。
- 转换：`int()` 向零截断，`float()` 得到二进制 double，`str()` 得到规范十进制文本，`extfloat(str)` 解析。
- 结果精确到 25 位有效数字，两端逐字节一致。

### 3.6 外部 C 函数（`extdef`，Lib-O3）

```vt
extdef cstr zlibVersion() from "../bin/libzlib.dll";
extdef cuint crc32(cuint crc, cstr buf, cuint len) from "../bin/libzlib.dll";

print(zlibVersion());        # 1.3.1
print(crc32(0, "hello", 5)); # 907060870
```

语法：`extdef <c类型> <名字>(<c类型> <形参>, ...) [from "<库文件>"];`

| C 类型   | 含义              | Vortex 值 |
| -------- | ----------------- | --------- |
| `cint`   | 32 位有符号整型   | `int`     |
| `cuint`  | 32 位无符号整型   | `int`     |
| `clong`  | 64 位有符号整型   | `int`     |
| `culong` | 64 位无符号整型   | `int`     |
| `cdouble`| C `double`        | `float`   |
| `cstr`   | `char*`           | `str`     |
| `cptr`   | 不透明指针        | `int`     |

- **解释器**：运行时加载 `from` 指定的库（`LoadLibrary` + `GetProcAddress`）并转换实参/返回值。`cdouble` **形参**会给出明确报错（`cdouble` *返回值*可用）；最多支持 4 个形参。
- **`vtcc`**：发射外部声明；对每个 `from` 的 DLL 用 `gendef` + `dlltool` 生成 MinGW 导入库并加入链接命令 —— 无需手工链接。
- **`vtpm pack`**：把 `from` 引用的 DLL 一起打进包（并对作为输入的 `.exe`/`.dll` 递归扫描 PE 导入表）。
- 运行时该 DLL 必须可被解析（位于可执行文件同目录或在 `PATH` 上）。
- 错误：库缺失报 `cannot load library '<库>'`；符号缺失报 `symbol not found in '<库>'`。

***

## 4. 标准库模块

所有模块都需先 `import`。每个函数在解释器与编译产物中均可用。

### 4.1 `math`

常量：`pi`、`e`、`tau`、`inf`、`nan`。

| 函数                                                | 返回     | 说明                     |
| ------------------------------------------------- | ------ | ---------------------- |
| `sqrt(x)`                                         | double | 平方根                    |
| `cbrt(x)`                                         | double | 立方根                    |
| `pow(x,y)`                                        | double | `x**y`                 |
| `ipow(base,exp)`                                  | long   | 整数快速幂（平方-乘算法）           |
| `exp`、`log(x,base)`、`log2`、`log10`                | double | 对数                     |
| `sin/cos/tan/asin/acos/atan/atan2/sinh/cosh/tanh` | double | 三角函数                   |
| `floor`、`ceil`、`trunc`                            | long   | 取整                     |
| `round(x, ndigits=0)`                             | double | 四舍五入                   |
| `fmod`、`hypot`                                    | double | 浮点取模 / 斜边              |
| `fmin/fmax`、`min/max`、`abs/fabs`                  | 随参     | 最值 / 绝对值（int/float 感知） |
| `gcd`、`lcm`                                       | long   | 最大公约数 / 最小公倍数          |
| `isinf`、`isnan`                                   | bool   | 浮点判断                   |

```vt
import math;
double area = math.pi * 5.0 ** 2;
print(math.gcd(48, 18));   # 6
print(math.lcm(4, 6));     # 12
print(math.ipow(2, 10));   # 1024（精确整数，结果类型为 int）
```

- `ipow(base, exp)` 要求 `exp >= 0`；负指数抛出类型化的 `ValueError`。溢出按二补码回绕，两端一致。
- `gcd` 用绝对值上的欧几里得算法（`gcd(0, 0) == 0`）；`lcm` 为 `|a / gcd(a, b) * b|`（任一参数为 `0` 时结果为 `0`）。

### 4.2 `time`

| 函数                              | 返回            | 说明                              |
| ------------------------------- | ------------- | ------------------------------- |
| `time()`                        | double        | Unix 时间戳（秒）                     |
| `sleep(sec)`                    | None          | 暂停                              |
| `gmtime(ts)`                    | tuple         | UTC `(年,月,日,时,分,秒,星期,年日,isdst)` |
| `localtime(ts)`                 | tuple         | 本地时间元组                          |
| `mktime(t)`                     | double        | `localtime` 的逆                  |
| `strftime(fmt, t)`              | str           | 格式 `%Y %m %d %H %M %S %A %a`    |
| `strptime(s, fmt)`              | tuple         | 解析回去                            |
| `counter()` / `reset_counter()` | double / None | 高精度计时器                          |
| `process_time()`                | double        | CPU 时间                          |

```vt
import time;
tuple utc = time.gmtime(1700000000);
print(time.strftime("%Y-%m-%d", utc));
time.sleep(0.5);
```

### 4.3 `random`（梅森旋转）

| 函数                                                                    | 返回         | 说明          |
| --------------------------------------------------------------------- | ---------- | ----------- |
| `seed(a)`                                                             | None       | 设种子以便复现     |
| `random()`                                                            | double     | \[0,1)      |
| `uniform(a,b)`、`gauss(mu,sigma)`、`expovariate(l)`、`triangular(a,b,m)` | double     | 各种分布        |
| `randint(a,b)`、`randrange(s,e,step)`                                  | long       | 整数          |
| `choice(seq)`                                                         | object     | 取一个元素       |
| `choices(pop,w,k)`、`sample(pop,k)`                                    | list       | 可重复 / 不重复抽样 |
| `shuffle(lst)`                                                        | None       | 原地打乱        |
| `getstate()` / `setstate(s)`                                          | str / None | 保存 / 恢复生成器  |

```vt
import random;
random.seed(42);
print(random.randint(1, 6));
```

### 4.4 `thread`

提供线程、互斥锁、原子量、channel、线程池。

### 4.5 `log`

结构化日志工具。

### 4.6 `file`

文件系统操作（读写/属性/目录）。编译器后端全部转发。

| 函数                          | 返回         | 说明           |
| --------------------------- | ---------- | ------------ |
| `read(path)`                | str        | 读取整个文件       |
| `write(path, data)`         | None       | 覆盖写入         |
| `append(path, data)`        | None       | 追加写入         |
| `exists(path)`              | bool       | 路径是否存在       |
| `remove(path)`              | bool       | 删除文件/目录      |
| `rename(from,to)`           | bool       | 重命名          |
| `size(path)`                | int        | 字节大小（出错为 -1） |
| `isdir(path)` / `isfile(p)` | bool       | 类型判断         |
| `mkdir(path)`               | bool       | 创建目录（可多级）    |
| `rmdir(path)`               | bool       | 删除目录树        |
| `listdir(path)`             | list\[str] | 目录条目           |

```vt
import file;
file.write("a.txt", "hello\n");
print(file.read("a.txt"));
print(file.exists("a.txt"), file.size("a.txt"));
file.remove("a.txt");
```

### 4.7 `zip`（zlib）

| 函数                    | 返回         | 说明                  |
| --------------------- | ---------- | ------------------- |
| `add(path,name,data)` | None       | 添加或替换条目             |
| `extract(path,name)`  | str        | 读取条目（deflate/store） |
| `count(path)`         | int        | 条目数量                |
| `has(path,name)`      | bool       | 条目是否存在              |
| `names(path)`         | list\[str] | 条目名           |

```vt
import zip;
zip.add("a.zip", "k.txt", "hello hello");
print(zip.extract("a.zip", "k.txt"));
```

### 4.8 `xml` / `html`

轻量标记处理（转义/去转义/取文本）。

- `xml.escape(s)`、`xml.unescape(s)`、`xml.parse_text(xml, tag)`

- `html.escape(s)`、`html.unescape(s)`、`html.strip_tags(s)`

```vt
import xml;
import html;
print(xml.escape("a < b"));               # a &lt; b
print(xml.parse_text("<b>hi</b>", "b"));  # hi
print(html.strip_tags("<p>a &amp; b</p>")); # a & b
```

### 4.9 `sql`（SQLite）

内存或文件型 SQLite，通过不透明连接句柄操作。

| 函数                           | 返回     | 说明                |
| ---------------------------- | ------ | ----------------- |
| `open(path)`                 | handle | 打开数据库（`:memory:`） |
| `close(handle)`              | None   | 关闭数据库             |
| `execute(handle, sql)`       | int    | 受影响行数             |
| `table_exists(handle, name)` | bool   | 表是否存在             |
| `query_one(handle, sql)`     | str    | 第一行第一列            |
| `query(handle, sql)`         | list   | 表行（每行一个字符串列表）     |

```vt
import sql;
import file;
str db = "_vf_test.db"; file.remove(db);
object h = sql.open(db);
sql.execute(h, "CREATE TABLE t(id INTEGER, name TEXT)");
sql.execute(h, "INSERT INTO t VALUES(1, 'Alice')");
print(sql.execute(h, "DELETE FROM t WHERE id=1"));   # 1
sql.close(h);
```

> 说明：编译器后端转发 `open/close/execute/table_exists/query_one/query`。`query_one` 返回首行首列字符串，`query` 返回嵌套字符串列表（每行为一个字符串列表），编译产物支持对其直接 `for-in` 遍历与下标取值（`row[i]`）。
>
> `file.listdir`、`zip.names`、`os.listdir` 等返回字符串列表的函数同样在编译产物中可直接遍历。

### 4.10 `os`（进程与环境）

全部返回标量，解释器与编译产物均可用。

| 函数                      | 返回       | 说明                      |
| ----------------------- | -------- | ----------------------- |
| `getenv(name)`          | str      | 环境变量（未设为空串）             |
| `hasenv(name)`          | bool     | 环境变量是否已设                |
| `setenv(name, val)`     | bool     | 设置环境变量                  |
| `unsetenv(name)`        | bool     | 取消环境变量                  |
| `cwd()` / `chdir(path)` | str/bool | 获取/切换工作目录               |
| `pid()`                 | int      | 进程号                     |
| `platform()`            | str      | `"windows"` / `"linux"` |
| `home()`                | str      | 用户主目录                   |
| `tempdir()`             | str      | 临时目录                    |
| `path_join(a, b)`       | str      | 拼接路径段                   |

```vt
import os;
os.setenv("K", "hello");
print(os.getenv("K"));            # hello
print(os.platform());             # windows
print(os.path_join("a", "b"));    # a/b
```

### 4.11 `regex`（正则表达式，std::regex）

全部返回标量（bool/str/int），解释器与编译产物均可用。

| 函数                          | 返回   | 说明        |
| --------------------------- | ---- | --------- |
| `valid(pattern)`            | bool | 表达式是否可编译  |
| `match(pattern, s)`         | bool | 整个串是否匹配   |
| `search(pattern, s)`        | bool | 是否含匹配子串   |
| `find(pattern, s)`          | str  | 首个匹配子串    |
| `find_all(pattern, s)`      | str  | 全部匹配，逗号分隔 |
| `replace(pattern, s, repl)` | str  | 替换所有匹配    |
| `count(pattern, s)`         | int  | 匹配个数      |

```vt
import regex;
print(regex.match("[0-9]+", "12345"));     # true
print(regex.find_all("\\d+", "a1b2c3"));   # 1, 2, 3
print(regex.replace("\\d+", "x7", "[N]")); # x[N]
```

### 4.12 `json`（标量导向子集）

JSON 标量的解析与字符串化。对象/数组完整往返暂未在编译器支持；标量读取两端均可用。

| 函数                                                              | 返回        | 说明                |
| --------------------------------------------------------------- | --------- | ----------------- |
| `valid(s)`                                                      | bool      | 是否为合法 JSON        |
| `parse_str(s)`                                                  | str       | 解析 JSON 字符串字面量    |
| `parse_int(s)` / `parse_float(s)`                               | int/float | 解析 JSON 数字        |
| `parse_bool(s)`                                                 | bool      | 解析 true/false     |
| `stringify_str(v)`                                              | str       | 字符串 JSON 转义       |
| `stringify_int(v)` / `stringify_float(v)` / `stringify_bool(v)` | str       | 标量转 JSON          |
| `stringify(v)`                                                  | str       | 自动识别标量→JSON（仅解释器） |

```vt
import json;
print(json.parse_str("\"hi\\n\""));
print(json.parse_int("42"));
print(json.stringify_str("a\"b"));   # "a\"b"
```

> 说明：`json.stringify`（自动类型识别）仅解释器可用；带类型的 `stringify_*` 及 `parse_*`/`valid` 在编译器中可用。

### 4.13 `base64`

标准 Base64 编解码。两个函数都返回 `str`，解释器与编译器均可用。

| 函数          | 返回  | 说明                  |
| ----------- | --- | ------------------- |
| `encode(s)` | str | Base64 编码           |
| `decode(s)` | str | Base64 解码（非法输入返回空串） |

```vt
import base64;
str e = base64.encode("hello");   # aGVsbG8=
print(base64.decode(e));          # hello
```

### 4.14 `datetime`

基于 proleptic-Gregorian 公历算法的日期辅助。全部返回标量，解释器与编译器均可用；日期使用 ISO `YYYY-MM-DD`。

| 函数                   | 返回  | 说明                |
| -------------------- | --- | ----------------- |
| `ymd(y, m, d)`       | str | 年月日 → ISO 日期      |
| `to_iso(epoch)`      | str | 纪元秒 → ISO 日期（UTC） |
| `from_iso(iso)`      | int | ISO 日期 → 纪元秒（UTC） |
| `today()`            | str | 今天 ISO 日期（UTC）    |
| `add_days(iso, n)`   | str | 加 `n` 天（可为负）      |
| `days_between(a, b)` | int | `b - a` 天数（带符号）   |

```vt
import datetime;
print(datetime.ymd(2024, 2, 29));          # 2024-02-29（闰年）
print(datetime.add_days("2024-01-01", 31)); # 2024-02-01
print(datetime.days_between("2024-01-01",
                            "2024-02-01")); # 31
```

### 4.15 `game2d`（2D，软件渲染）

图像、精灵、碰撞、绘制：

- 图像：`new_image(w,h)`、`checker_image(w,h,tile)`、`solid_image(w,h,r,g,b)`、`image_size(img)`、`image_w`、`image_h`。

- 精灵：`new_sprite(img,x,y)`、`sprite_move`、`sprite_setpos`、`sprite_pos`、`sprite_angle`、`sprite_scale`、`sprite_visible`、`sprite_collides(a,b)`、`sprite_contains(sp,x,y)`。

- 窗口/主循环：`set_window(w,h,t)`、`set_title`、`width`、`height`、`time`、`dt`、`running`、`set_fps`、`quit`。

- 绘制：`clear(r,g,b)`、`draw_rect`、`draw_circle`、`draw_line`、`draw_text`、`draw_image`、`draw_sprite`、`draw_all_sprites`、`key_down`、`set_key`、`set_mouse`。

```vt
import game2d;
object img = game2d.checker_image(8, 4, 2);
print(game2d.image_size(img));      # (8, 4)
```

### 4.16 `render3d`（软件光栅化）

带 Blinn-Phong 光照与深度缓冲的 CPU 光栅化；`render` 返回图像，尺寸用 `game2d` 校验：

- 场景：`new_scene`、`scene_add_node`、`scene_add_light`、`scene_set_ambient`、`scene_set_fog`。

- 相机：`new_camera_perspective`、`new_camera_ortho`、`camera_lookat`、`camera_set`、`camera_fov`。

- 网格：`mesh_box`、`mesh_sphere`、`mesh_plane`、`mesh_cylinder`；`new_node`、`node_attach_mesh`、`node_set_pos/rot/scale`、`node_add_child`。

- 材质/光源：`new_material`、`material_set_texture`、`new_texture_solid`、`new_texture_checker`、`new_light_dir`、`new_light_point`。

- 渲染：`render(scene, cam, w, h)` → 图像；`render_to_window`。

```vt
import render3d;
import game2d;
object scene = render3d.new_scene();
object cam = render3d.new_camera_perspective();
render3d.camera_lookat(cam, 0,0,5, 0,0,0);
object box = render3d.mesh_box(2.0, 2.0, 2.0);
object n = render3d.new_node(box);
render3d.scene_add_node(scene, n);
object img = render3d.render(scene, cam, 80, 60);
print(game2d.image_size(img));      # (80, 60)
```

### 4.17 `csv`（CSV 编解码，标量）

- `to_line(a...) -> str`：字段序列拼成一行 CSV，自动转义逗号/引号

- `count_fields(line, sep=",") -> int`、`field_at(line, index, sep=",") -> str`

- `quote(field, sep=",") -> str`、`parse_line(line) -> str`

```vt
import csv;
print(csv.to_line("a", "x, y", "say \"hi\""));   # a,"x, y","say ""hi"""
print(csv.field_at("a;b;c", 1, ";"));            # b
```

### 4.18 `hash`（摘要）

- `md5(s)`、`sha1(s)`、`sha256(s)`：输出小写十六进制 `str`

```vt
import hash;
print(hash.md5("abc"));       # 900150983cd24fb0d6963f7d28e17f72
print(hash.sha256(""));       # e3b0c44298fc1c149afbf4c8996fb924...
```

### 4.19 `text`（字符串工具）

- 大小写：`upper`、`lower`、`title`

- 裁剪：`trim`、`ltrim`、`rtrim`

- 检索：`len`、`find`、`rfind`、`count`、`contains`、`starts_with`、`ends_with`

- 变换：`replace`、`slice`、`repeat`、`pad_left`、`pad_right`

- 字符：`char_at`、`ord`、`chr`

- 占位符插值：`format(fmt, args...)`（连续 `{}` 按序替换）

```vt
import text;
print(text.upper("hi"));                      # HI
print(text.trim("  x  "));                    # x
print(text.format("{} + {} = {}", 2, 3, 5));  # 2 + 3 = 5
print(text.chr(65));                          # A
```

### 4.20 `net`（网络 / HTTP，Windows）

- 编解码：`url_encode(s)`、`url_decode(s)`

- HTTP：`http_get(url) -> str`、`http_post(url, body) -> str`（基于 WinHTTP，返回响应体）

```vt
import net;
print(net.url_encode("a b"));                 # a%20b
print(net.url_decode("a%20b"));               # a b
str body = net.http_get("https://httpbin.org/get");
```

***

## 5. 错误处理与边界情况

- **编译错误**输出到 stderr 并返回非零退出码；例如 g++ 链接失败以 `[Link] g++ failed` 结束。

- **运行时异常**可能来自内建操作（如 `int("bad")` 抛类型化的 `ValueError`）或来自 `raise`。用 `try/catch` 捕获，可按类型过滤（`catch (ValueError e) { ... }`）；未捕获的异常会向上传播并以消息中止程序。内建错误类型：`Error`、`TypeError`、`ValueError`、`SintaxError`、`IndexError`、`ZeroDivisionError`。

- **外部 C 调用（`extdef`）**：库缺失报 `cannot load library '<库>'`，符号缺失报 `symbol not found in '<库>'`（均为运行时错误，可像其他异常一样捕获）。解释器不支持 `cdouble` 形参，且形参最多 4 个。编译端若找不到 `from` 的 DLL 或导入库生成失败，链接步骤以 `[Link] ...` 报错。

- **静态链接产物**：编译出的可执行文件使用 `-static`，无需运行时 DLL。

- **headless 的** **`game2d`/`render3d`**：无 SDL2 时回退到软件渲染，所有脚本保持可无头运行。

- **调试信息**：因 LLVM 23 调试格式原因，逐变量数值可能不精确；请用函数断点/调用栈做可靠单步。

***

## 6. 回归测试

`tests/runtest.ps1` 自动发现 `tests/*.vt`，两种校验方式：

- **正确性测试（推荐）**：存在 `tests/<name>.expected` 时，断言解释器退出码为 0 且 stdout 精确等于期望内容（CRLF/末尾换行不敏感）。新增测试只需放入一对 `Foo.vt` + `Foo.expected` 即被自动纳入回归。
- **golden 测试**：无 `.expected` 时用 `vtcc` 编译并运行（含 `--debug` 复核），将 stdout/stderr 与解释器对比，保证两端语义一致。

常用参数：

```sh
powershell -ExecutionPolicy Bypass -File runtest.ps1          # 默认：白名单 + 所有带 .expected 的测试
powershell -ExecutionPolicy Bypass -File runtest.ps1 -a       # -a：把所有 .vt 都纳入 golden 验证
powershell -ExecutionPolicy Bypass -File runtest.ps1 -j 16    # -j：并行 worker 数
powershell -ExecutionPolicy Bypass -File runtest.ps1 -t 20    # -t：单步超时秒数（默认 20）
```

每个运行/编译步骤超过 `-t` 秒会被强制终止并报 `FAIL (timeout)`，防止挂起测试拖垮整个套件。缺失 `.expected` 且不在白名单的 `.vt` 默认跳过（可用 `-a` 强制运行）。

覆盖用例（须全部通过）：标量与控制流、容器、引用（`@`/`~`）、异常（`raise`、类型化 `catch`）、闭包、类与继承、运算符重载、`auto`/解构、`type()`、`extfloat`、外部 C 调用（`extdef`）、模块（`math`/`time`/`random`/`log`/`file`/`zip`/`xml`/`html`/`sql`/`os`/`regex`/`json`/`base64`/`datetime`/`csv`/`hash`/`text`/`net`）、线程/channel，以及 `game2d`/`render3d`。带 `*.expected` 的测试（如 `test_x`、`test_atomic`、`test_log`、`test_mod_import`、`test_mathmod`）直接做输出精确校验。

> `extdef` 用例（`test_ffi`）会链接 DLL，套件运行前会把 `bin/libzlib.dll` 复制到编译产物旁边。

预期收尾（白名单共 50 项）：

```
[1/50] errtest ... ok
...
================================
ALL PASS
```

## 7. 错误提示

运行时错误采用 gcc/clang 风格的 `文件:行:列: error: 消息` 格式，并附出错源码行与 `^` 定位；对未定义名字附带 `did you mean 'xxx'?` 建议：

```
err.vt:3:1: error: Undefined variable: 't'; did you mean 't1'?
  | print(t);
  | ^
```

编译器同样以 `file:line:col`（`Compile error: [codegen] ...`）报告词法/解析/代码生成错误，并对静态可判定的类型错误（如 `str + 数值`）直接报编译错误而非崩溃。

