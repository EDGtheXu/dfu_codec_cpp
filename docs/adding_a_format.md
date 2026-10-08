# 加一种格式（ops 入口头契约）

本库按「**每种 `DynamicOps` 一个入口头**」组织，include 即用：

| 入口头 | 内容 | 依赖 |
| --- | --- | --- |
| `include/codec.hpp` | **核**：擦除值句柄 `Value`、`DynamicOps`/`MapLike`/构造器接口、`Codec`/`MapCodec` 层、`DataResult`/`Lifecycle`、错误定位与 `Number` | 只有标准库 |
| `include/codec_json.hpp` | JSON：`JsonValue`（引用语义句柄）、`JsonParseError`、`JsonOps`（`INSTANCE` / `COMPRESSED`） | 核 + nlohmann/json 单头文件 |
| `include/codec_toml.hpp` | TOML：`TomlDocument`、`parseToml`、`TomlOps`、`dumpToml` | 核 + tinytoml 单头文件 |

本文是**新格式入口头的契约清单**；`include/codec_toml.hpp` 是照此写出来的范例（含注释里的四张表），
`docs/dynamic_ops_generic.md` 记录了为什么 ops 层是"擦除值 + 纯虚接口"这个形状。

---

## 0. 一句话结论

接一种格式 = **再写一个 `DynamicOps`** + 一个入口头 + 三份测试。核一行都不用改，
（只要你想让"没有该格式库"的构建继续可编译）测试也要做到**不依赖其它格式**。

---

## 1. 只 include 什么

```cpp
#include "codec.hpp"          // 核（唯一必须的库内头文件）
#include <toml/toml.h>        // 该格式自己的库（这里以 tinytoml 为例）
```

* **不要** include 别的格式入口头（`codec_json.hpp` / 其它 `codec_<fmt>.hpp`）；
  交叉格式的桥是 `DynamicOps::convertTo`，不需要在编译期见到对方。
* **绝不要**让核 include 你：`codec.hpp` 里出现任何格式的名字都是缺陷
  （仓库用 grep 断言守着：核里 `JsonValue` / `nlohmann` 出现次数必须为 0）。
* 第三方库的头文件若会产生与项目无关的警告（例如 tinytoml 用了 `gmtime`），
  用 `#pragma warning(push)/(disable:4996)/(pop)` **局部**包住 include，不要用全局 `/wd`。

## 2. 必须提供的入口

在 `namespace codec` 里：

| 入口 | 作用 | 参考实现 |
| --- | --- | --- |
| `class XxxOps : public DynamicOps` | 该格式的 ops | `TomlOps` / `JsonOps` |
| `static const XxxOps INSTANCE;`（行内变量定义在头文件末尾） | 单例，直接喂给 codec | `inline const TomlOps TomlOps::INSTANCE{};` |
| `XxxDocument`（可选） | 持有解析结果的**不可变 DOM**（与装箱句柄共享所有权） | `TomlDocument` |
| `parseXxx(std::string_view) -> DataResult<XxxDocument>`（可选） | 文本 → DOM；失败时把库自己的错误文本带出来（含行号） | `parseToml` |
| `dumpXxx(const Value&) -> DataResult<std::string>`（可选） | DOM → 文本；**写出前统一校验**（见 §5） | `dumpToml` |
| 与核的互操作 | 见 §4，把"格式句柄 → `Value`"和"`Value` → 格式句柄"两件事放在入口头里 | `JsonValue::operator Value()` + `jsonView` |

`DynamicOps` 的纯虚方法必须**全部**实现（编译器会替你检查）：`empty`、`createBoolean`、
`createNumeric`、`createString`、`createList`、`createMap`、`getNumberValue`、
`getBooleanValue`、`getStringValue`、`getStream`、`getMap`、`getMapValues`、
`mergeToList`（两个重载）、`mergeToMap`（两个重载）、`remove`、`toString(const Value&)`、
`convertTo`。按需覆写带默认实现的方法：`isStringKey`、`valueEquals`、`listBuilder`、
`mapBuilder`、`compressMaps`、`getList`。逐条语义照 `DynamicOps` 的注释来——
它们是 DFU 的行为，不是本移植的自由发挥。

