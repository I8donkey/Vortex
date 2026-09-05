# Vortex Usage Guide

> 简体中文版见：[中文使用指南](USAGE_zh.md)

This document is the complete operational reference for Vortex. README.md covers the language reference; here you get every CLI command, the standard-library module signatures, error handling, and the regression-test workflow.

***

## 1. Components

| Binary                | Role                                            |
| --------------------- | ----------------------------------------------- |
| `vortex`              | Tree-walking **interpreter**                    |
| `vortexcc`            | **LLVM native compiler** (`.vt` → exe)          |
| `vortex_editor`       | Qt editor integrating run + compile             |
| `libvortex_runtime.a` | Static runtime linked into compiled executables |

All binaries are published to `bin/` after a build.

***

## 2. CLI Reference

### 2.1 `vortex` — interpreter

```sh
vortex <file.vt> [args...]
```

Runs a `.vt` script through the interpreter. Supports the entire language.

### 2.2 `vortexcc` — native compiler

```
vortexcc build <file.vt> [-o out.exe] [-O0|-O1|-O2|-O3] [--asm] [--debug]
vortexcc --help
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
vortexcc build hello.vt -o hello.exe
./hello.exe            # runs the compiled program directly
```

**Debug session** (`--debug`)

```sh
vortexcc build prog.vt -o prog.exe --debug
# prog.exe now carries DWARF info: function breakpoints,
# backtraces, and approximate line numbers (LLVM 23 format).
```

**Optimization example**

```sh
vortexcc build prog.vt -O3 --asm
```

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
```

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
| `listdir(path)`             | list\[str] | directory entries (interp-only) |

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
| `names(path)`           | list\[str] | entry names (interp-only)     |

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
| `query(handle, sql)`         | list    | rows as list (interp-only) |

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

> Note: the compiler backend forwards `open`/`close`/`execute`/`table_exists`. Functions returning lists of strings (`file.listdir`, `zip.names`, `sql.query`, `sql.query_one`) work in the interpreter only, because the compiler does not yet iterate string lists.

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

- **Runtime exceptions** (e.g. `int("bad")`) are raised internally and must be caught with `try/catch(e)`; uncaught ones propagate and abort the program with the message.

- **Statically-linked output**: compiled executables use `-static`, so they run without a runtime DLL.

- **Headless** **`game2d`/`render3d`**: the graphics modules fall back to software rendering when SDL2 is absent, so every script stays runnable headlessly.

- **Debug info** may not reflect per-variable values precisely due to LLVM 23's debug format; use function breakpoints/backtraces for reliable stepping.

***

## 6. Regression Testing

The golden suite compiles each test with `vortexcc`, runs it, and compares stdout/stderr against the interpreter. It also re-verifies `--debug` builds for semantic parity.

```sh
powershell -ExecutionPolicy Bypass -File runtest.ps1
```

Covered cases (all must pass): scalars & control flow, containers, references (`@`/`~`), exceptions, closures, modules (`time`/`random`/`log`), threading/channels, and `game2d`/`render3d`.

Expected end state:

```
[ok] test_p3_closure
...
[ok] test_modules_r3d_g2d
================================
ALL PASS
```

