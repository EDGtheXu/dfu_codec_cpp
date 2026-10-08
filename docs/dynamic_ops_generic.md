# 泛化 ops（方案 B）设计记录

状态：**阶段 0（可行性 + 微基准）、阶段 1（ops 层 + codec 层擦除）、阶段 2（`Dynamic`）、
阶段 3（第二种格式：TOML）、阶段 4（把 JSON 层从核切出去 + 收尾）均已完成**。门禁实测：
阶段 1 绝对性能与改动前持平（2 风险 codec 解码 15 423 ns vs 基线 15 500 ns）；阶段 3 的
复测见 §4.3（隔离解码指标在**代码布局噪声**内，`unboundedMap` 成功路径的多余分配已修掉），
验收后按 §4.4 修掉了 TOML 编码的 O(N²)（400 项 248 ms → 4.2 ms，线性）。
阶段 4 之后：核 `codec.hpp` 里 `JsonValue`/`nlohmann` 各 **0** 次命中，
**没有 nlohmann 也能构建运行**（见 §4.6）——203/203 在 Release（MSVC 14.50）与
Debug（MSVC 14.44）下全绿，无 JSON 配置 18/18，TOML=OFF 配置 181/181。
分支：`generic-ops`（`zh-cn` 未受影响）。阶段 0 的原型已在阶段 4 删除；它的实测结论
完整保留在 §4，代码本身可从提交 `6b5d523` 取回。

## 1. 目标

让同一批 codec 直接工作在多种序列化格式上（JSON 今天，TOML/NBT/YAML 以后），
而不是为每种格式写一套适配器。这正是 DataFixerUpper 里 `DynamicOps<T>` 的作用。

## 2. 为什么不能照抄 Java 的 `DynamicOps<T>`

Java 的 `Codec<A>` 是**方法**对 T 泛型（`decode(DynamicOps<T>, T)`），字段里存的是泛型接口，
所以一个 `Codec<A>` 天然适配所有 T。C++17 的 `std::function` **存不了泛型 lambda**，
而本移植正是靠 **57 处 `std::function`** 做类型擦除，才换来：

* `Codec<A>` 是普通值类型，能存进 `std::vector`、能当函数返回值（`dispatch` 的
  `std::function<DataResult<Codec<E>>(K)>` 就靠它）；
* 没有虚接口的 codec 层，combinator 全是值语义组合。

所以「全模板化」（`Codec<A>` 的方法都变模板 + 到处泛型 lambda）会直接摧毁这个 API 形状：
codec 再也无法被擦除存储。**结论：B 在 C++ 里必须擦除"值类型"，而不是模板化 codec 层。**

## 3. 选型

| 子方案 | 做法 | 判定 |
| --- | --- | --- |
| B1 全模板化 | `Codec<A>` 方法模板化 + 泛型 lambda | ✗ API 崩，57 处擦除点重写，dispatch 无法表达 |
| B2 中央 variant | `using Value = std::variant<JsonValue, TomlValue, …>` | △ 每接一种格式都要改核心枚举，每个 ops 都得处理全部分支 |
| **B3 类型擦除句柄**（采用） | `Value = { 共享所有者, 节点指针, 类型标签 }`，ops 收发 `Value` | ✓ 开放（新格式不改核心）、`Codec<A>` 不变 |

### 3.1 `Value`：擦除句柄

```cpp
template <class T> const void* tagOf();   // 每类型一个稳定地址，O(1) 比较（不用 typeid）

struct Value {
  std::shared_ptr<const void> owner;  // 与 JsonValue 同一个所有者 —— 装箱零分配
  const void* node = nullptr;         // 指向 DOM 里的节点
  const void* tag = nullptr;          // &tagOf<T>()，用于跨格式/诊断时的类型检查
};
```

关键点：`JsonValue` 本身就是「`shared_ptr<const Raw>` 所有者 + 指向文档的 `const Raw*`」，
所以 `Value` 与它**同构**，装箱只是复制所有者（一次原子加一），不分配、不拷贝文档。

### 3.2 `Dynamic`：值 + 它的 ops（**阶段 2 已落地**）

DFU 的 `com.mojang.serialization.Dynamic<T>`。它解决一件事：`Codec.PASSTHROUGH` 需要一个
"原始动态值"的载体，而值离开它所属的 ops 就无法解释（`Value` 只是擦除句柄）。Dynamic 把两者
绑在一起，因此也是跨格式搬运的入口；`codecs::Passthrough` 现在是 `Codec<Dynamic>`。

```cpp
class Dynamic {
 public:
  explicit Dynamic(const DynamicOps& ops);                  // = Dynamic(ops, ops.empty())
  Dynamic(const DynamicOps& ops, Value value);
  const DynamicOps& ops() const;  const Value& value() const;
  Dynamic withValue(Value) const;  Value castTo(const DynamicOps&) const;
  Dynamic convertTo(const DynamicOps& outOps) const;        // DFU 的 convert
  template <class A> DataResult<std::pair<A, Dynamic>> decode(const Decoder<A>&) const;
  DataResult<Number> asNumber() const;  DataResult<std::string> asString() const;
  DataResult<bool> asBoolean() const;                       // 补充（DFU 在 DynamicLike 里）
  std::optional<Dynamic> get(const std::string& key) const; // DFU 返回 OptionalDynamic
  std::optional<Dynamic> getElement(int32_t index) const;   // 列表版本是补充
  Dynamic set(const std::string&, const Dynamic&) const;
  Dynamic remove(const std::string&) const;
  Dynamic update(const std::string&, const std::function<Dynamic(const Dynamic&)>&) const;
  bool operator==(const Dynamic&) const;                    // 只比值，不比 ops 身份
};
```

