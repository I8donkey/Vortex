# Vortex

> 简体中文版见：[中文文档](README_zh.md)

Vortex is a Python-inspired scripting language with three interoperable modes: a **tree-walking interpreter**, an **LLVM native compiler** (`vortexcc`) that turns `.vt` sources into standalone machine-code executables, and a **Qt editor** that integrates both. Every language feature is implemented for *both* the interpreter and the compiler, and the two backends are kept semantically identical through golden regression tests.

Vortex ships batteries for scripting: dynamic-typed scalars, containers, functions with closures, reference semantics, exception handling, modules (`math`, `time`, `random`, `thread`, `log`, `game2d`, `render3d`), and software-rendered 2D/3D graphics.

---

## Table of Contents

- [Language Reference](#language-reference) (full syntax, Python-oriented)
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

- **Interp** ✓ **Comp** ✓

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

- **Interp** ✓ **Comp** ✓

> Note: Vortex uses explicit typing on declarations (`int x = 42;`) rather than Python's implicit binding. See [Variables](#variables).

### Types

Scalar: `int`, `uint`, `short`, `ushort`, `long`, `ulong`, `float`, `double`, `char`, `unichar`, `memadr`, `bool`, `str`, `unistr`, `bin`.

Container: `list`, `stack`, `queue`, `set`, `undset`, `dict`, `pair`, `tuple`.

Special: `object` (any), `function` (callable), `None`.

- **Interp** ✓ **Comp** ✓

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

- **Interp** ✓ **Comp** ✓

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

- **Interp** ✓ **Comp** ✓

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

- **Interp** ✓ **Comp** ✓

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

- **Interp** ✓ **Comp** ✓

### Containers & Subscripting

```vt
list a = [10, 20, 30];
print(a[0]);
a[1] = 99;

dict d = {"k": 1, "n": 2};        # or [(k, v), ...]
print(d["k"]);

set s = set([1, 2, 3]);
pair p = pair(1, "one");
tuple t = tuple(...);
```

`stack`, `queue`, `undset` are also available. `a.b` performs member access.

- **Interp** ✓ **Comp** ✓

### String Operations

```vt
str s = "hello world";
int n = s.length;              # 11
str t = "a" + "b";             # concatenation
bool has = s.contains("world");
```

- **Interp** ✓ **Comp** ✓

### Reference Semantics (`@` / `~`)

`@x` yields the address (storage slot) of a named variable; `~p` dereferences it (read or write). Works on scalars and strings.

```vt
int x = 5;
~@x = 10;      # write through the slot
print(~@x);    # 10
int y = x;
~@y += 1;      # increment through reference
```

- **Interp** ✓ **Comp** ✓

### Exceptions (`try` / `catch` / `finally`)

```vt
try {
    int v = int("bad");       # built-in conversions may throw
} catch(e) {
    print("caught: " + e);    # e = exception message
} finally {
    print("finally ok");
}
```

`throw` is not a user-facing keyword; library operations (e.g. `int("bad")`) throw internally and are caught via `try/catch`.

- **Interp** ✓ **Comp** ✓

### Import / Modules

```vt
import math;
import time as t;
from random import seed;
```

See [Standard Library Modules](#standard-library-modules).

- **Interp** ✓ **Comp** ✓

### Built-in Functions

- `print(...)` — write one or more values, space-separated.
- Type conversion casts: `int()`, `long()`, `float()`, `double()`, `str()`, `bool()`, `uint()`, `ulong()`, `char()`.
- `range(start, stop, step)` — iterate a numeric range.

### Concurrency

The `thread` module provides thread creation/join, mutex, atomics, channels, and a thread pool. See the USAGE document for signatures.

- **Interp** ✓ **Comp** ✓

### Notes vs Python

- Vortex is **type-embedded**: declarations carry explicit types (`int x = 1;`), unlike Python's implicit typing.
- Blocks use `{ }` and terminate statements with `;`, not indentation.
- Logic operators are `&&`/`||`/`!` (Python words `and`/`or`/`not` are accepted equivalently).
- Users write `#comment` the same way, and `for`/`while`/`if`/`def`/`lambda` map one-to-one.

---

## Standard Library Modules

| Module | Purpose |
|---|---|
| `math` | constants `pi` `e` `tau` `inf` `nan`; `sqrt cbrt pow exp log log2 log10 sin cos tan asin acos atan atan2 sinh cosh tanh floor ceil round fmod gcd lcm isinf isnan hypot` |
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
| `game2d` | 2D images, sprites, collisions and drawing (software-rendered) |
| `render3d` | software rasterizer: scenes, cameras, meshes, materials, lights, `render` |

---

## Installation

Prerequisites:

- A C++17 toolchain with a working `g++` (MinGW used here) for linking.
- LLVM libraries (>= 14) for the native compiler backend, discoverable via `LLVM_DIR`.

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

## Full Usage Guide

Complete, detailed usage reference (CLI commands, module API signatures, error handling, regression testing): [USAGE.md](USAGE.md)

---

## License

See the project repository for license details.