# Vortex

> 简体中文版见：[中文文档](README_zh.md)

Vortex is a Python-inspired scripting language with three interoperable modes: a **tree-walking interpreter**, an **LLVM native compiler** (`vortexcc`) that turns `.vt` sources into standalone machine-code executables, and a **Qt editor** that integrates both. Every language feature is implemented for *both* the interpreter and the compiler, and the two backends are kept semantically identical through golden regression tests.

Vortex ships batteries for scripting: dynamic-typed scalars, containers, functions with closures, classes with single inheritance, operator overloading, type inference (`auto`) and destructuring, exact base-10 decimals (`extfloat`), reference semantics, exception handling (`raise` / typed `catch`), external C function calls (`extdef`), modules (`math`, `time`, `random`, `thread`, `log`, `game2d`, `render3d`), and software-rendered 2D/3D graphics.

---

## Table of Contents

- [Language Reference](#language-reference) (full syntax, Python-oriented)
  - [Classes & Inheritance](#classes--inheritance)
  - [Operator Overloading](#operator-overloading)
  - [Type Inference (`auto`) & Destructuring](#type-inference-auto--destructuring)
  - [Exact Decimals (`extfloat`)](#exact-decimals-extfloat)
  - [External C Functions (`extdef`)](#external-c-functions-extdef)
- [Standard Library Modules](#standard-library-modules)
- [Installation](#installation)
- [Quick Start](#quick-start)
- [Native Compiler (`vortexcc`)](#native-compiler-vortexcc)
- [Full Usage Guide](#full-usage-guide)

---

## Language Reference

> Rows marked with a check are supported. **Interp** = interpreter, **Comp** = `vortexcc` (LLVM backend). All elements below are implemented in both backends and verified by the golden regression suite.

### Comments

```vt
# line comment (rest of the line is ignored)
```



### Literals

- Integers: decimal `42`, binary `0b1010`, octal `0o17`, hex `0x2A`
- Floats: `3.14`, `1e3`, `-2.5`
- Strings: double `"hi"` or single `'hi'`; escapes `\n \t \r \\ \" \' \0 \xHH`
- Booleans: `true`, `false`
- Null: `None`

```vt
int d = 42;
int h = 0x2A    # hex
float f = 1e3;
str s = "line\n\t"
str fmt = 'single quoted'
bool flag = true
object nil = None
```



> Note: Vortex uses explicit typing on declarations (`int x = 42;`) rather than Python's implicit binding. See [Variables](#variables).

### Types

Scalar: `int`, `uint`, `short`, `ushort`, `long`, `ulong`, `float`, `double`, `char`, `unichar`, `memadr`, `bool`, `str`, `unistr`, `bin`.

Container: `list`, `stack`, `queue`, `set`, `undset`, `dict`, `pair`, `tuple`.

Special: `object` (any), `function` (callable), `None`.



### Variables

Explicit type plus optional initializer; `const` for constants; `global` for module scope; `del` to remove.

```vt
int x = 1;            # typed declaration + init
float f;              # declaration without init
str s = "hi";
list l = [1, 2, 3];
pair p = pair(1, "a");

const double PI = 3.14159;   # constant
global counter = 0;          # module-level

del x;                # delete binding
```

Compound assignment mirrors Python: `x += 1`, `x -= 2`, `x *= 3`, `x /= 4`, `x //= 2`, `x %= 3`, `x &= m`, `x |= m`, `x ^= m`, `x <<= n`, `x >>= n`.



### Operators (lowest → highest precedence)

| Precedence | Operator | Meaning |
|---|---|---|
| 1 | `c ? a : b` | ternary |
| 2 | `\|\|` | logical or |
| 3 | `&&` | logical and |
| 4 | `\|` | bitwise or |
| 5 | `^` | bitwise xor |
| 6 | `&` | bitwise and |
| 7 | `==` `!=` | equality |
| 8 | `<` `>` `<=` `>=` | relational |
| 9 | `<<` `>>` | shift |
| 10 | `+` `-` | additive |
| 11 | `*` `/` `//` `%` | multiplicative (`//` integer div) |
| 12 | `**` | power (right-assoc) |
| 13 | unary `!  -  +  ~  @` | not / negate / plus / deref & addr (`not` ≡ `!`, `~` value, `@` slot addr) |
| 14 | `.` `[]` `()` | member / subscript / call (postfix) |

`not`, `and`, `or` keywords are also accepted as `!`, `&&`, `||`.

```vt
int a = (1 + 2) * 3 ** 2;   # 27
bool b = a > 20 && a < 30;  # true
int c = 7 // 2;             # 3
```



### Control Flow

```vt
if (x > 0) {
} elif (x < 0) {
} else {
}

for (item in items) {
    break;
    continue;
}

while (cond) {
    break;
}

return;      # or return value;
```



### Functions, Lambda & Closures

`def` defines named functions; `lambda` creates anonymous functions. Closures capture enclosing variables **by reference** (live reads reflect later updates). Parameters may carry default values and a vararg tail.

```vt
def add(a, b) {
    return a + b;
}
print(add(2, 3));

def greet(name, greeting = "Hello") {
    print(greeting + ", " + name);
}
greet("World");

object inc = lambda(x) { return x + 1; };   # anonymous closure
print(inc(9));

int base = 40;
object bump = lambda(x) { return x + base; };
base = 100;
print(bump(2));          # 102 — captured by reference
```

Named arguments use `name = value` at the call site.



### Classes & Inheritance

`class` declares a type with fields and methods; `class Child(Parent)` adds **single inheritance**. `init(self, ...)` is the constructor, and every method takes an explicit `self`. Method lookup walks the parent chain (child first), so subclasses **override**; fields are initialized root → child.

```vt
class Point {
    int x;
    int y;
    def init(self, x, y) { self.x = x; self.y = y; }
    def sum(self) { return self.x + self.y; }
}
Point p = Point(3, 4);
print(p.sum());          # 7
p.x = 10;                # fields are mutable
print(p.sum());          # 14

class Point3(Point) {                     # single inheritance
    int z;
    def init(self, x, y, z) { self.x = x; self.y = y; self.z = z; }
    def sum(self) { return self.x + self.y + self.z; }   # override
}
Point3 q = Point3(1, 2, 3);
print(q.sum());          # 6
print(q.x);              # 1 — inherited field
```

A subclass inherits fields and methods even when it declares no `init` of its own (the parent `init` is used).



### Operator Overloading

Top-level `operator` declarations overload a binary or unary operator for specific operand types (static dispatch on the operand types).

```vt
class node {
    int a; int b; int c;
    def init(self, a, b, c) { self.a = a; self.b = b; self.c = c; }
    def merge(self, other: node) { return node(self.a + other.a, self.b + other.b, self.c + other.c); }
}

node operator node x + node y     { return x.merge(y); }            # binary, same types
node operator node x + int  k     { return x.merge(node(k, k, k)); } # binary, mixed types
node operator - node x            { return node(-x.a, -x.b, -x.c); } # unary prefix
node operator > node x            { return x.merge(x); }             # unary prefix

node p = node(1, 2, 3);
node r = p + node(10, 20, 30);
node s = -r;
node u = p + 5;
```

Form: `<ret> operator <ltype> <lname> <op> <rtype> <rname> { ... }` for binary, `<ret> operator <op> <type> <name> { ... }` for unary. Declarations are allowed **only at top level** — not inside a `class` body. Both `>` and `<` may be used as prefix unary operators this way.



### Type Inference (`auto`) & Destructuring

`auto` infers a variable's type from its initializer. `auto [a, b, ...] = expr;` destructures a value positionally.

```vt
auto i = 3;
auto s = "asd";
auto l = [1, 2, 3];
auto t = (1, 2, 3);           # tuple literal
auto d = {1: 1, 2: 2};        # dict literal
auto n = node(7, 8, 9);

auto [t0, t1, t2] = t;              # tuple
auto [p, q] = (10, 20);             # pair / 2-tuple
auto [m, k] = [2, 3];               # list elements
auto [x, y, z] = node(4, 5, 6);     # class instance fields (root → child)
```

Rules: `auto` requires an initializer; destructuring is only allowed with `auto`; the unpacked elements are bound as `int`; the name count must match (for an instance, the total field count along the parent chain). Supported sources: tuple, pair, list, and class instances (fields only, not methods).

Functions and methods accept a **`auto` return type** as well:

```vt
int add(int a, int b) { return a + b; }   # explicit return type
auto twice(int a) { return a * 2; }       # return type inferred
```

Destructuring binds by position, and the tuple literal `(a, b, c)` is also how tuples are written (`tuple` values report `auto_generated_class(...)` via `type()`, see [Built-in Functions](#built-in-functions)).



### Exact Decimals (`extfloat`)

`extfloat` is an exact base-10 decimal type (like Python's `decimal`), written with an `ext` suffix. Arithmetic, comparison and conversion are exact and identical across both backends.

```vt
extfloat a = 0.1ext;
extfloat b = 0.2ext;
print(a + b);          # 0.3
print(0.1 + 0.2ext);   # 0.3
print(1ext / 3ext);    # 0.3333333333333333333333333  (25 significant digits)
print(7ext // 3ext);   # 2
print(7ext % 3ext);    # 1
print(0.1ext ** 3ext); # 0.001
print(a == 0.1ext);    # true
print(type(a));        # extfloat

extfloat d = extfloat("1.25");
print(int(2.9ext));    # 2   (truncates toward zero)
print(float(0.5ext));  # 0.5
print(str(a));         # 0.1
```

An `extfloat` literal is written `0.1ext` (or `1ext`). Mixing with `int`/`float` promotes the other operand exactly (e.g. `a + 1`, `1 + a`, `0.1 + 0.2ext`). Division keeps 25 significant digits; `//` (floor division) and `%` follow sign conventions consistent with the integer operators.



### External C Functions (`extdef`)

`extdef` declares a function that lives in an external C library / DLL and calls it directly. Supported C types: `cint`, `cuint`, `clong`, `culong`, `cdouble`, `cstr` (string), `cptr` (opaque pointer).

```vt
extdef cstr zlibVersion() from "../bin/libzlib.dll";
extdef cuint crc32(cuint crc, cstr buf, cuint len) from "../bin/libzlib.dll";

print(zlibVersion());        # 1.3.1
print(crc32(0, "hello", 5)); # 907060870
```

- The **interpreter** loads the library at runtime (`LoadLibrary` + `GetProcAddress`) and marshals arguments/return values. `cdouble` **parameters** are not supported by the interpreter (a `cdouble` *return* value is fine).
- **`vortexcc`** declares the symbol and links automatically: for each `from` DLL it generates a MinGW import library (`gendef` + `dlltool`) and appends it to the link step — no manual linking required.
- **`vtpm pack`** bundles the DLLs referenced by `extdef ... from` into the `.vtp` (and recursively scans the PE import tables of any `.exe`/`.dll` input), so the package is self-contained.
- The `.dll` must be resolvable at runtime (next to the executable or on `PATH`).



### Containers & Subscripting

```vt
list a = [10, 20, 30];
print(a[0]);
a[1] = 99;

dict d = {"k": 1, "n": 2};        # or [(k, v), ...]
print(d["k"]);

set s = set([1, 2, 3]);
pair p = pair(1, "one");
tuple t = (1, 2, 3);              # tuple literal
```

`stack`, `queue`, `undset` are also available. `a.b` performs member access.



### String Operations

```vt
str s = "hello world";
int n = s.length;              # 11
str t = "a" + "b";             # concatenation
bool has = s.contains("world");
```



### Reference Semantics (`@` / `~`)

`@x` yields the address (storage slot) of a named variable; `~p` dereferences it (read or write). Works on scalars and strings.

```vt
int x = 5;
~@x = 10;      # write through the slot
print(~@x);    # 10
int y = x;
~@y += 1;      # increment through reference
```



### Exceptions (`try` / `catch` / `finally`, `raise`)

```vt
try {
    int v = int("bad");       # built-in conversions may throw
} catch(e) {
    print("caught: " + e);    # e = exception message
} finally {
    print("finally ok");
}
```

Programs can also raise their own exceptions with `raise`, and `catch` can **filter by type**:

```vt
try {
    raise Error("boom");          # raise a built-in error object
} catch (Error e) {               # typed catch: matches Error and its subclasses
    print("caught:" + e);         # caught:boom
}

try { raise TypeError("typed"); } catch (e) { print(e); }

try { raise "plain"; } catch (e) { print(e); }   # a bare value is caught by an untyped catch
```

- `raise <value>` throws. The value may be a built-in error object or a plain value.
- Built-in error types: `Error` (base), `TypeError`, `ValueError`, `SintaxError`, `IndexError`, `ZeroDivisionError`. `e` prints/concatenates as its message.
- `catch (<Type> e)` matches that type and its subclasses; a **non-matching** exception propagates to the next enclosing `try`. `catch (e)` (no type) catches everything.
- Built-in runtime errors are typed too: `int("bad")` raises a `ValueError`, so `catch (ValueError e)` matches it.



### Import / Modules

```vt
import math;
import time as t;
from random import seed;
```

See [Standard Library Modules](#standard-library-modules).



### Built-in Functions

- `print(...)` — write one or more values, space-separated.
- Type conversion casts: `int()`, `long()`, `float()`, `double()`, `str()`, `bool()`, `uint()`, `ulong()`, `char()`.
- `extfloat(x)` — build an exact decimal from a string or number.
- `type(v)` — the value's type name (`int` / `double` / `bool` / `char` / `str` / `list` / `dict` / `extfloat`, a class name for instances, and `auto_generated_class(<elem types>)` for tuples, e.g. `type((1, 2, true, 'c'))` → `auto_generated_class(int,int,bool,char)`).
- `range(start, stop, step)` — iterate a numeric range.

### Concurrency

The `thread` module provides thread creation/join, mutex, atomics, channels, and a thread pool. See the USAGE document for signatures.



### Notes vs Python

- Vortex is **type-embedded**: declarations carry explicit types (`int x = 1;`), unlike Python's implicit typing. `auto` offers inference (`auto x = expr;`).
- Blocks use `{ }` and terminate statements with `;`, not indentation.
- Logic operators are `&&`/`||`/`!` (Python words `and`/`or`/`not` are accepted equivalently).
- Classes use `class Child(Parent)` for single inheritance and an explicit `self`; operators are overloaded with top-level `operator` declarations.
- Exact base-10 decimals use an `ext` suffix (`0.1ext`) instead of Python's `decimal.Decimal`.
- Users write `#comment` the same way, and `for`/`while`/`if`/`def`/`lambda` map one-to-one.

---

## Standard Library Modules

| Module | Purpose |
|---|---|
| `math` | constants `pi` `e` `tau` `inf` `nan`; `sqrt cbrt pow ipow exp log log2 log10 sin cos tan asin acos atan atan2 sinh cosh tanh floor ceil round fmod gcd lcm isinf isnan hypot` |
| `time` | `time sleep gmtime localtime mktime strftime strptime counter reset_counter process_time` |
| `random` | `seed getstate setstate random uniform randint randrange choice choices shuffle sample gauss expovariate triangular` |
| `thread` | threads, mutex, atomic, channel, thread pool |
| `log` | logging |
| `file` | read/write/append/exists/remove/rename/size/isdir/isfile/mkdir/rmdir/listdir |
| `zip` | add/extract/count/has/names (zlib) |
| `xml` | escape/unescape/parse_text |
| `html` | escape/unescape/strip_tags |
| `sql` | open/close/execute/table_exists/query/query_one (SQLite) |
| `os` | getenv/hasenv/setenv/unsetenv/cwd/chdir/pid/platform/home/tempdir/path_join |
| `regex` | valid/match/search/find/find_all/replace/count |
| `json` | valid/parse_str/parse_int/parse_float/parse_bool/stringify_* |
| `base64` | encode/decode |
| `datetime` | ymd/to_iso/from_iso/today/add_days/days_between |
| `csv` | to_line/count_fields/field_at/quote/parse_line (escaping) |
| `hash` | md5/sha1/sha256 (hex digests) |
| `text` | case/trim/search/replace/format interpolation string tools |
| `net` | url encode/decode/http_get/http_post (WinHTTP) |
| `sys` | version/platform/time_ms/clock/sleep/exit |
| `gui` | Qt windows/widgets/layout/events/msgbox (editor-only) |
| `game2d` | 2D images, sprites, collisions and drawing (software-rendered) |
| `render3d` | software rasterizer: scenes, cameras, meshes, materials, lights, `render` |

---

## Installation

Prerequisites:

- A C++17 toolchain (MinGW used here).
- LLVM libraries (>= 14) for the native compiler backend, discoverable via `LLVM_DIR`.
- **Optional**: LLD (auto-fetched via CPM, lets `vortexcc` link without an external g++); LLDB (editor debug; resolved from PATH or `VORTEX_DEBUGGER`).
- **Optional**: Qt5/Qt6 Widgets (to build `vortex_editor` and the `gui` module; set `CMAKE_PREFIX_PATH`).

### Build from source

```sh
# configure with CMake + Ninja (enable the LLVM backend)
cmake -S . -B build_ninja -G Ninja -DVORTEX_WITH_LLVM=ON

# build the interpreter, compiler and editor
ninja -C build_ninja vortex vortexcc vortex_editor
```

### Get binaries

`vortexcc.exe`, `vortex.exe`, `vortex_editor.exe` and `libvortex_runtime.a` are published to the `bin/` directory after a successful build.

---

## Quick Start

```vt
# hello.vt
import math;
print("hello vortex, sqrt(16) =", math.sqrt(16));
```

```sh
# run in the interpreter
vortex hello.vt

# compile to a standalone exe and run it
vortexcc build hello.vt -o hello.exe
./hello.exe
```

---

## Native Compiler (`vortexcc`)

`vortexcc` compiles `.vt` source directly to a native executable — no `.vt` source needed at runtime.

```
vortexcc build <file.vt> [-o out.exe] [-O0|-O1|-O2|-O3] [--asm] [--debug]
```

- `-o out.exe` — output path (default `<file>.exe`).
- `-O0..-O3` — LLVM optimization level (default `-O2`).
- `--asm` — also emit assembly listing.
- `--debug` — emit DWARF debug info (function breakpoints, backtraces, approximate line numbers).

The compiler reuses the interpreter's lexer, parser and AST, then lowers typed IR to native code through the runtime ABI. Golden tests keep interpreter and compiled executables behaviorally identical.

---

## Module import & packaging (`vtpm`)

Import user source modules Python-style:

```vt
import mymath;              # loads mymath.vt (or packed mymath.vtp) next to the script
print(mymath.add(2, 3));
```

`vtpm` bundles `.vt` files into a compressed `.vtp` for distribution:

```sh
vtpm pack mymath.vt -o mymath.vtp   # LZMA2 compression
vtpm list mymath.vtp                # list entries
import mymath;                      # still resolves when the .vt is gone
```

When a packed `.vt` declares `extdef ... from "..."`, `vtpm` also bundles the referenced DLLs (and recursively the PE imports of any `.exe`/`.dll` input) so the package is self-contained. See [USAGE.md](USAGE.md#23-vtpm--packager-vt--vtp-lzma2-compression).

Both the interpreter and `vortexcc` support `.vt`/`.vtp` module imports.

---

## Qt Editor

`vortex_editor` is an IDLE-like integrated IDE; open a `.vt` file to start.

- **Edit**: tabbed files, syntax highlighting, go-to-line, bracket matching, auto-indent, find/replace (with full-highlight), duplicate-line/comment, undo/redo.
- **Shell**: built-in bottom REPL evaluates expressions live, with multi-line continuation and auto-indent.
- **Run**: `Ctrl+R` runs the current file via the **interpreter**; `Ctrl+F5` compiles to an exe and launches it; `Ctrl+B` compiles only (lex/parse check). The current document is auto-saved before compiling/running.
- **Debug**: `F5` rebuilds with `--debug` and drives **LLDB** (batch `run; bt`), printing the backtrace on crash. `lldb` is resolved from PATH and can be overridden via the `VORTEX_DEBUGGER` env var.
- **Other**: English/Chinese UI, light/dark themes; language and theme preferences persist automatically.

---

## Full Usage Guide

Complete, detailed usage reference (CLI commands, module API signatures, error handling, regression testing): [USAGE.md](USAGE.md)

---

## License

See the project repository for license details.