相对 DFU 省略的 API：`OptionalDynamic`（改用 `std::optional<Dynamic>`）、
`castTyped`/`cast`（改为按需转换 `castTo`；DFU 在 ops 不一致时抛异常）、
`asByteBufferOpt`/`asIntStreamOpt`/`asLongStreamOpt`（NBT 的字节/整数流视图）、
`merge`/`getMapValues`/`updateMapValues`（ops 层已有对应实现）、
`map`/`into`/`hashCode`（Java 的函数式糖与哈希）。

两处有意偏离：`decode` 把余下的值也包成 `Dynamic` 返回（DFU 返回裸的 `T`）；
`operator==` 只比较值而不比 ops 身份（ops 是单例，比身份会让"不同实例、同一份数据"永不相等）。

### 3.3 `convertTo` 从恒等变成真转换（**阶段 3 已落地**）

接口里本来就有它（`DynamicOps::convertTo`），阶段 1 时 `JsonOps` 还返回原值。
阶段 3 之后它是**真的转换**：由*源* ops 递归遍历自己的 DOM，逐节点调用*目标* ops 的
`createX`（`JsonOps::convertNode`，同一 ops 时仍走恒等快路径），`TomlOps` 侧同样实现。
于是 `Dynamic::convertTo` / `DynamicOps::convertTo` 真正成为格式间的桥，JSON 与 TOML
可以来回搬运同一棵树（`test/unit/toml_ops_test.cpp` 双向验证）。
JSON 的 `null` 映射为目标 ops 的 `empty()`：`convertTo` 保持 DFU 的全函数语义，
拒绝发生在写出前的校验里（例如 `dumpToml`）。

## 4. 阶段 0：原型与实测

阶段 0 的原型（`prototype/generic_ops.hpp` + `poc_main.cpp`，两个可执行文件；已在阶段 4
删除，代码见提交 `6b5d523`）做了四件事：

1. 用**真实的 `codec::JsonValue`** 做 JSON DOM，写两条完全等价的取值路径：
   A = ops 直接收发 `JsonValue`（今天的形态），B = ops 收发擦除句柄；
2. 第二种值类型「玩具 DOM」（递归 `std::variant` + `shared_ptr` 子节点），
   形状与 nlohmann 完全不同，用来证明句柄不依赖 JSON；
3. **零分配装箱**：全局 `operator new` 计数，20 万次装箱断言分配次数为 0；
4. 跨格式：`JSON → ToyOps → JSON` 逐字节相同，并且**同一个 decode 函数**在玩具 DOM 上
   解出同一个结构体。

实测（Windows x64，MSVC 14.50，Release，7 轮取最好一次；同一份
`{"id":"r-1","severity":3,"tags":["a","b","c"],"nested":{"x":7,"y":9}}`；
解码一行取 3 次运行的中位数）：

| 指标 | A：直接收发 `JsonValue` | B：擦除句柄 24B | B：擦除句柄 32B |
| --- | ---: | ---: | ---: |
| 解码 ns/op（4 字段 + 3 元素数组 + 嵌套对象，每个字段都过 ops） | ~740 | ~777 (**+4.8 %**) | ~775 (**+5.0 %**) |
| 分配数/op | 20.00 | 18.00 | 18.00 |
| 20 万次装箱的分配次数 | — | **0** | **0** |
| 每次取值的句柄校验 | — | 0.294 ns（仅 `valid()`） | 0.287 ns（标签比较） |
| 顺序遍历 1M 个句柄（缓存受限） | — | 0.33–0.38 ns/元素 | 0.50–0.54 ns/元素（**+41~53 %**） |
| `sizeof` | 24 | 24 | 32 |

3 次运行的差值区间：24B 为 +3.8~4.9 %，32B 为 +3.9~5.6 %；A 的绝对值在 737~752 ns 之间波动
（同一台机器的噪声约 ±2 %）。

**结论**

* **装箱零分配成立**（复用 `JsonValue` 的所有者），分配数不增反降（键的包装方式略有差异）。
* **擦除的代价 ≈ +4~5 %**，测于"每个字段都必须过 ops"的极端微基准；真实解码里 parser 与
  codec 组合子的占比更大，端到端分摊会更小。这**刚好压在 5 % 判据线上**，因此阶段 1 的门禁
  必须是真实 perf 层（2/400 风险项）的复测，而不是这条微基准。
* **句柄尺寸在解码路径上量不出差别**（+4.8 % vs +5.0 %，完全落在噪声内）；尺寸只在
  「大量长寿命句柄」的容器里体现（1M 遍历差 40~50 %）。codec 路径里的句柄都是短命临时量。
