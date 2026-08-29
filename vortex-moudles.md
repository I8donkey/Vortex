## 标准库模块

以下模块在标准库中提供，使用时需通过 `import` 语句引入。

---

### math 模块

提供基本的数学函数和常量。所有三角函数均以**弧度**为单位。

#### 常量

| 名称 | 类型 | 值 | 说明 |
|------|------|----|------|
| `math.pi` | `double` | 3.141592653589793 | 圆周率 π |
| `math.e` | `double` | 2.718281828459045 | 自然对数的底 e |
| `math.tau` | `double` | 6.283185307179586 | 2π |
| `math.inf` | `double` | 无穷大 | 正无穷，用于溢出比较 |
| `math.nan` | `double` | 非数值 | 表示未定义或不可表示的数值 |

#### 函数

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `sqrt` | `(x: double)` | `double` | 返回 x 的平方根（x ≥ 0） |
| `cbrt` | `(x: double)` | `double` | 返回 x 的立方根 |
| `pow` | `(x: double, y: double)` | `double` | 返回 x^y（与运算符 `**` 等价） |
| `exp` | `(x: double)` | `double` | 返回 e^x |
| `log` | `(x: double, base: double = math.e)` | `double` | 返回以 base 为底的对数，默认自然对数 |
| `log2` | `(x: double)` | `double` | 返回以 2 为底的对数 |
| `log10` | `(x: double)` | `double` | 返回以 10 为底的对数 |
| `sin` | `(x: double)` | `double` | 正弦 |
| `cos` | `(x: double)` | `double` | 余弦 |
| `tan` | `(x: double)` | `double` | 正切 |
| `asin` | `(x: double)` | `double` | 反正弦（主值，结果在 [-π/2, π/2]） |
| `acos` | `(x: double)` | `double` | 反余弦（主值，结果在 [0, π]） |
| `atan` | `(x: double)` | `double` | 反正切（主值，结果在 [-π/2, π/2]） |
| `atan2` | `(y: double, x: double)` | `double` | 从 y/x 计算反正切（考虑象限，结果在 [-π, π]） |
| `sinh` | `(x: double)` | `double` | 双曲正弦 |
| `cosh` | `(x: double)` | `double` | 双曲余弦 |
| `tanh` | `(x: double)` | `double` | 双曲正切 |
| `asinh` | `(x: double)` | `double` | 反双曲正弦 |
| `acosh` | `(x: double)` | `double` | 反双曲余弦（x ≥ 1） |
| `atanh` | `(x: double)` | `double` | 反双曲正切（\|x\| < 1） |
| `hypot` | `(x: double, y: double)` | `double` | 返回 sqrt(x² + y²)，避免溢出 |
| `floor` | `(x: double)` | `long` | 向下取整（返回小于等于 x 的最大整数） |
| `ceil` | `(x: double)` | `long` | 向上取整（返回大于等于 x 的最小整数） |
| `trunc` | `(x: double)` | `long` | 截断小数部分（向零取整） |
| `round` | `(x: double, ndigits: int = 0)` | `double` | 四舍五入到 ndigits 位小数（默认到整数） |
| `fmod` | `(x: double, y: double)` | `double` | 浮点数取模（余数与 x 同号） |
| `gcd` | `(a: long, b: long)` | `long` | 返回 a 和 b 的最大公约数 |
| `lcm` | `(a: long, b: long)` | `long` | 返回 a 和 b 的最小公倍数 |
| `isinf` | `(x: double)` | `bool` | 若 x 为正负无穷大则返回 `true` |
| `isnan` | `(x: double)` | `bool` | 若 x 是 NaN 则返回 `true` |

#### 示例
```cpp
import math;

double r = 5.0;
double area = math.pi * r ** 2;          # 78.5398
double log_val = math.log(100, 10);      # 2.0
double angle = math.asin(0.5);           # 0.5236
long g = math.gcd(48, 18);               # 6
```

---

### time 模块

提供时间相关的函数，用于获取系统时间、暂停执行、格式化日期等。时间值以**秒**为单位，从 Unix 纪元（1970-01-01 00:00:00 UTC）开始。

#### 常量

| 名称 | 类型 | 值 | 说明 |
|------|------|----|------|
| `time.TIME_UTC` | `int` | 0 | 用于 `gmtime` 的时区标志（UTC） |

