# Vortex 使用指南

> English version: [USAGE.md](USAGE.md)

本文是 Vortex 的完整操作参考。README 负责语言参考；本文给出全部 CLI 命令、标准库模块签名、错误处理与回归测试流程。

***

## 1. 组件

| 程序                    | 作用                          |
| --------------------- | --------------------------- |
| `vortex`              | 树遍历**解释器**                  |
| `vortexcc`            | **LLVM 原生编译器**（`.vt` → exe） |
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

### 2.2 `vortexcc` —— 原生编译器

```
vortexcc build <file.vt> [-o out.exe] [-O0|-O1|-O2|-O3] [--asm] [--debug]
vortexcc --help
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
vortexcc build hello.vt -o hello.exe
./hello.exe            # 直接运行编译产物
```

**调试会话（`--debug`）**

```sh
vortexcc build prog.vt -o prog.exe --debug
# prog.exe 携带 DWARF 信息：函数断点、调用栈、近似行号（LLVM 23 格式）。
```

**优化示例**

```sh
vortexcc build prog.vt -O3 --asm
```

***

### 2.3 `vtpm` —— 打包工具（.vt → .vtp，LZMA2/7z 压缩）

```
vtpm pack [-o out.vtp] <file.vt> [file2.vt ...]
vtpm list <pkg.vtp>
vtpm extract <pkg.vtp> [-o dir]
```

- `pack`：把一个或多个 `.vt` 打包为压缩的 `.vtp`。
- `list`：列出包内条目。
- `extract`：解压缩还原。

`.vtp` 是私有二进制容器（`VTPD` 头 + 条目表 + LZMA2 压缩负载），首页 Core/解释器/编译器共用 `liblzma` 读写。

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
- 编译器（`vortexcc`）同样支持：把模块源码内联进编译单元，模块函数以 `模块名.函数名` 静态链接。用户函数参数/返回支持 `int`/`float`/`str`/`bool`。注意编译端 `int / int` 为整除（与解释器真除语义不同）。

***

## 3. 语言速查

完整参考见 [README\_zh.md](README_zh.md#语法参考)。语法借鉴 Python，采用显式类型：

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
```

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

- **运行时异常**（如 `int("bad")`）由内部抛出，必须用 `try/catch(e)` 捕获；未捕获则向上传播并以消息中止程序。

- **静态链接产物**：编译出的可执行文件使用 `-static`，无需运行时 DLL。

- **headless 的** **`game2d`/`render3d`**：无 SDL2 时回退到软件渲染，所有脚本保持可无头运行。

- **调试信息**：因 LLVM 23 调试格式原因，逐变量数值可能不精确；请用函数断点/调用栈做可靠单步。

***

## 6. 回归测试

`tests/runtest.ps1` 自动发现 `tests/*.vt`，两种校验方式：

- **正确性测试（推荐）**：存在 `tests/<name>.expected` 时，断言解释器退出码为 0 且 stdout 精确等于期望内容（CRLF/末尾换行不敏感）。新增测试只需放入一对 `Foo.vt` + `Foo.expected` 即被自动纳入回归。
- **golden 测试**：无 `.expected` 时用 `vortexcc` 编译并运行（含 `--debug` 复核），将 stdout/stderr 与解释器对比，保证两端语义一致。

常用参数：

```sh
powershell -ExecutionPolicy Bypass -File runtest.ps1          # 默认：白名单 + 所有带 .expected 的测试
powershell -ExecutionPolicy Bypass -File runtest.ps1 -a       # -a：把所有 .vt 都纳入 golden 验证
powershell -ExecutionPolicy Bypass -File runtest.ps1 -j 16    # -j：并行 worker 数
powershell -ExecutionPolicy Bypass -File runtest.ps1 -t 20    # -t：单步超时秒数（默认 20）
```

每个运行/编译步骤超过 `-t` 秒会被强制终止并报 `FAIL (timeout)`，防止挂起测试拖垮整个套件。缺失 `.expected` 且不在白名单的 `.vt` 默认跳过（可用 `-a` 强制运行）。

覆盖用例（须全部通过）：标量与控制流、容器、引用（`@`/`~`）、异常、闭包、模块（`math`/`time`/`random`/`log`/`file`/`zip`/`xml`/`html`/`sql`/`os`/`regex`/`json`/`base64`/`datetime`/`csv`/`hash`/`text`/`net`）、线程/channel，以及 `game2d`/`render3d`。带 `*.expected` 的测试（如 `test_x`、`test_atomic`、`test_log`、`test_mod_import`、`test_mathmod`）直接做输出精确校验。

预期收尾：

```
[1/38] errtest ... ok
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