* **因此选 32 字节带标签句柄**：代价测不出来，却能把"把 A 格式的句柄递给 B 格式的 ops"
  这种静默 UB 变成明确诊断——原型里就真的踩到过（见 §4.1）。
* 若阶段 1 的真实数字超过 5 %，退路是给 `JsonOps` 加 JSON 快速旁路
  （`jsonView(ops)` → 直接收发 `JsonValue`，绕开装箱），而不是放弃方案。

### 4.1 原型踩到的两个坑（阶段 1 必须写进规范）

1. **不要把 `getMap` 的结果绑到临时量上**：
   `for (auto& e : ops.getMap(doc).value())` 绑定的是临时 `optional` 内部的引用，
   生命周期不延长 → 遍历已释放内存（原型里表现为"解析出全 0"）。
   必须先存局部量。今日的 codec 代码基本都这么做，但接口形状（按值返回 `vector`）会持续诱人犯错，
   阶段 1 应把这条写进注释与 code review 清单。
2. **`convertTo` 必须用 `outOps` 重建键和值**，不能把本 ops 的句柄直接递过去：
   正确做法是 `entries.emplace_back(convertTo(outOps, key), convertTo(outOps, value))`。
   忘了转换在 24 字节句柄下是**静默 UB**，32 字节句柄下才会抛错——这就是保留标签的理由。

### 4.2 阶段 1 的实测与优化（2 风险文档，MSVC 14.50 Release）

第一版转换（"每处签名换成 `Value`，`JsonOps` 用 `jsonView(value)` 取节点"）确实慢了：
codec 解码 16 422 ns（基线 15 500，+5.9 %），比值 vs 裸 parser 1.344（README 1.20）。
定位到**多余的所有权流量**——`shared_ptr` 的每一次复制都是一对原子 RMW：

| 位置 | 优化前 | 优化后 |
| --- | ---: | ---: |
| 头文件里 `.asJson()` 调用点 | 43 | **1**（只剩 `Passthrough` 解码器，它必须产出 `JsonValue`） |
| `JsonOps` 读方法（`getNumberValue`/`getStringValue`/`getList`/`getMapValues`/…） | 每次取值 `asJson()`（2 原子 + 临时 `JsonValue`） | `requireNode()` 直连 `const JsonValue::Raw*`，**0 原子** |
| 子节点装箱（`getStream`/`getMapValues`/`entries`） | 先造 `JsonValue` 再装箱 = 2 原子 | 新增 `Value::ofChild(...)` 复用外层所有者 = **1 原子**（无法再省） |
| `empty()` | 每次新建 `JsonValue::null()` 装箱再 `asJson()` 比较 | 缓存成 `JsonOps` 成员，比较走节点级 `valueEquals` |
| 写路径（record/list builder、`mergeToMap`） | 每字段 `asJson()`（2 原子）+ 存 `JsonValue`（1 原子） | `putRawMember` 直接写 nlohmann 节点，累加器存装箱句柄 = **每字段 1 原子** |
| 装箱工厂 | `Value(JsonValue)` 拷贝（2–3 原子） | `Value::of(shared_ptr<const T>, const T*)` 按值 + move（converting move，**0 原子**） |

优化后（9 次运行中位数，分母用**当次运行**的 `ordered_json::parse`）：

| 指标 | 优化前 | 优化后 | vs README 基线 |
| --- | ---: | ---: | ---: |
| `ordered_json::parse`（对照） | 12 133 ns | 12 133 ns | 比 README 的 12 900 快 6 % |
| `codec decode (pre-parsed)` | 16 422 ns | **15 423 ns** | **−0.5 %**（基线 15 500） |
| `parse + codec decode` | 28 791 ns | 28 064 ns | −1.9 %（基线 28 600） |
| `codec encode + dump` | 31 589 ns | 29 749 ns | −4.7 %（基线 31 200） |
| 比值 decode / parse | 1.344 | **1.277** | 用 README 的 parser 作分母是 **1.195 ≈ 1.20** |

**判定**：绝对指标达标（解码与改动前持平、另外两项更低）；比值 1.277 略高于 1.25 是**分母**问题
——同机 parser 比 README 记录快 6 %，把比值整体抬高，用同一分母换算即回到 1.20。

**保留 32 字节带标签句柄**：剩下那 1~2 % 对应的是句柄多出的 8 字节（缓存足迹），而标签换来的是
跨格式误用的明确诊断；为了 1~2 % 的比值去换成 24 字节（类型由 ops 自负）不划算。若将来确有需要，
这条记录就是当时的取舍依据。

### 4.3 阶段 3 的复测：layout 噪声，与一处真实的多余分配

阶段 3 结束时按惯例复测，得到"隔离解码 15 988 ns、比值 1.364"（阶段 2 为 15 270 ns /
1.245），看起来是 +3.5 % 的回归。为此做了**同源 A/B**：同一份 perf 源码、同一套编译参数，
只把 `include/codec.hpp` 换成 `81e8b9d`（阶段 2）的版本，两个 exe 交替运行 6 轮：