#### 函数

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `time` | `()` | `double` | 返回当前时间戳（秒，浮点数，精度依平台而定） |
| `sleep` | `(seconds: double)` | `None` | 暂停当前线程执行 `seconds` 秒 |
| `gmtime` | `(timestamp: double = time.time())` | `tuple<long, ...>` | 将时间戳转换为 UTC 时间元组：`(year, month, day, hour, minute, second, weekday, yearday, isdst)`，其中 weekday: 0=Monday, 6=Sunday；yearday: 1~366；isdst 始终为 0 |
| `localtime` | `(timestamp: double = time.time())` | `tuple<long, ...>` | 将时间戳转换为本地时间（与系统时区相关），元组格式同 `gmtime` |
| `mktime` | `(tm_tuple: tuple)` | `double` | 将本地时间元组转换为时间戳（与 `localtime` 互逆） |
| `strftime` | `(format: str, tm_tuple: tuple)` | `str` | 根据格式字符串格式化时间元组，常用格式符：`%Y`年, `%m`月, `%d`日, `%H`时, `%M`分, `%S`秒, `%A`星期全名, `%a`缩写 |
| `strptime` | `(str: str, format: str)` | `tuple<long, ...>` | 将字符串按格式解析为时间元组（与 `strftime` 互逆） |
| `counter` | `()` | `double` | 返回高精度性能计数器值（秒），从某个固定起点开始，可用于测量短时间间隔 |
| `reset_counter` | `()` | `None` | 重置性能计数器的起点为当前时刻，此后 `counter()` 返回从该时刻起经过的时间 |
| `process_time` | `()` | `double` | 返回当前进程的 CPU 时间（用户+系统），秒 |

#### 示例
```cpp
import time;

double ts = time.time();
print("Timestamp:", ts);

tuple utc = time.gmtime(ts);
str date = time.strftime("%Y-%m-%d %H:%M:%S", utc);
print("UTC time:", date);

time.sleep(1.5);

time.reset_counter();
double sum = 0.0;
for (i in range(0, 1000000)) {
    sum += math.sqrt(i);
}
double elapsed = time.counter();
print("Elapsed:", elapsed, "seconds");

double cpu = time.process_time();
print("CPU time:", cpu);
```

---

### random 模块

提供伪随机数生成器，基于梅森旋转（Mersenne Twister）算法，支持多种分布和随机操作。

#### 常量

| 名称 | 类型 | 值 | 说明 |
|------|------|----|------|
| `random.DEFAULT_SEED` | `ulong` | 自动 | 默认种子（基于系统时间） |

#### 函数

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `seed` | `(a: ulong)` | `None` | 使用整数种子初始化随机数生成器 |
| `getstate` | `()` | `str` | 返回当前生成器状态（字符串），用于保存 |
| `setstate` | `(state: str)` | `None` | 恢复先前保存的状态 |
| `random` | `()` | `double` | 返回 [0.0, 1.0) 范围内的随机浮点数 |
| `uniform` | `(a: double, b: double)` | `double` | 返回 [a, b] 范围内的随机浮点数（均匀分布） |
| `randint` | `(a: long, b: long)` | `long` | 返回 [a, b] 范围内的随机整数（含两端） |
| `randrange` | `(start: long, stop: long, step: long = 1)` | `long` | 返回 `range(start, stop, step)` 中随机选取的一个整数 |
| `choice` | `(seq: list)` | `object` | 从非空列表 `seq` 中随机返回一个元素 |
| `choices` | `(population: list, weights: list = None, k: ulong = 1)` | `list` | 从 `population` 中按权重（可选）随机选取 `k` 个元素（可重复），返回列表 |
| `shuffle` | `(lst: list)` | `None` | 原地打乱列表 `lst`（使用当前随机源） |
| `sample` | `(population: list, k: ulong)` | `list` | 从 `population` 中随机抽取 `k` 个不重复的元素，返回新列表 |
| `gauss` | `(mu: double, sigma: double)` | `double` | 返回高斯分布（正态）随机数，均值 mu，标准差 sigma |
| `normalvariate` | `(mu: double, sigma: double)` | `double` | 同 `gauss`，但使用不同的算法 |
| `expovariate` | `(lambd: double)` | `double` | 返回指数分布随机数（λ = lambd） |
| `triangular` | `(low: double, high: double, mode: double = (low+high)/2)` | `double` | 返回三角形分布随机数，范围 [low, high]，众数 mode |

#### 示例
```cpp
import random;

random.seed(12345);
double r = random.random();             # 0.123...
int dice = random.randint(1, 6);        # 1~6
list deck = ["A", "2", "3", "4", "5"];
random.shuffle(deck);
str card = random.choice(deck);

list sample = random.sample(deck, 3);

double height = random.gauss(170.0, 10.0);
```

---

### 使用说明

- 所有模块需通过 `import` 导入，如 `import math;` 或 `from math import pi;`。
- 函数返回值类型由编译器自动推导，无需显式标注。
- 时间元组为 `tuple<long, ...>`，元素顺序固定。
- 随机数生成器默认使用系统时间种子；用户可调用 `seed()` 设置固定种子以复现结果。