## 3. 头注释里必须写清的四张表

`codec_toml.hpp` 的文件头就是范例（照着填，别省）：

1. **类型映射**：格式的每个值类型 ↔ `DynamicOps` 视图 ↔ 怎么读（哪个 `getXxxValue`）、
   写回时变成什么。有损映射（例如 TOML 的日期时间只能当字符串）必须标出来。
2. **与 `JsonOps` 的按格式差异**：哪些差别是**格式本身**决定的，不是 bug。
   例如 TOML 的布尔读不出数字、`valueEquals` 类型严格（`1 != 1.0`）、键按直接子节点查找。
3. **已知限制**：该库版本表达不了/读不回的东西（TOML v0.4：点号键、混型数组、本地时间、
   `null`、键排序导致的非逐字节往返、不保存源码位置所以 codec 错误给不出行列）。
   写清楚"这是库的边界，不是移植的缺陷"，读者才不会把它当 bug 报。
4. **写出前置校验**：`createX` 是**全函数**（返回 `Value`，报不了错），所以"这份数据写不出"
   只能在 `dumpXxx` 里一次说清。列出你校验的每一条（根必须是表、不得有 `null`、数组同型……）。

另外在头注释里写**编码复杂度**：你的构造器是不是线性的、`mergeToList`/`mergeToMap` 的代价。

## 4. 装箱与核的互操作

* 装箱：`Value::of<FormatNode>(std::shared_ptr<const FormatNode>, const FormatNode*)`——
  按值 + move 的重载**零引用计数操作**；只借用外层所有者换子节点时用
  `Value::ofChild(owner, node, tagOf<FormatNode>())`（一次原子）。标签是
  `&tagOf<T>()`，跨格式误用会变成明确的 `nullptr`/异常，而不是静默 UB。
* **不要**在核里加"格式 → `Value`"的构造函数，也不要用 `#ifdef` 条件包含：
  把隐式转换写成格式句柄的**成员转换运算符**（`XxxValue::operator Value() const`），
  这样 `codec.parse(XxxOps::INSTANCE, XxxValue::parse(text))` 这种写法继续可用，
  而核里不出现格式的名字。
* 反方向写一个**自由函数**（例如 `jsonView(const Value&)`），标签不匹配时抛
  `std::logic_error`。核里没有"取出某种格式节点"的逃生口，这是故意的。

## 5. 两条最容易踩的坑

1. **不可变 DOM 必须覆写 `listBuilder()` / `mapBuilder()`.**
   基类默认给的是通用构造器（`UniversalListBuilder` / `UniversalRecordBuilder`），它们
   **逐元素**调 `mergeToList` / `mergeToMap`。对深拷贝的值类型（`toml::Value`、JSON 节点），
   这两个方法每次调用都要复制整个已累积容器 ⟹ **编码变成 O(N²)**。
   正确做法是照 `TomlListBuilder` / `TomlRecordBuilder` 写一对 mutable 累加器：
   状态放 `shared_ptr` 里，`add` 只追加句柄，`build(prefix)` 一次性成型。
   （这不是理论风险：TOML 层第一版就是这么慢的，400 个风险项 248 ms → 4.2 ms。）
2. **`MapLike::get` 的"缺失"语义要和你的格式对齐**，并在头注释里说明。
   TOML 侧用 `findChild`（点号是字面量）并把 `NULL_TYPE` 当缺失；JSON 侧把显式 `null`
   当缺失。同一份 codec 代码在两种语义下都应该给出"缺失"而不是崩溃。