| 指标 | 阶段 2 头文件 | 阶段 3 头文件 | 差 |
| --- | ---: | ---: | ---: |
| `ordered_json::parse`（同样代码！） | 12 079 ns | 11 513 ns | −4.7 % |
| `codec decode (pre-parsed)` | 15 241 ns | 15 776 ns | +3.5 % |
| `parse + codec decode` | 27 778 ns | 27 953 ns | +0.6 % |
| `codec encode + dump` | 29 323 ns | 29 229 ns | −0.3 % |

**纯 parser 在两个 exe 里差了 4.7 %，而它的代码完全一样** —— 说明这个量级是**代码布局**
（函数地址/对齐）造成的，不是语义差异。于是再加一个对照：给同一个头文件、同一份源码
**多链一个纯填充 TU**（`build/phase3/ab/noise_tu.cpp`，120 个不被调用的函数），
四个 exe 交替各跑 5 轮：

| exe | header | 额外填充 TU | decode |
| --- | --- | --- | ---: |
| p2 | 阶段 2 | 无 | 15 202 ns |
| p2_noise | 阶段 2 | 有 | 15 498 ns（**+1.9 %**） |
| p3 | 阶段 3 | 无 | 15 713 ns |
| p3_noise | 阶段 3 | 有 | **15 211 ns（−3.2 %，比 p2 还快）** |

四个 exe 的极差 3.36 %。**结论：隔离解码指标的 ±3 % 是布局噪声，阶段 3 没有可测的语义回归**；
`p3_noise`（带全部阶段 3 代码）反而测出与 `p2` 相同的 15 211 ns。真正稳的是端到端
`parse + decode`（±0.6 %）。

不过复测查代码时发现一处**真实的**多余流量（本 fixture 碰不到，但任何用 `unboundedMap`
的用户都会碰到）：阶段 1 的"通用代码里最后一处 JSON 假设"改成了 ops 级询问，写法是把
键文本**无条件**取出来：

```cpp
const bool stringKey = ops.isStringKey(entry.first);
std::string keyText = ...ops.getStringValue(entry.first)...;   // ← 每个条目都拷贝一次
```

而 `DataResult::addPath(std::string_view)` 在**成功**时是恒等（`isSuccess() ? *this : …`），
也就是说这次拷贝在正常路径上纯属浪费（每个条目一次 `std::string` + 一个 `DataResult`）。
改成只在 `key.isError() || value.isError()` 时才问 ops 拿键文本，语义完全等价（错误路径的
path 逐字不变）。用 `build/phase3/ab/unbounded_bench.cpp`（200 键的 `unboundedMap`）三版本对比：

| 版本 | 200 条目解码（3 次） | ns/条目 |
| --- | ---: | ---: |
| 阶段 2（JSON 直连） | 64 900 / 65 000 / 67 800 ns | 324–339 |
| 阶段 3 首版（无条件取键文本） | 69 600 / 68 000 / 70 000 ns | 340–350（**+20~24 ns/条目**） |
| 阶段 3 修正后（出错才取） | 63 400 / 62 000 / 62 400 ns | 310–317 |

即首版确实给 `unboundedMap` 成功路径加了 ~5~7 % 的成本，修正后与阶段 2 持平（略优）。

### 4.4 阶段 3 验收后修的第二处真实缺陷：TOML 编码是 O(N²)

验收方量到：`codec.encodeStart(TomlOps::INSTANCE, doc)` 的耗时随规模**翻倍约 ×4**
（25 → 400 个风险项：1.3 ms → 248 ms），而 `dumpToml` 是线性的（0.17 → 1.80 ms）。

**根因**：`TomlOps` 当时没有覆写 `listBuilder()` / `mapBuilder()`，于是走 `DynamicOps`
的通用构造器（`UniversalListBuilder` / `UniversalRecordBuilder`），它们**逐元素**调
`mergeToList` / `mergeToMap`；而 TOML 侧这两个方法每次都
`toml::Array out = node->as<toml::Array>()`（同理 `toml::Table`）复制**整个已累积的容器**
再追加一个元素 —— 每个元素一遍全量深拷贝 ⟹ 编码 O(N²)。JsonOps 没这个问题，因为它的
`ArrayListBuilder` / `StringRecordBuilder` 把累加器挂在 `shared_ptr` 后面：DFU 的 Builder
本来就是**可变对象**，由 applicative 链按引用携带，`add` 只追加、`build` 才成型。

**修法**：给 TOML 也加 mutable 累加器 `TomlListBuilder` / `TomlRecordBuilder`
（`codec_toml.hpp`）：状态与 `add` 重载、`withErrorsFrom` / `mapError` / lifecycle、
prefix 的"空 or 非空"判定与错误消息都与 JsonOps 的对应构造器逐条一致，只有成型那一步是
TOML 专有的；`TomlOps::listBuilder()` / `mapBuilder()` 覆写为它们。
`mergeToList` / `mergeToMap` / `remove` 保留（它们是 DFU 的 ops 语义，供直接调用），
不为它们强求线性。

修完实测（`build/phase3/bench_toml3.cpp`，每个规模 best-of-5；单位 ms）：

