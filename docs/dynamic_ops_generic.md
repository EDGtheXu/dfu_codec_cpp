# 泛化 ops（方案 B）设计记录

状态：**阶段 0（可行性 + 微基准）已完成**，结论是「可以推进」，实测数字见 §4。
分支：`generic-ops`（`zh-cn` 未受影响）。原型代码在 [`prototype/`](../prototype)：**设计实验，不是库的一部分**，阶段 1 之后应删除。

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

### 3.2 `Dynamic`：值 + 它的 ops

DFU 的 `com.mojang.serialization.Dynamic<T>`。它解决一件事：`Codec.PASSTHROUGH` 需要一个
"原始动态值"的载体（今天这里是 `Codec<JsonValue>`，是 DOM 类型唯一渗进 codec 层的地方）。

```cpp
class Dynamic {
 public:
  const DynamicOps& ops() const;
  const Value& value() const;
  template <class A> DataResult<A> read(const Codec<A>&) const;
  template <class A> DataResult<Dynamic> write(const Codec<A>&, const A&) const;
  DataResult<Dynamic> convertTo(const DynamicOps& outOps) const;
};
```

### 3.3 `convertTo` 从恒等变成真转换

接口里本来就有它（`DynamicOps::convertTo`），`JsonOps` 今天返回原值，
而内部已有递归调用点（`convertList` / `convertMap`）。泛化之后它就是
**JSON ↔ TOML 的桥**：由*源* ops 递归遍历自己的 DOM，逐节点调用*目标* ops 的 `createX`。

## 4. 阶段 0：原型与实测

原型（`prototype/generic_ops.hpp` + `poc_main.cpp`，两个可执行文件）做了四件事：

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
  （`ops.asJson()` → 直接收发 `JsonValue`，绕开装箱），而不是放弃方案。

### 4.1 原型踩到的两个坑（阶段 1 必须写进规范）

1. **不要把 `getMap` 的结果绑到临时量上**：
   `for (auto& e : ops.getMap(doc).value())` 绑定的是临时 `optional` 内部的引用，
   生命周期不延长 → 遍历已释放内存（原型里表现为"解析出全 0"）。
   必须先存局部量。今日的 codec 代码基本都这么做，但接口形状（按值返回 `vector`）会持续诱人犯错，
   阶段 1 应把这条写进注释与 code review 清单。
2. **`convertTo` 必须用 `outOps` 重建键和值**，不能把本 ops 的句柄直接递过去：
   正确做法是 `entries.emplace_back(convertTo(outOps, key), convertTo(outOps, value))`。
   忘了转换在 24 字节句柄下是**静默 UB**，32 字节句柄下才会抛错——这就是保留标签的理由。

## 5. 阶段 1–4（每阶段门禁：174/174 + perf 复测）

改动面（按段量化，`JsonValue` 出现次数）：

| 区域 | 次数 | 处理 |
| --- | ---: | --- |
| 1–3 段 json / lifecycle / data_result | 103 | 不动（`JsonValue` 类、`Number`、`Lifecycle`、`PathSegment`/`Frame`/`report()` 本来就格式无关） |
| 4–5 段 dynamic_ops / json_ops | **329** | 主体：`DynamicOps` 的 40+ virtual、`MapLike`、`ListBuilder`、`RecordBuilder`、`KeyCompressor` 收发 `const Value&` |
| 6–8 段 codec / codecs / record_codec | **143** | `Codec<A>::parse/encodeStart`、`Decoder/Encoder`、`MapCodec`、`DataResult<std::pair<A, Value>>`（`DataResult<…JsonValue…>` 共 121 处） |

| 阶段 | 内容 | 门禁 |
| --- | --- | --- |
| 1 | 4–5 段改签名；`JsonOps` 装箱/拆箱（`asJson()` 便捷入口保留 JSON 快路径）；`JsonValue` 公开 API 不变 | 174 + perf ≤5 % |
| 2 | 6–8 段改签名；`Passthrough` → `Codec<Dynamic>`；`models/risk_def.hpp` 的 `value` 成员跟进 | 174 |
| 3 | `TomlOps`（toml++ 单头文件）实现 ops；「同一 codec 吃 JSON/TOML → 同结构」交叉用例；TOML 行列错误位置 | 新增用例 |
| 4 | 文档：两份 README 的「值类型」章节、格式支持矩阵、§7 性能重测 | — |

另外阶段 1 顺带补两件今天缺的东西：

* `DynamicOps::toString(const Value&)`：错误消息里的 `MapLike[...]` 现在硬编码 JSON 渲染，
  泛化后必须由 ops 决定（否则 TOML 用户看到的是 JSON 原文）。
* `Value` 的公开构造入口（`JsonValue` → `Value` 的隐式转换），让
  `codec.parse(JsonOps::INSTANCE, JsonValue::parse(text))` 这类现有写法不用改。

## 6. 用户可见影响

* **不变**：`Codec<A>`、`record/fieldOf/optionalFieldOf/listOf/unboundedMap/either/pair/dispatch`、
  错误定位 `risks[3].condition.or[0].op`、可点击栈帧、`CodecError`/`throwIfError`。
* **要改的**：`Codec.PASSTHROUGH` 的字段类型 `JsonValue` → `codec::Dynamic`
  （`models/risk_def.hpp` 的 `std::optional<JsonValue> value`）。
* **新增**：`TomlOps::INSTANCE` 可直接喂给同一批 codec；`Dynamic::convertTo` 在格式间搬运。

## 7. 风险与对策

| 风险 | 对策 / 判据 |
| --- | --- |
| 热路径变慢 | 阶段 0 实测 +3~5 %；阶段 1 用真实 perf 层复测，超标则给 `JsonOps` 加 JSON 快路径（`ops.asJson()` → 直接收发 `JsonValue` 的旁路） |
| 装箱引入分配 | 已证明为 0（复用 `JsonValue` 的所有者）；阶段 1 保留这个断言作为回归 |
| 跨格式误用句柄（静默 UB） | 保留 32 字节标签；`convertTo` 一律经 `outOps` 重建 |
| 临时量生命周期 | §4.1 第 1 条写进规范；考虑让 `getMap`/`getList` 返回共享句柄而不是按值 `vector` |
| 压缩 ops / `KeyCompressor`（NBT 风格） | 键也是 `Value`；`dynamic_ops_test` 必须继续全绿 |
| 编译时间 | 擦除方案不增加模板实例化，应基本不变（这也是不选 B1 的理由之一） |

## 8. 复现

```powershell
cmake -S . -B build-poc -G Ninja -DCMAKE_BUILD_TYPE=Release -DCODEC_BUILD_PROTOTYPE=ON
cmake --build build-poc
build-poc/prototype/codec_generic_ops_poc.exe          # 24 字节句柄
build-poc/prototype/codec_generic_ops_poc_tagged.exe   # 32 字节句柄
```

两个可执行文件代码完全相同，只有 `PROTO_TAGGED_HANDLE=0/1` 不同，用来隔离"多 8 字节"的代价。

## 9. 原型退役

阶段 1 落地后，`prototype/`、`CODEC_BUILD_PROTOTYPE` 选项和本文档的 §4 应一并删除
（结论可并入 README 的架构章节）。