**自检**：`perf` 层打印的 `per-risk encode` 在两种规模的夹具上应该基本持平（缓存效应除外）。
数字随文档规模线性上涨以外的大幅跳变，基本就是踩了第 1 条。

## 6. CMake 怎么挂

```cmake
option(CODEC_BUILD_XXX "Build the XXX entry header (include/codec_xxx.hpp) and its tests" ON)
if(CODEC_BUILD_XXX)
  # 库存在性检查放在这里，别放在核上：核不需要这个库
  add_library(codec_xxx INTERFACE)
  add_library(codec::xxx ALIAS codec_xxx)
  target_link_libraries(codec_xxx INTERFACE codec)
  target_include_directories(codec_xxx INTERFACE "…/third_party/xxx")
endif()
```

* **核目标只带 `include/`**；第三方 include 目录挂在各格式目标上。
* 测试目标：`codec_xxx_unit_tests`、`codec_xxx_smoke_tests`、`codec_xxx_perf_tests`
  （各一个可执行文件，CTest 前缀 `xxx_unit.` / `xxx_smoke.` / `xxx_perf.`），
  并加进 `check` 目标的 `DEPENDS`。
* 测试若**本质上**要第二种格式做对照（交叉格式冒烟），把它挂在 `if(CODEC_BUILD_JSON)`
  之类的组合条件下；**其余测试要做到不依赖别的格式**：这样"只有核 + 一种格式"的配置
  （见 README 的 `-DCODEC_BUILD_JSON=OFF`）仍然有真实覆盖。
* 用 CMake 的 `target_compile_definitions(<测试目标> PRIVATE CODEC_TEST_WITH_JSON)`
  包住少数必须有第二种格式的断言，而不是让整个文件依赖它。
* 格式测试的断言用**该格式自己的** `dumpXxx` / `ops.toString`，不要借 JSON 的 `dumpJson`。

## 7. 要写哪些测试

| 层 | 写什么 | 范例 |
| --- | --- | --- |
| unit | 类型映射逐条、按格式差异（布尔/相等/键查找）、`convertTo` 同 ops 恒等、写出前校验的每个拒绝分支、解析错误带行号；**以及构造器**：前缀并入不改前缀、累加器复用、重复键 last-wins、`add`/`withErrorsFrom`/`mapError` 的错误传播、非字符串键、异格式句柄 | `test/unit/toml_ops_test.cpp` |
| smoke | **同一个 codec** 吃两种格式得到相同结构、dump → 重新解析稳定、`Passthrough` 的 `Dynamic` 跨格式搬运 | `test/smoke/toml_risk_def_test.cpp` |
| perf | `parseXxx`、解析+解码、已解析解码、编码、编码+dump、`convertTo` 双向，**逐规模成本**（`per-risk encode` 是线性守卫） | `test/perf/toml_perf_test.cpp` |

perf 层**只测量、不断言时间**：唯一允许的断言是"各条路径结果一致"。

## 8. 检查清单

- [ ] 核里没有出现任何该格式的名字（grep 计数为 0），核也没 include 这个入口头
- [ ] 只 include `codec.hpp` + 该格式自己的库
- [ ] `DynamicOps` 的纯虚方法全部实现，默认方法按需覆写并注释理由
- [ ] 头注释里有四张表 + 编码复杂度说明
- [ ] 不可变 DOM 覆写了 `listBuilder()` / `mapBuilder()`
- [ ] `dumpXxx` 在写出前校验，并在测试里覆盖每条拒绝分支
- [ ] 互操作写成成员转换运算符 + 自由函数，没有 `#ifdef` 条件包含
- [ ] CMake：独立的 `codec_xxx` INTERFACE 目标（核不带第三方 include 路径）
- [ ] 测试：unit + smoke + perf 三份，perf 有逐规模成本；能 JSON-free 的就 JSON-free
- [ ] 文档：README 的入口头说明/目录树/依赖表/测试计数，以及阶段表里加一行