| 风险项 | encode | 倍率 | dumpToml | 倍率 | parseToml | decode | 倍率 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 25 | 0.24 | — | 0.16 | — | 0.25 | 0.12 | — |
| 50 | 0.48 | 2.02× | 0.31 | 2.01× | 0.50 | 0.23 | 2.01× |
| 100 | 1.08 | 2.26× | 0.63 | 2.00× | 1.01 | 0.48 | 2.04× |
| 200 | 2.17 | 2.01× | 1.30 | 2.06× | 2.17 | 1.16 | 2.44× |
| 400 | **4.42** | 2.04× | 2.57 | 1.98× | 4.42 | 2.19 | 1.89× |

即 **N 翻倍 → 约 ×2，编码与解析、解码一样是线性的**；400 项的编码从 ~248 ms（验收方单次
测量）降到 **4.1~4.4 ms**（复跑三次：4.10 / 4.42 / 4.72 ms，运行间波动约 ±7 %）。

同一进程内直接对比两条构造路径（`build/phase3/bench_dump_probe.cpp`：`TomlOps` vs 显式
退回通用构造器的 `QuadraticTomlOps`），两份产出逐字节相同（`identical=yes`）：

| 风险项 | 新 encode | 旧 encode | 新 dump | 旧 dump |
| ---: | ---: | ---: | ---: | ---: |
| 25 | 0.24 | 1.19 | 0.16 | 0.16 |
| 50 | 0.47 | 3.85 | 0.32 | 0.32 |
| 100 | 0.95 | 14.45 | 0.63 | 0.63 |
| 200 | 2.10 | 55.48 | 1.27 | 1.26 |
| 400 | **4.10** | **215.30** | 2.57 | 2.59 |

顺便证伪了一个假象：验收方那版单次测量里 `dumpToml` 看起来从 1.8 ms 涨到 2.6 ms，但同一
进程内两条构造路径产出的树 dump 耗时**完全一致**（2.60 vs 2.59 ms）——差异来自测量条件
（旧版是在 247 ms 的编码循环之后紧接着测的），与本改动无关。

**取舍**：`build` 时仍对每个元素做一次深拷贝（`toml::Value` 是深拷贝类型，元素节点又属于
别的所有者，借不过来）。这正是 JsonOps 的行为（`out.push_back(*node)` / `putRawMember`），
所以两条路径的常数因子相同；关键是**逐元素累加阶段一次深拷贝都不做**。理论上可以把累加器
`move` 出去省掉成型时的那次元素拷贝，但 `withErrorsFrom` 会把失败结果连同**部分值**一起保留
（`detail::propagateErrors` 里的 `errorParts(..., builder.value(), ...)`），从共享状态里搬走
容器会让那条路径观察到 moved-from 状态，因此选择与 JSON 一致的"复制成型"。

**回归可见性**：新增 `test/perf/toml_perf_test.cpp`（可执行文件 `codec_toml_perf_tests`，
CTest 前缀 `toml_perf.`，3 个用例：一致性守卫 + 2 风险 + 400/40 风险），打印 parse / decode /
encode / dumpToml / 双向 convertTo 与 `per-risk encode`。它**单独一个可执行文件**，不并入
`codec_perf_tests`：一是让核心 perf 二进制保持不依赖 tinytoml，二是避免给那个二进制加代码
——§4.3 已经量出代码布局能移动数字 ±3 %。若编码再退回通用构造器，这里会以 `per-risk encode`
从 ~20 µs/risk 跳到 ~550 µs/risk 的形式直接暴露。

### 4.5 拆分前的准备：通用累加器里的 JSON 假设

在做"每格式一个头"之前先修了一个潜伏缺陷：`UniversalRecordBuilder::add`（通用累加器，
"没有自带构造器的 ops"的回退路径）用 `key.as<JsonValue::Raw>()` 取键名来给编码错误加位置。
对 JSON 键没问题，但对任何别的格式都只会拿到 `nullptr`，于是**错误位置被静默丢掉**。
TOML 之所以没暴露它，只是因为 `TomlOps` 恰好覆写了 `mapBuilder`。

修法：通过 ops 问（`isStringKey(key)` + `getStringValue(key)`），并把 `add` 改成类外定义
（`UniversalRecordBuilder` 定义在 `DynamicOps` 之前，类内调用 ops 成员会撞上不完整类型）。

回归测试放在 `toml_ops_test.cpp`（`UniversalRecordBuilderAsksTheOpsForKeyNames`）：直接用
`TomlOps::INSTANCE` 驱动通用累加器，断言错误仍带 `severity` 位置、非字符串键不加位置。
**该用例已用临时改回旧实现的方式验证过**：旧实现下 `location()` 为空、`describe()` 丢掉
`severity: ` 前缀，用例变红；改回新实现后通过——即这条用例真的能抓住这类回归。

### 4.6 阶段 4：把 JSON 层从核里切出去

目标：**核 `codec.hpp` 只放擦除值 + ops 接口 + codec 层**，JSON 进 `codec_json.hpp`、
TOML 留在 `codec_toml.hpp`，做到"没有 nlohmann 也能构建运行"。

改动面（按 `JsonValue`/`nlohmann` 命中统计，切分前）：1 段（`JsonValue` + `JsonParseError`）
104 + 26 次、4 段 51 次散件、5 段（`JsonOps`）57 次；2/3/6/7/8 段为 0。

