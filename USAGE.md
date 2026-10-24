# Vortex Usage Guide

> 简体中文版见：[中文使用指南](USAGE_zh.md)

**Version:** 1.0

This document is the complete operational reference for Vortex. README.md covers the language reference; here you get every CLI command, the standard-library module signatures, error handling, and the regression-test workflow.

***

## 1. Components

| Binary                | Role                                            |
| --------------------- | ----------------------------------------------- |
| `vortex`              | Tree-walking **interpreter**                    |
| `vtcc`            | **LLVM native compiler** (`.vt` → exe)          |
| `vortex_editor`       | Qt editor integrating run + compile             |
| `vtpm`                | `.vt` → `.vtp` packager (LZMA2)                 |
| `libvortex_runtime.a` | Static runtime linked into compiled executables |

All binaries are published to `bin/` after a build.

***

## 2. CLI Reference

### 2.1 `vortex` — interpreter

```sh
vortex <file.vt> [args...]
```

Runs a `.vt` script through the interpreter. Supports the entire language.

### 2.2 `vtcc` — native compiler

```
vtcc build <file.vt> [-o out.exe] [-O0|-O1|-O2|-O3] [--asm] [--debug]
vtcc --help
```

**Parameters**

| Option       | Description                                   |
| ------------ | --------------------------------------------- |
| `<file.vt>`  | source file to compile                        |
| `-o out.exe` | output executable path (default `<file>.exe`) |
| `-O0`        | minimal optimization                          |
| `-O1`        | basic optimization                            |
| `-O2`        | default optimization (used when omitted)      |
| `-O3`        | aggressive optimization                       |
| `--asm`      | also emit a `.s` assembly listing             |
| `--debug`    | emit DWARF debug information                  |
| `--help`     | print usage                                   |

**Artifacts** — compiling `foo.vt` produces:

- `foo.exe` — the native executable (links `libvortex_runtime.a`, statically).

- `foo.exe.ll` — the generated LLVM IR (kept for diagnostics).

- `foo.exe.o` — the object file.

- `foo.exe.s` — only with `--asm`.

**Example**

```sh
vtcc build hello.vt -o hello.exe
./hello.exe            # runs the compiled program directly
```

**Debug session** (`--debug`)

```sh
vtcc build prog.vt -o prog.exe --debug
# prog.exe now carries DWARF info: function breakpoints,
# backtraces, and approximate line numbers (LLVM 23 format).
```

**Optimization example**

```sh
vtcc build prog.vt -O3 --asm
```

***

### 2.3 `vtpm` — packager (.vt → .vtp, LZMA2 compression)

```
vtpm pack [-o out.vtp] <file.vt> [file2.vt ...]
vtpm pack [-o out.vtp] <vmake-project-dir>
vtpm list <pkg.vtp>
vtpm extract <pkg.vtp> [-o dir]
```

- `pack`: bundle one or more `.vt` files into a compressed `.vtp`.
- `list`: list the entries in a package.
- `extract`: decompress and restore files.

A `.vtp` is a private binary container (`VTPD` header + entry table + LZMA2 payloads), read/written via `liblzma`.

**vmake project mode.** When the target is a vmake project (a directory containing `VmakeLists.txt`, or the current directory), `pack` also embeds everything the project scripts reference:

- the scripts themselves: `VmakeLists.txt`, `include(...)`d `.vmake` modules and `add_subdirectory(...)` trees (followed recursively);
- every existing file named in a script: sources (`add_executable`/`add_library`/`target_sources`), `configure_file` inputs, `install(FILES ...)`, etc.;
- headers found in directories named by the script (e.g. `target_include_directories`);
- entries keep their path relative to the project root, and `extract` recreates the subdirectories.

```sh
vtpm pack . -o demo.vtp          # inside a vmake project: whole project
vtpm pack path/to/project -o demo.vtp
vtpm pack --project path/to/project
vtpm pack --no-project main.cpp  # only the listed file, ignore the project
```

`file(GLOB ...)` patterns are not expanded by the text scanner (they are resolved by `vmake`, not `vtpm`), and variables such as `${VAR}` are skipped.

**Automatic dependency bundling.** `pack` also collects and embeds the files a program depends on:

- For each `.vt` input it scans `extdef ... from "<lib>"` clauses and adds the referenced library (resolved relative to the `.vt`, then cwd).
- For each `.exe`/`.dll` input it reads the PE import table with `objdump -p` and recursively adds any imported DLL found next to the input (system DLLs that cannot be found are skipped).
- Explicitly listed files are deduplicated by file name and keep their original names; project files keep their relative paths.

```sh
# bundles mymath.vt plus libzlib.dll (referenced via extdef from)
vtpm pack mymath.vt -o mymath.vtp
```

> The bundled DLL must still be loadable at runtime when the packed program runs (place it next to the executable or on `PATH`).

### 2.4 User source modules (`import <name>.vt` / `.vtp`)

Besides built-in/extension modules, `import` can load a user source module:

```vt
# mymath.vt
def add(a, b) { return a + b; }
```

```vt
import mymath;
print(mymath.add(2, 3));
```

- Resolution: built-in/extension modules first, then `<name>.vt` or `<name>.vtp` in the **script's directory / cwd**.
- The `.vtp` entry is named `<name>.vt` (the default of `vtpm pack mymath.vt`).
- Top-level `def`s/constants/globals become module members; the interpreter runs and caches each module once.
- `vtcc` supports the same: it inlines the module source into the compilation unit and statically links module functions as `module.func`. User functions support `int`/`float`/`str`/`bool` parameters and returns. Note: compiled `int / int` is integer division (unlike the interpreter's true division).

### 2.5 `vmake` — build tool

A minimal CMake-style configure/build tool. It reads `VmakeLists.txt`, stores options in `VmakeCache.txt`, and generates **Ninja** files.

| Option | Meaning |
|---|---|
| `-S <dir>` | source directory (default `.`) |
| `-B <dir>` | build directory (default `.`) |
| `-D <var>[:<type>]=<value>` | create/update a cache entry |
| `-U <glob>` | remove matching cache entries |
| `-G <name>` | generator; only `Ninja` is implemented |
| `-C <file>` | pre-load a script before the project file |
| `--fresh` | delete `VmakeCache.txt`/`build.ninja` and reconfigure |
| `-L[A][H]`, `-LR <regex>` | list cached variables |
| `--install-prefix <dir>` | install prefix stored in the cache |
| `--project-file <name>` | alternate project file name |
| `-N` | parse and print targets, write nothing |
| `-P <file>` | run a script |
| `--build <dir> [--target <t>] [-j <n>]` | run `ninja` |
| `--install <dir>` | run `ninja install` |
| `-E <cmd> ...` | `echo`, `copy`, `copy_if_different`, `make_directory`, `remove`, `remove_directory`, `cat`, `touch`, `compare_files` |

```sh
vmake -S . -B build -G Ninja -DVMAKE_BUILD_TYPE=Debug
vmake --build build -j 8
vmake --install build
```

Script commands: `vmake_minimum_required`, `project`, `set`/`unset`/`list`, `add_executable`, `add_library`, `target_sources`, `target_include_directories`, `target_link_libraries`, `target_compile_definitions`, `target_compile_options`, `target_link_options`, `set_target_properties`, `if`/`elseif`/`else`, `foreach`, `include`, `add_subdirectory`, `message`, `file(GLOB/MAKE_DIRECTORY/WRITE)`, `get_filename_component`, `configure_file`, `install`.

Not implemented yet: `find_package`, `add_custom_command`, `add_custom_target`, `try_compile` (reported and skipped). Variables: `VMAKE_CXX_COMPILER`, `VMAKE_C_COMPILER`, `VMAKE_CXX_FLAGS`, `VMAKE_C_FLAGS`, `VMAKE_CXX_STANDARD`, `VMAKE_BUILD_TYPE`, `VMAKE_INSTALL_PREFIX`, plus `<CONFIG>`-suffixed variants.

Sample project: `vmake/examples/hello`.

***

## 3. Language Quick Reference

See [README.md](README.md#language-reference) for the full reference. The syntax is Python-inspired with explicit types:

```vt
# comment
import math;

int x = 42;
float f = 1e3;
str s = "hi";

def add(a, b) { return a + b; }
object inc = lambda(v) { return v + 1; };

if (x > 0) { print("pos"); }
for (i in [1, 2, 3]) { print(i); }

# references
int v = 5;
~@v = 10;

# exceptions
try { int z = int("bad"); }
catch(e) { print("caught: " + e); }
```

### 3.1 Classes & inheritance

```vt
class Point {
    int x;
    int y;
    def init(self, x, y) { self.x = x; self.y = y; }   # constructor
    def sum(self) { return self.x + self.y; }          # method
}

Point p = Point(3, 4);      # construct -> calls init
print(p.sum());             # 7
p.x = 10;                   # fields are readable / writable

class Point3(Point) {                              # single inheritance
    int z;
    def init(self, x, y, z) { self.x = x; self.y = y; self.z = z; }
    def sum(self) { return self.x + self.y + self.z; }   # override
}
class Point4(Point3) { }     # no own init -> inherits Point3's
Point4 s = Point4(1, 1, 1);
print(s.sum());             # 3
```

- A `class` body declares fields (`<type> <name>;`) and methods (`def name(self, ...) { ... }`).
- `class Child(Parent)` adds **single inheritance** (at most one parent).
- `init(self, ...)` is the constructor invoked by `Child(...)`. A subclass that omits `init` uses the nearest ancestor's.
- Methods resolve along the parent chain, child first, so `sum()` may be overridden.
- Fields initialize root → child; access them with `obj.field`.
- `type(instance)` returns the class name.

### 3.2 Operator overloading

```vt
class node {
    int a; int b; int c;
    def init(self, a, b, c) { self.a = a; self.b = b; self.c = c; }
    def merge(self, other: node) { return node(self.a + other.a, self.b + other.b, self.c + other.c); }
}

node operator node x + node y { return x.merge(y); }              # binary, same types
node operator node x + int  k { return x.merge(node(k, k, k)); }   # binary, mixed types
node operator - node x        { return node(-x.a, -x.b, -x.c); }   # unary prefix
node operator > node x        { return x.merge(x); }               # unary prefix

node p = node(1, 2, 3);
node r = p + node(10, 20, 30);   # -> 11
node s = -r;                     # unary
node u = p + 5;                  # mixed operands
```

- Binary form: `<ret> operator <ltype> <lname> <op> <rtype> <rname> { ... }` — declares the operator for the pair `(ltype, rtype)`.
- Unary form: `<ret> operator <op> <type> <name> { ... }` — prefix (`-`, `>`, `<`, … all usable).
- Dispatch is **static** on operand types; the left operand is tried first, then the right.
- Declarations must be at **top level** — an `operator` inside a `class` body is an error.

### 3.3 Type inference (`auto`) & destructuring

```vt
auto i = 3;                 # int
auto s = "asd";             # str
auto l = [1, 2, 3];         # list
auto t = (1, 2, 3);         # tuple literal
auto d = {1: 1, 2: 2};      # dict literal
auto n = node(7, 8, 9);     # instance

auto [t0, t1, t2] = t;             # tuple elements, by position
auto [p, q] = (10, 20);            # pair / 2-tuple
auto [m, k] = [2, 3];              # list elements
auto [x, y, z] = node(4, 5, 6);    # instance fields (root → child)

int add(int a, int b) { return a + b; }   # explicit return type
auto twice(int a) { return a * 2; }       # return type inferred
```

- `auto` **requires** an initializer; the type is taken from the value.
- Destructuring is only valid with `auto` (`auto [a, b] = expr;`).
- Unpacked elements are bound as `int`, regardless of their original type.
- The number of names must match: tuples/lists/pairs by length, instances by the total field count along the parent chain.
- Supported sources: tuple, pair, list, class instance. Other sources are an error.
- Functions and methods may also use an `auto` return type.

### 3.4 Exceptions: `raise` and typed `catch`

```vt
try {
    raise Error("boom");           # built-in error object
} catch (Error e) {                # typed catch (matches Error and subclasses)
    print("caught:" + e);          # caught:boom
} finally {
    print("finally");
}

try { raise "plain"; } catch (e) { print(e); }   # untyped catch = catch-all

try {
    int x = int("bad");            # raises a typed ValueError at runtime
} catch (ValueError e) {
    print("value-error:" + e);
}
```

- `raise <value>` throws; `<value>` may be a built-in error object or any value.
- Built-in error types: `Error` (base), `TypeError`, `ValueError`, `SintaxError`, `IndexError`, `ZeroDivisionError`.
- `catch (<Type> e)` matches `<Type>` **and its subclasses**; a non-matching exception keeps propagating to the next enclosing `try`.
- `catch (e)` (no type) catches everything. `finally` always runs.
- An uncaught exception aborts the program with its message.

### 3.5 Exact decimals (`extfloat`)

```vt
extfloat a = 0.1ext;
extfloat b = 0.2ext;

print(a + b);              # 0.3
print(0.1 + 0.2ext);       # 0.3
print(1ext / 3ext);        # 0.3333333333333333333333333  (25 significant digits)
print(7ext // 3ext);       # 2
print(7ext % 3ext);        # 1
print(0.1ext ** 3ext);     # 0.001
print(a == 0.1ext);        # true

extfloat d = extfloat("1.25");
print(type(a));            # extfloat
print(int(2.9ext));        # 2   (toward zero)
print(float(0.5ext));      # 0.5
print(str(a));             # 0.1
```

- Literals use an `ext` suffix: `1ext`, `0.1ext`.
- `+ - * / // % **`, unary `-`, and comparisons (`== < > …`) operate exactly on decimals.
- Mixed arithmetic with `int`/`float` promotes the other operand exactly (`a + 1`, `1 + a`, `0.1 + 0.2ext`, `a + 0.5`).
- Conversions: `int()` truncates toward zero, `float()` yields a binary double, `str()` yields canonical decimal text, `extfloat(str)` parses.
- Results are exact to 25 significant digits and byte-identical across both backends.

### 3.6 External C functions (`extdef`, Lib-O3)

```vt
extdef cstr zlibVersion() from "../bin/libzlib.dll";
extdef cuint crc32(cuint crc, cstr buf, cuint len) from "../bin/libzlib.dll";

print(zlibVersion());        # 1.3.1
print(crc32(0, "hello", 5)); # 907060870
```

Syntax: `extdef <ctype> <name>(<ctype> <arg>, ...) [from "<library>"];`

| C type   | Meaning             | Vortex value |
| -------- | ------------------- | ------------ |
| `cint`   | 32-bit signed int   | `int`        |
| `cuint`  | 32-bit unsigned int | `int`        |
| `clong`  | 64-bit signed int   | `int`        |
| `culong` | 64-bit unsigned int | `int`        |
| `cdouble`| C `double`          | `float`      |
| `cstr`   | `char*`             | `str`        |
| `cptr`   | opaque pointer      | `int`        |

- **Interpreter**: loads `from` at runtime (`LoadLibrary` + `GetProcAddress`) and marshals arguments / return values. `cdouble` **parameters** are rejected with a clear error (a `cdouble` *return* value works). At most 4 parameters are supported.
- **`vtcc`**: emits the external declaration; for each `from` DLL it generates a MinGW import library (`gendef` + `dlltool`) and appends it to the link step — no manual linking.
- **`vtpm pack`**: bundles the DLLs referenced by `from` (and recursively the PE imports of any `.exe`/`.dll` input).
- At runtime the DLL must be resolvable (next to the executable or on `PATH`).
- Errors: a missing library reports `cannot load library '<lib>'`; a missing symbol reports `symbol not found in '<lib>'`.

***

## 4. Standard-Library Modules

All modules must be imported first. Every function is available in both the interpreter and the compiled executable.

### 4.1 `math`

Constants: `pi`, `e`, `tau`, `inf`, `nan`.

| Function                                          | Returns | Description            |
| ------------------------------------------------- | ------- | ---------------------- |
| `sqrt(x)`                                         | double  | square root            |
| `cbrt(x)`                                         | double  | cube root              |
| `pow(x,y)`                                        | double  | `x**y`                 |
| `ipow(base,exp)`                                  | long    | integer fast power (square-and-multiply) |
| `exp`, `log(x,base)`, `log2`, `log10`             | double  | logarithms (radians)   |
| `sin/cos/tan/asin/acos/atan/atan2/sinh/cosh/tanh` | double  | trigonometric          |
| `floor`, `ceil`, `trunc`                          | long    | rounding               |
| `round(x, ndigits=0)`                             | double  | rounding to digits     |
| `fmod`, `hypot`                                   | double  | float mod / hypotenuse |
| `gcd`, `lcm`                                      | long    | integer gcd / lcm      |
| `isinf`, `isnan`                                  | bool    | floating checks        |

```vt
import math;
double area = math.pi * 5.0 ** 2;
print(math.gcd(48, 18));   # 6
print(math.lcm(4, 6));     # 12
print(math.ipow(2, 10));   # 1024 (exact integer, result type int)
```

- `ipow(base, exp)` requires `exp >= 0`; a negative exponent raises a typed `ValueError`. Overflow wraps as two's complement, identical in both backends.
- `gcd` uses Euclid's algorithm on absolute values (`gcd(0, 0) == 0`); `lcm` is `|a / gcd(a, b) * b|` (`lcm` with `0` is `0`).

### 4.2 `time`

| Function                        | Returns       | Description                                               |
| ------------------------------- | ------------- | --------------------------------------------------------- |
| `time()`                        | double        | unix timestamp (sec)                                      |
| `sleep(sec)`                    | None          | pause                                                     |
| `gmtime(ts)`                    | tuple         | UTC `(year,month,day,hour,min,sec,weekday,yearday,isdst)` |
| `localtime(ts)`                 | tuple         | local time tuple                                          |
| `mktime(t)`                     | double        | inverse of localtime                                      |
| `strftime(fmt, t)`              | str           | format `%Y %m %d %H %M %S %A %a`                          |
| `strptime(s, fmt)`              | tuple         | parse back                                                |
| `counter()` / `reset_counter()` | double / None | high-res timer                                            |
| `process_time()`                | double        | CPU time                                                  |

```vt
import time;
tuple utc = time.gmtime(1700000000);
print(time.strftime("%Y-%m-%d", utc));
time.sleep(0.5);
```

### 4.3 `random` (Mersenne Twister)

| Function                                                                 | Returns    | Description                    |
| ------------------------------------------------------------------------ | ---------- | ------------------------------ |
| `seed(a)`                                                                | None       | set seed for reproducible runs |
| `random()`                                                               | double     | \[0,1)                         |
| `uniform(a,b)`, `gauss(mu,sigma)`, `expovariate(l)`, `triangular(a,b,m)` | double     | distributions                  |
| `randint(a,b)`, `randrange(s,e,step)`                                    | long       | integers                       |
| `choice(seq)`                                                            | object     | one element                    |
| `choices(pop,w,k)`, `sample(pop,k)`                                      | list       | with-replacement / without     |
| `shuffle(lst)`                                                           | None       | in-place                       |
| `getstate()` / `setstate(s)`                                             | str / None | save/restore generator         |

```vt
import random;
random.seed(42);
print(random.randint(1, 6));
```

### 4.4 `thread`

Threads, mutex, atomics, channels, and a thread pool.

### 4.5 `log`

Structured logging utilities.

### 4.6 `file`

Filesystem operations (read/write/query). The compiler backend forwards all of these.

| Function                    | Returns    | Description                     |
| --------------------------- | ---------- | ------------------------------- |
| `read(path)`                | str        | read whole file                 |
| `write(path, data)`         | None       | overwrite file                  |
| `append(path, data)`        | None       | append to file                  |
| `exists(path)`              | bool       | path exists                     |
| `remove(path)`              | bool       | delete file/dir                 |
| `rename(from, to)`          | bool       | rename file                     |
| `size(path)`                | int        | file size in bytes (-1 on err)  |
| `isdir(path)` / `isfile(p)` | bool       | type checks                     |
| `mkdir(path)`               | bool       | create dirs (recursive)         |
| `rmdir(path)`               | bool       | remove dir tree                 |
| `listdir(path)`             | list\[str] | directory entries |

```vt
import file;
file.write("a.txt", "hello\n");
print(file.read("a.txt"));
print(file.exists("a.txt"), file.size("a.txt"));
file.remove("a.txt");
```

### 4.7 `zip` (zlib)

| Function                | Returns    | Description                   |
| ----------------------- | ---------- | ----------------------------- |
| `add(path, name, data)` | None       | add or replace an entry       |
| `extract(path, name)`   | str        | read an entry (deflate/store) |
| `count(path)`           | int        | number of entries             |
| `has(path, name)`       | bool       | entry exists                  |
| `names(path)`           | list\[str] | entry names     |

```vt
import zip;
zip.add("a.zip", "k.txt", "hello hello");
print(zip.extract("a.zip", "k.txt"));
```

### 4.8 `xml` / `html`

Lightweight markup helpers (escape / unescape / text extraction).

- `xml.escape(s)`, `xml.unescape(s)`, `xml.parse_text(xml, tag)`

- `html.escape(s)`, `html.unescape(s)`, `html.strip_tags(s)`

```vt
import xml;
import html;
print(xml.escape("a < b"));              # a &lt; b
print(xml.parse_text("<b>hi</b>", "b")); # hi
print(html.strip_tags("<p>a &amp; b</p>")); # a & b
```

### 4.9 `sql` (SQLite)

In-memory or file-backed SQLite via an opaque connection handle. Handles flow through open/execute/close in a script.

| Function                     | Returns | Description                |
| ---------------------------- | ------- | -------------------------- |
| `open(path)`                 | handle  | open DB (`":memory:"` ok)  |
| `close(handle)`              | None    | close DB                   |
| `execute(handle, sql)`       | int     | rows changed               |
| `table_exists(handle, name)` | bool    | table exists               |
| `query_one(handle, sql)`     | str     | first column, first row    |
| `query(handle, sql)`         | list    | table rows (each row a string list) |

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

> Note: the compiler backend forwards `open`/`close`/`execute`/`table_exists`/`query_one`/`query`. `query_one` returns the first-column string; `query` returns a nested string list (one string list per row) that the compiled executable can iterate directly with `for-in` and index with `row[i]`. Functions returning string lists (`file.listdir`, `zip.names`, `os.listdir`, …) are likewise iterable in compiled executables.

### 4.10 `os` (process & environment)

All functions return scalars, available in both the interpreter and the compiled executable.

| Function                | Returns  | Description              |
| ----------------------- | -------- | ------------------------ |
| `getenv(name)`          | str      | env var ("" if unset)    |
| `hasenv(name)`          | bool     | env var set?             |
| `setenv(name, val)`     | bool     | set env var              |
| `unsetenv(name)`        | bool     | unset env var            |
| `cwd()` / `chdir(path)` | str/bool | get / change working dir |
| `pid()`                 | int      | process id               |
| `platform()`            | str      | `"windows"` / `"linux"`  |
| `home()`                | str      | home directory           |
| `tempdir()`             | str      | temp directory           |
| `path_join(a, b)`       | str      | join path segments       |

```vt
import os;
os.setenv("K", "hello");
print(os.getenv("K"));            # hello
print(os.platform());             # windows
print(os.path_join("a", "b"));    # a/b
```

### 4.11 `regex` (regular expressions, std::regex)

All returns are scalar (bool/str/int), available in both interpreter and compiled executable.

| Function                    | Returns | Description                  |
| --------------------------- | ------- | ---------------------------- |
| `valid(pattern)`            | bool    | pattern compiles?            |
| `match(pattern, s)`         | bool    | whole string matches         |
| `search(pattern, s)`        | bool    | substring matches            |
| `find(pattern, s)`          | str     | first matching substring     |
| `find_all(pattern, s)`      | str     | all matches, comma-separated |
| `replace(pattern, s, repl)` | str     | replace all matches          |
| `count(pattern, s)`         | int     | number of matches            |

```vt
import regex;
print(regex.match("[0-9]+", "12345"));    # true
print(regex.find_all("\\d+", "a1b2c3"));  # 1, 2, 3
print(regex.replace("\\d+", "x7", "[N]")); # x[N]
```

### 4.12 `json` (scalar-oriented subset)

JSON parsing/stringifying of scalar values. Full object/array round-trips are not yet available in the compiler; scalar getters work in both backends.

| Function                                                        | Returns | Description                                  |
| --------------------------------------------------------------- | ------- | -------------------------------------------- |
| `valid(s)`                                                      | bool    | is valid JSON?                               |
| `parse_str(s)`                                                  | str     | parse a JSON string literal                  |
| `parse_int(s)`                                                  | int     | parse a JSON number as int                   |
| `parse_float(s)`                                                | float   | parse a JSON number as float                 |
| `parse_bool(s)`                                                 | bool    | parse true/false                             |
| `stringify_str(v)`                                              | str     | JSON-escape a string                         |
| `stringify_int(v)` / `stringify_float(v)` / `stringify_bool(v)` | str     | scalar to JSON                               |
| `stringify(v)`                                                  | str     | auto-detect scalar → JSON (interpreter-only) |

```vt
import json;
print(json.parse_str("\"hi\\n\""));
print(json.parse_int("42"));
print(json.stringify_str("a\"b"));   # "a\"b"
```

> Note: `json.stringify` (auto-type detection) is interpreter-only; the typed `stringify_*` variants and all `parse_*`/`valid` are available in the compiler.

### 4.13 `base64`

Standard Base64 encoding/decoding. Both functions return `str` and are available in the interpreter and the compiler.

| Function    | Returns | Description                     |
| ----------- | ------- | ------------------------------- |
| `encode(s)` | str     | Base64 encode                   |
| `decode(s)` | str     | Base64 decode ("" on bad input) |

```vt
import base64;
str e = base64.encode("hello");   # aGVsbG8=
print(base64.decode(e));          # hello
```

### 4.14 `datetime`

Calendar-date helpers built on a proleptic-Gregorian civil-date algorithm. All return scalars; parser/generator and arithmetic work in both interpreter and compiler. Dates use ISO `YYYY-MM-DD`.

| Function             | Returns | Description                       |
| -------------------- | ------- | --------------------------------- |
| `ymd(y, m, d)`       | str     | ISO date for year-month-day       |
| `to_iso(epoch)`      | str     | ISO date from epoch seconds (UTC) |
| `from_iso(iso)`      | int     | epoch seconds for ISO date (UTC)  |
| `today()`            | str     | today's ISO date (UTC)            |
| `add_days(iso, n)`   | str     | add `n` days (negative allowed)   |
| `days_between(a, b)` | int     | `b - a` in days (signed)          |

```vt
import datetime;
print(datetime.ymd(2024, 2, 29));          # 2024-02-29 (leap year)
print(datetime.add_days("2024-01-01", 31)); # 2024-02-01
print(datetime.days_between("2024-01-01",
                            "2024-02-01")); # 31
```

### 4.15 `game2d` (2D, software-rendered)

Images, sprites, collisions, drawing:

- Image: `new_image(w,h)`, `checker_image(w,h,tile)`, `solid_image(w,h,r,g,b)`, `image_size(img)`, `image_w`, `image_h`.

- Sprite: `new_sprite(img,x,y)`, `sprite_move`, `sprite_setpos`, `sprite_pos`, `sprite_angle`, `sprite_scale`, `sprite_visible`, `sprite_collides(a,b)`, `sprite_contains(sp,x,y)`.

- Window/loop: `set_window(w,h,t)`, `set_title`, `width`, `height`, `time`, `dt`, `running`, `set_fps`, `quit`.

- Draw: `clear(r,g,b)`, `draw_rect`, `draw_circle`, `draw_line`, `draw_text`, `draw_image`, `draw_sprite`, `draw_all_sprites`, `key_down`, `set_key`, `set_mouse`.

```vt
import game2d;
object img = game2d.checker_image(8, 4, 2);
print(game2d.image_size(img));      # (8, 4)
```

### 4.16 `render3d` (software rasterizer)

Software rasterization with Blinn-Phong lighting and a depth buffer; `render` returns an image whose size you validate with `game2d`:

- Scene: `new_scene`, `scene_add_node`, `scene_add_light`, `scene_set_ambient`, `scene_set_fog`.

- Camera: `new_camera_perspective`, `new_camera_ortho`, `camera_lookat`, `camera_set`, `camera_fov`.

- Mesh: `mesh_box`, `mesh_sphere`, `mesh_plane`, `mesh_cylinder`; `new_node`, `node_attach_mesh`, `node_set_pos/rot/scale`, `node_add_child`.

- Material/Light: `new_material`, `material_set_texture`, `new_texture_solid`, `new_texture_checker`, `new_light_dir`, `new_light_point`.

- Render: `render(scene, cam, w, h)` → image; `render_to_window`.

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

### 4.17 `csv` (CSV encode/decode, scalar)

- `to_line(a...)`, `count_fields(line, sep=",")`, `field_at(line, index, sep=",")`, `quote(field, sep=",")`, `parse_line(line)`.

### 4.18 `hash` (digests)

- `md5(s)`, `sha1(s)`, `sha256(s)` → lowercase-hex `str`.

### 4.19 `text` (string utilities)

- Case: `upper`/`lower`; trim: `trim`/`ltrim`/`rtrim`; search: `len`/`find`/`rfind`/`count`/`contains`/`starts_with`/`ends_with`;
  transform: `replace`/`slice`/`repeat`/`pad_left`/`pad_right`; chars: `char_at`/`ord`/`chr`; interpolation: `format(fmt, args...)`.

### 4.20 `net` (HTTP, Windows)

- `url_encode(s)`, `url_decode(s)`, `http_get(url)`, `http_post(url, body)` (WinHTTP-based; returns response body).

***

## 5. Error Handling & Edge Cases

- **Compile errors** are reported on stderr and exit non-zero, e.g. a g++ link failure ends with `[Link] g++ failed`.

- **Runtime exceptions** may come from built-in operations (e.g. `int("bad")` raises a typed `ValueError`) or from `raise`. Catch them with `try/catch`, optionally filtering by type (`catch (ValueError e) { ... }`); uncaught exceptions propagate and abort the program with their message. Built-in error types: `Error`, `TypeError`, `ValueError`, `SintaxError`, `IndexError`, `ZeroDivisionError`.

- **External C calls (`extdef`)**: a missing library reports `cannot load library '<lib>'` and a missing symbol reports `symbol not found in '<lib>'` (both are runtime errors, catchable like any other). The interpreter rejects `cdouble` parameters and more than 4 parameters. On the compiler side, if a `from` DLL cannot be found or an import library cannot be generated, the link step fails with `[Link] ...`.

- **Positioned diagnostics**: runtime errors use the gcc/clang-style `file:line:col: error: message` format with the offending source line and a `^` caret, and append a `did you mean 'xxx'?` hint for undefined names. The compiler likewise emits `Compile error: [codegen] ...` with `file:line:col` for lexical/parse/codegen errors, and reports statically-knowable type errors (e.g. `str + number`) as compile errors instead of crashing.

- **Statically-linked output**: compiled executables use `-static`, so they run without a runtime DLL.

- **Headless** **`game2d`/`render3d`**: the graphics modules fall back to software rendering when SDL2 is absent, so every script stays runnable headlessly.

- **Debug info** may not reflect per-variable values precisely due to LLVM 23's debug format; use function breakpoints/backtraces for reliable stepping.

***

## 6. Regression Testing

`tests/runtest.ps1` auto-discovers `tests/*.vt` and verifies in two ways:

- **Correctness tests (recommended)**: when `tests/<name>.expected` exists, it asserts the interpreter exits 0 and its stdout exactly equals the expected content (CRLF / trailing-newline insensitive). Adding a new test is just dropping a `Foo.vt` + `Foo.expected` pair into `tests/`.
- **Golden tests**: without `.expected`, the suite compiles each test with `vtcc` (also re-verifies `--debug` builds) and compares stdout/stderr against the interpreter for semantic parity.

Common options:

```sh
powershell -ExecutionPolicy Bypass -File runtest.ps1          # default: whitelist + all *.expected tests
powershell -ExecutionPolicy Bypass -File runtest.ps1 -a       # -a: also golden-verify every *.vt
powershell -ExecutionPolicy Bypass -File runtest.ps1 -j 16    # -j: parallel workers
powershell -ExecutionPolicy Bypass -File runtest.ps1 -t 20    # -t: per-step timeout seconds (default 20)
```

Every run/compile step that exceeds `-t` seconds is force-killed and reported as `FAIL (timeout)`, so a hanging test cannot stall the whole suite. A `.vt` with neither `.expected` nor whitelist membership is skipped by default (use `-a` to force it).

Covered cases (all must pass): scalars & control flow, containers, references (`@`/`~`), exceptions (`raise`, typed `catch`), closures, classes & inheritance, operator overloading, `auto`/destructuring, `type()`, `extfloat`, external C calls (`extdef`), modules (`math`/`time`/`random`/`log`/`file`/`zip`/`xml`/`html`/`sql`/`os`/`regex`/`json`/`base64`/`datetime`/`csv`/`hash`/`text`/`net`), threading/channels, and `game2d`/`render3d`. Tests shipped with a `*.expected` file (e.g. `test_x`, `test_atomic`, `test_log`, `test_mod_import`, `test_mathmod`) are checked by exact output match.

> The `extdef` test (`test_ffi`) links a DLL, so the suite copies `bin/libzlib.dll` next to the compiled executables before running them.

Expected end state (50 whitelisted tests):

```
[1/50] errtest ... ok
...
================================
ALL PASS
```