| 处理 | 内容 |
| --- | --- |
| **搬走** | `JsonValue`/`JsonParseError`、`detail::nullNodeOwner`/`numberOf`/`jsonNodesEqual`、`putRawMember`/`putJsonMember`、`JsonObjectMapLike`、`ArrayListBuilder`、`StringRecordBuilder`（含它们的 `build`/`buildState` 类外定义）、`JsonOps` + `INSTANCE` |
| **留在核** | `Number`（格式无关：`getNumberValue` 返回它）、ops 接口、codec 层、`Value`/`Dynamic`、`DataResult`/`Lifecycle`、诊断与栈帧 |
| **四个互操作钩子改成"替换"** | `Value(const JsonValue&)` → JSON 头里的 `JsonValue::operator Value() const`（隐式，于是 `parse(JsonOps::INSTANCE, JsonValue::parse(text))` 照旧可写）；`Value::asJson()` → JSON 头里的自由函数 `jsonView(const Value&)`；`JsonValue::toValue()` 随类搬走；nlohmann 的 include 与说明搬到 JSON 头。**没有用 `#ifdef` 条件包含**——那会让 ODR 分叉 |
| **4 段里被漏掉的真实耦合** | `MapDecoder::compressedDecode` 与 `MapEncoder::compressedBuilder`（第 6 段，格式无关代码）直接构造 `CompressedMapLike` / `CompressedRecordBuilder`。这两个类其实**不依赖 JSON**（只有"空槽位=null"和"键文本"两处 JSON 化），因此没有再加 `DynamicOps` 钩子，而是把它们**改成格式无关**：空槽位判定用 `ops.empty()`（JSON 下就是 null），键文本走新增的 `detail::withKeyPath`（问 ops），渲染走 `ops.toString`。JSON 侧行为逐字不变（`empty()` 就是缓存的那个 null 节点） |

**硬性断言**（实测，代码与注释都算）：

```powershell
# include/codec.hpp: JsonValue=0 nlohmann=0
# include/codec.hpp 不 include codec_json.hpp / codec_toml.hpp / <nlohmann/*>
# include/codec_toml.hpp 只 include codec.hpp 与 <toml/toml.h>
```

CMake 侧：`option(CODEC_BUILD_JSON)`；核目标 `codec` 只带 `include/`，nlohmann 的 include
目录移到新的 `codec_json` INTERFACE 目标（存在性检查也跟着移过去）；JSON 测试层挂在
`CODEC_BUILD_JSON` 下，TOML 层挂在 `CODEC_BUILD_TOML` 下，跨格式冒烟挂在两者都开时。
文件小节从 8 段重编号为 7 段（少了 json/json_ops）。

**验收实测**：

| 配置 | 结果 |
| --- | --- |
| Release（MSVC 14.50），`build/` | 0 warning、**203/203** |
| Debug（MSVC 14.44），`cmake-build-debug/` | 0 warning、**203/203** |
| `CODEC_BUILD_JSON=OFF`（且把 `third_party/nlohmann/json.hpp` 改名藏起来） | 0 warning、**18/18**（`toml_unit` 15 + `toml_perf` 3，没有任何 TU 包含 JSON 头） |
| `CODEC_BUILD_TOML=OFF` | 0 warning、**181/181** |
| 断言审计 `node build/phase1/assert_audit.js` | `NO ASSERTION WAS WEAKENED OR REMOVED`（审计脚本增加了 `jsonView(X) → X`、`jsonView(*X).y → X->y` 的归一化，否则 41 条"只是改了写法"的断言会被误报为缺失） |

**原型已删除**：`prototype/` 与 `CODEC_BUILD_PROTOTYPE` 选项在阶段 4 一起移除（它还是
只认识旧核的实验产物）；§4 的实测数字是它的结论，代码可从提交 `6b5d523` 取回。

## 5. 阶段（每阶段门禁：全绿 + perf 复测）

改动面（按段量化，`JsonValue` 出现次数，切分前的口径）：

| 区域 | 次数 | 处理 |
| --- | ---: | --- |
| 1–3 段 json / lifecycle / data_result | 103 | 不动（`JsonValue` 类、`Number`、`Lifecycle`、`PathSegment`/`Frame`/`report()` 本来就格式无关） |
| 4–5 段 dynamic_ops / json_ops | **329** | 主体：`DynamicOps` 的 40+ virtual、`MapLike`、`ListBuilder`、`RecordBuilder`、`KeyCompressor` 收发 `const Value&` |
| 6–8 段 codec / codecs / record_codec | **143** | `Codec<A>::parse/encodeStart`、`Decoder/Encoder`、`MapCodec`、`DataResult<std::pair<A, Value>>`（`DataResult<…JsonValue…>` 共 121 处） |

| 阶段 | 内容 | 门禁 | 状态 |
| --- | --- | --- | --- |
| 1 | 4–5 段 + 6–8 段改签名；`JsonOps` 装箱/拆箱；`JsonValue` 公开 API 不变 | 174 + perf ≤5 % | **完成**（174/174；绝对性能持平，见 §4.2） |
| 2 | `Passthrough` → `Codec<Dynamic>`；`models/risk_def.hpp` 的 `value` 成员跟进；`Dynamic` 类型落地 | 全绿 | **完成**（181/181；`Passthrough` 严格照 `Codec.java:197-224`；新增 `test/unit/dynamic_test.cpp` 7 个用例；头文件里 `jsonView(value)` 调用点 43 → **0**） |
| 3 | `TomlOps`（[tinytoml](https://github.com/mayah/tinytoml) v0.4，用户指定）实现 ops；「同一 codec 吃 JSON/TOML → 同结构」交叉用例；`convertTo` 变成真转换；通用代码里最后两处 JSON 假设改成 ops 级钩子；TOML 侧 mutable 构造器（修掉验收发现的 O(N²) 编码） | 新增用例 | **完成**（203/203；新增 `include/codec_toml.hpp` + 15 个 unit + 4 个 smoke + 3 个 perf 用例；`CODEC_BUILD_TOML` 可选层；`getMap` 改纯虚、新增 `isStringKey` 钩子、通用 `UniversalListBuilder`；性能见 §4.3、§4.4，另修掉通用累加器的 JSON 假设见 §4.5） |
| 4 | **把 JSON 层从核切出去**：新增 `codec_json.hpp`（JsonValue / JsonOps / 构造器 / 互操作钩子），核只留 `Number` + ops 接口 + codec 层；`CODEC_BUILD_JSON` + `codec_json` 目标；`toml_ops_test` / `toml_perf_test` 做成 JSON-free；新增 `docs/adding_a_format.md` | 核里 `JsonValue`/`nlohmann` 0 命中；无 nlohmann 配置全绿 | **完成**（203/203；`codec.hpp` 5 001 → 4 017 行，`codec_json.hpp` 1 063 行；无 JSON 配置 18/18，TOML=OFF 181/181；详见 §4.6） |
| 5 | 收尾：删除 `prototype/` 与 `CODEC_BUILD_PROTOTYPE`、验证工具收进 `scripts/`、§7 复测、复核三个配置 | 三配置全绿 + 结论可复现 | **完成**（`scripts/assert_audit.js` + `scripts/verify_no_json.ps1` 随库发布；§7 的复测结论见该节） |

阶段 1 实际做出来时比原计划多做了 6–8 段（原本排在阶段 2），并顺带补了两件今天缺的东西：

* `DynamicOps::toString(const Value&)`（纯虚）：错误消息里的 `MapLike[...]` 原先硬编码 JSON
  渲染，现在由 ops 决定，否则 TOML 用户看到的会是 JSON 原文。
* `DynamicOps::valueEquals(const Value&, const Value&)`：基类 `mergeToPrimitive` 需要 DFU 的
  `prefix.equals(empty())`，不能让基类写 JSON 专有代码；JsonOps 覆盖为节点级深度相等。
* `Value` 的公开构造入口（`JsonValue → Value` 隐式转换），让
  `codec.parse(JsonOps::INSTANCE, JsonValue::parse(text))` 这类现有写法不用改。

阶段 1 还留了两处"通用代码里的 JSON 假设"，已在**阶段 3 全部改成 ops 级钩子**：

* `DynamicOps::getMap` 的默认实现（构造 `JsonObjectMapLike`）→ 改成**纯虚**，由 `JsonOps`
  与 `TomlOps` 各自实现自己格式的 `MapLike`（JSON 对象视图 / TOML 表视图）；
* `unboundedMap` 里"键是不是字符串"的判断 → 新增 `DynamicOps::isStringKey(const Value&)`
  钩子（默认走 `getStringValue` 是否成功，`JsonOps` 覆盖为"严格是 JSON 字符串"，与移植前逐字一致）；
  键文本也改成只在出错时才向 ops 索取（见 §4.3）。

阶段 3 还顺手把 `DynamicOps::listBuilder()` 的默认实现换成通用的 `UniversalListBuilder`
（逐元素 `mergeToList`，不依赖具体 DOM），`JsonOps` 仍覆盖为自己的 `ArrayListBuilder`，
因此 JSON 的列表构建路径没有变化。`TomlOps` 不复用任何 JSON 代码。

## 6. 用户可见影响

* **不变**：`Codec<A>`、`record/fieldOf/optionalFieldOf/listOf/unboundedMap/either/pair/dispatch`、
  错误定位 `risks[3].condition.or[0].op`、可点击栈帧、`CodecError`/`throwIfError`；
  `codec.parse(JsonOps::INSTANCE, JsonValue::parse(text))` 也不用改（`JsonValue → Value` 隐式）。
* **阶段 1 已经变的**：`encodeStart` 现在返回 `DataResult<Value>`（配 JsonOps 时用
  `jsonView(result())` 取回 JSON 节点）；`DynamicOps`/`MapLike`/builder 的签名收发 `Value`。
  两份 README 的示例与"值类型"章节已同步。
* **阶段 2 已经改的**：`codecs::Passthrough` 现在是 `Codec<Dynamic>`，因此
  `models/risk_def.hpp` 的 `std::optional<JsonValue> value` 变成了
  `std::optional<Dynamic> value`；渲染动态值请用
  `value->ops().toString(value->value())`（示例里就是这么做的，输出文本不变）。
* **阶段 4 的破坏性变更（include 与取值写法）**：JSON 使用者现在 include
  `codec_json.hpp`（或链接 `codec::json`）；`Value::asJson()` 这个成员没有了，改成自由函数
  `jsonView(value)`（`x->asJson()` → `jsonView(*x)`）。只 include `codec.hpp` 仍然可用，
  但那时没有 `JsonOps`/`JsonValue` —— 那正是"只用核 + 别的格式"的场景。
* **阶段 4 新增**：`include/codec_json.hpp` 入口头、`codec_json` / `codec::json` 目标、
  `CODEC_BUILD_JSON` 选项、`docs/adding_a_format.md`；`codec_toml.hpp` 不再依赖 JSON。
* **阶段 3 新增**：`include/codec_toml.hpp`（独立可选层，`CODEC_BUILD_TOML` / `codec_toml`
  目标）提供 `TomlDocument`、`parseToml`、`TomlOps::INSTANCE`、`dumpToml`；同一批 codec 可直接
  喂 `TomlOps`，`convertTo` 双向可用，`Dynamic`/`Passthrough` 能在格式间搬运。
* **阶段 3 的破坏性变更**（对本仓库以外的 `DynamicOps` 派生类）：`getMap` 从"有默认实现的
  虚函数"变成**纯虚**。仓库内部的 `JsonOps`/`TomlOps` 都已实现；第三方若自己继承过
  `DynamicOps` 并依赖那个默认实现，需要自己写 5 行 `getMap`。
* **阶段 3 的能力边界**（tinytoml v0.4，非本移植）：不支持点号键（引号 `"a.b"` 可以）、
  数组必须同型、不支持本地时间（本地日期会被规范化成 UTC 日期时间）、键按字典序写出
  （因此不是逐字节往返）、无 `null`（写出前校验会拒绝）、codec 错误给的是 codec 路径而不是
  TOML 行列号（tinytoml 不保存值的源码位置，只有解析错误有 `line N`）。

## 7. 风险与对策

| 风险 | 对策 / 判据 |
| --- | --- |
| 热路径变慢 | 已解决：首版 +5.9 %（绝对值），按 §4.2 消除多余所有权流量后回到 **−0.5 %**（15 423 vs 15 500 ns）；阶段 3 复测见 §4.3（隔离解码在布局噪声内，`unboundedMap` 首版的 +20~24 ns/条目已修正），不需要 JSON 旁路 |
| 装箱引入分配 | 已证明为 0（复用 `JsonValue` 的所有者），阶段 1 的读/写路径同样是"1 次分配 + 0 原子" |
| 跨格式误用句柄（静默 UB） | 保留 32 字节标签；`convertTo` 一律经 `outOps` 重建 |
| 临时量生命周期 | §4.1 第 1 条写进规范；考虑让 `getMap`/`getList` 返回共享句柄而不是按值 `vector` |
| 压缩 ops / `KeyCompressor`（NBT 风格） | 键也是 `Value`；阶段 1 后 `dynamic_ops_test` 仍全绿（97 条断言未变） |
| 通用代码里残留 JSON 假设 | 已清零：`getMap` 纯虚 + `isStringKey` 钩子（阶段 3）、4 段散件搬走或泛化（阶段 4，§4.6），核里 `JsonValue`/`nlohmann` 0 命中 |
| 核被某个格式"粘住" | 已清零：核目标不带任何第三方 include 路径；`CODEC_BUILD_JSON=OFF` + 藏掉 nlohmann 的构建 18/18 全绿（§4.6） |
| 格式入口头互相污染 | 入口头只 include `codec.hpp` + 自己的库；交叉格式走 `convertTo`，不靠 include |
| perf 数字本身不可靠 | 阶段 3 证明"隔离解码"的 ±3 % 可能只是代码布局（§4.3）；结论以同源 A/B + 端到端指标为准，README §7 已补这条方法论 |
| 编译时间 | 擦除方案不增加模板实例化，应基本不变（这也是不选 B1 的理由之一） |

## 8. 复现各配置

```powershell
# 全部：核 + JSON + TOML（203 个用例）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure

# 只做 JSON：关掉 TOML 层（181 个用例）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCODEC_BUILD_TOML=OFF

# 只做 TOML：关掉 JSON 层，并把 third_party/nlohmann/json.hpp 改名藏起来
# （脚本会构建、跑 ctest，并在 finally 里还原那个头文件；期望 18/18）
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verify_no_json.ps1
```

阶段 0 原型的微基准（两个只有 `PROTO_TAGGED_HANDLE=0/1` 不同的可执行文件）已随
`prototype/` 删除；需要重跑时从提交 `6b5d523` 取回即可，结论已固化在 §4。

## 9. 收尾记录（阶段 4）

* `prototype/`、`CODEC_BUILD_PROTOTYPE` 选项、本文档里"原型待删"的提示一并移除（结论留在 §4）。
* 验证工具从 gitignored 的 `build/` 收进 `scripts/`：`scripts/assert_audit.js`（证明没有
  削弱断言，可指定修订）、`scripts/verify_no_json.ps1`（无 nlohmann 的构建+测试+自动还原）。
* 新格式的接入规范独立成 [`docs/adding_a_format.md`](adding_a_format.md)——本文档只负责
  "为什么 ops 层是这个形状"。
