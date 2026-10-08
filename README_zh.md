# Codec —— Mojang DataFixerUpper `Codec` API 的 C++17 移植

[English](README.md) | **简体中文**

这是 **DataFixerUpper 6.0.8** 中 `com.mojang.serialization` 包的忠实 C++17 移植，
也就是 Minecraft 用来序列化/反序列化从注册表条目到世界数据的一切内容的
`Codec` / `MapCodec` / `RecordCodecBuilder` / `DynamicOps` / `DataResult` 机制。

参考实现由 `com.mojang:datafixerupper:6.0.8` 反编译而来，通过
[`scripts/decompile_reference.ps1`](scripts/decompile_reference.ps1) 生成到
`reference/dfu-6.0.8/`。这些源码属于 Mojang，仅作阅读参考——不参与构建，也刻意
不纳入版本管理——脚本的作用是让本次移植的来源可复现。

* 仅头文件库：[`include/codec/`](include/codec)
* 分层测试：[`test/unit/`](test/unit)（114 个用例）、[`test/smoke/`](test/smoke)
  （21 个用例）、[`test/perf/`](test/perf)（3 个用例，codec 与 nlohmann/json 的
  性能对比）——每层一个独立可执行文件
* 参考用例（风险定义文档）：[`models/risk_def.hpp`](models/risk_def.hpp)
* 可运行示例：[`examples/risk_def_main.cpp`](examples/risk_def_main.cpp)

```cpp
#include "codec/all.hpp"

struct RiskDef {
  std::string id;
  std::string vid;
  std::vector<VersionRange> affectedVersions;
  std::optional<Condition> condition;
};

Codec<RiskDef> RiskDefCodec = record<RiskDef>(
    fieldOf("id",                &RiskDef::id,                codecs::String),
    fieldOf("vid",               &RiskDef::vid,               codecs::String),
    fieldOf("affected_versions", &RiskDef::affectedVersions,  listOf(VersionRangeCodec)),
    optionalFieldOf("condition", &RiskDef::condition,         ConditionCodec));

// decode / encode
DataResult<RiskDef> decoded = RiskDefCodec.parse(JsonOps::INSTANCE, JsonValue::parse(text));
DataResult<JsonValue> encoded = RiskDefCodec.encodeStart(JsonOps::INSTANCE, value);
```

---

## 1. 依赖与构建

| 依赖 | 版本 | 位置 |
| --- | --- | --- |
| C++ | **C++17**（`/std:c++17`、`-std=c++17`） | 必需 |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0（单头文件） | `third_party/nlohmann/json.hpp` |
| [GoogleTest](https://github.com/google/googletest) | 1.17.0 | `third_party/googletest-1.17.0`（仅测试用） |
| CMake | ≥ 3.16，Ninja 或 MSBuild | 构建 |

```powershell
# 1. 拉取 nlohmann/json 与 GoogleTest（需要代理时加 -Proxy http://127.0.0.1:7890）
powershell -File scripts/fetch_deps.ps1 [-Proxy http://127.0.0.1:7890]

# 2. 构建（通过 vcvars64.bat 使用 MSVC）并运行测试
powershell -File scripts/build.ps1 -RunTests
```

也可以直接使用 CMake：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure   # 全部测试层，138 个用例
cmake --build build --target check           # 等价的一键目标
build/examples/risk_def_example.exe          # 可选：示例文档演示
```

三个测试层是相互独立的可执行文件，因此也可以直接运行：

```powershell
build/test/unit/codec_unit_tests.exe     [--gtest_filter=RecordCodecTest.*]
build/test/smoke/codec_smoke_tests.exe   [--gtest_filter=SmokeTest.*]
build/test/perf/codec_perf_tests.exe     [--gtest_filter=PerfTest.SmallDocument]  # 打印表格
```

**在 CLion 中运行。** 本项目就是普通的 CMake 工程：用 CLion 打开目录并让它完成
配置后，所有用例都会出现在 Run/Debug 下拉框中——`All CTest` 一键运行三个测试层，
`unit.*` / `smoke.*` / `perf.*` 按层分组，每个 `TEST(...)`（包括每个基准测试夹具）
都可单独运行或调试；目标列表里还有 `check` 目标。性能层会自动校准迭代次数，
Release 下约 3.5 秒、Debug 下约 13 秒；`ctest -R perf -V` 可以打印它的表格。

测试套件已在 **MSVC 14.44（VS2022 / CLion）Debug** 与
**MSVC 14.50（VS2026）Release** 下验证通过。

> **工具链提示（MSVC 14.44）。** 含有反斜杠转义的原始字符串字面量不能直接传给
> GoogleTest 宏：MSVC 预处理器在字符串化该实参时会重写 token，转义随后被当作
> 真正的转义重新解释（`R"("\ud83d\ude00")"` 会变成代理对通用字符名 → `C3850`，
> `R"("a\"b")"` → `C2017`）。请先绑定到局部变量：
> `const std::string text = R"(...)"; EXPECT_EQ(f(text), ...)`，参见
> `test/unit/json_test.cpp`。像 `R"({"a":1})"` 这样不含反斜杠的原始字符串不受影响。

CMake 选项：`CODEC_BUILD_TESTS`、`CODEC_BUILD_EXAMPLES`、
`CODEC_WARNINGS_AS_ERRORS`。库目标名为 `codec`（别名 `codec::codec`），链接它并
`#include "codec/all.hpp"` 即可。

### 关于 JSON 值类型

DFU 的 `JsonOps` 基于 Gson 的 `JsonElement`。本移植改用 **nlohmann/json**，具体是
`nlohmann::ordered_json`，从而保证：

* 对象保持插入顺序，与 Gson 中基于 `LinkedTreeMap` 的 `JsonObject` 完全一致；
* 对已存在的成员赋值是**原地替换**（Gson `JsonObject#add` 语义）；
* 解析、序列化与浮点格式化都由 nlohmann 完成。

`JsonValue` 是 `nlohmann::ordered_json` 之上的一层薄封装，并且和 Gson 的
`JsonElement` 一样具有**引用语义**：一个句柄与它来源的文档共享所有权，并指向其中
某个节点，因此复制句柄、读取成员或遍历数组都是 O(1)，绝不会复制整棵 DOM。codec
层保持简洁易读，同时应用侧仍可完全访问底层文档：

```cpp
JsonValue value = nlohmann::ordered_json::parse(text);   // 隐式转换
const nlohmann::ordered_json& raw = value.raw();         // 取回 nlohmann 文档
value.find("id")->asString();
value.get("missing").has_value();                        // false
```

值一旦构造就不可变；请通过 `JsonValue::object(...)`、`JsonValue::array(...)` 或
直接使用 nlohmann 构造新文档。

## 2. 目录结构

```
include/codec/
  json.hpp          JsonValue（底层为 nlohmann）+ Number（对应 java.lang.Number）
  lifecycle.hpp     Lifecycle
  data_result.hpp   DataResult、PartialResult 语义、Unit
  dynamic_ops.hpp   DynamicOps、MapLike、RecordBuilder、ListBuilder、KeyCompressor
  json_ops.hpp      JsonOps（INSTANCE / COMPRESSED）
  codec.hpp         Encoder、Decoder、MapEncoder、MapDecoder、Codec、MapCodec
  codecs.hpp        基础与组合 codec、范围校验、recursive、dispatch
  record_codec.hpp  RecordCodecBuilder：record<>、fieldOf、optionalFieldOf、forGetter
  all.hpp           总头文件
models/risk_def.hpp    风险定义用例（RiskDef/Condition 的 codec）
test/
  CMakeLists.txt       定义各测试层与一键 `check` 目标
  support/             共享测试辅助（test_support.hpp）
  unit/                codec_unit_tests   -- 组件级测试套件
  smoke/               codec_smoke_tests  -- 端到端 API 检查
  perf/                codec_perf_tests   -- codec 与 nlohmann/json 的性能测量
examples/              risk_def_main.cpp
reference/dfu-6.0.8/   反编译得到的 Java 原始代码（仅供阅读）
third_party/           nlohmann/json、googletest
scripts/               fetch_deps.ps1、build.ps1
```

## 3. Java → C++ 对照

| DataFixerUpper（Java） | 本移植（C++） |
| --- | --- |
| `Codec<A>`、`MapCodec<A>`（接口） | 持有 `std::function` 包装的可复制值类型 |
| `Encoder<A>`、`Decoder<A>`、`MapEncoder<A>`、`MapDecoder<A>` | 同样四个包装器，同样的组合子 |
| `Codec.of(encoder, decoder)` / `MapCodec.of(...)` | 同名工厂函数 |
| `MapCodec.MapCodecCodec` | 由 `MapCodec` 构造的 `Codec`（隐式转换，`Codec::mapCodec()` 可识别） |
| `DynamicOps<T>`（`T` = `JsonElement`） | 单一 `JsonValue` 之上的抽象 `DynamicOps` |
| `JsonOps.INSTANCE` / `JsonOps.COMPRESSED` | `JsonOps::INSTANCE` / `JsonOps::COMPRESSED` |
| `DataResult<R>`（`Either<R, PartialResult<R>>`） | `DataResult<R>`（可选值 + 可选错误，保留部分值） |
| `Lifecycle` | `Lifecycle`（`add` 规则一致） |
| `RecordCodecBuilder.create(i -> i.group(f1, f2).apply(i, Ctor::new))` | `record<O>(f1, f2)` 或 `record<O>(ctor, f1, f2)` |
| `Codec.STRING.fieldOf("id").forGetter(RiskDef::id)` | `fieldOf("id", &RiskDef::id, codecs::String)` 或 `codecs::String.fieldOf("id").forGetter(&RiskDef::id)` |
| `Codec.optionalFieldOf("x", default)` | `optionalFieldOf("x", &O::member, codec, defaultValue)` |
| `Codec.listOf()`、`Codec.either(a, b)`、`Codec.pair(a, b)`、`Codec.unboundedMap(k, v)` | `listOf(codec)`、`either(a, b)`、`pair(a, b)`、`unboundedMap(k, v)` |
| `codec.dispatch` / `partialDispatch` / `dispatchMap` | 同名（`KeyDispatchCodec`） |
| `Codec.intRange` / `floatRange` / `doubleRange` | 同名 |
| `Codec.checkRange` | 内联进各范围 codec |
| `DynamicOps` 中的 `Stream<T>` | `std::vector<JsonValue>`（实体化） |
| `Optional<T>` | `std::optional<T>` |
| `Pair<A, B>` | `std::pair<A, B>` |
| `Either<L, R>` | `codec::Either<L, R>`（基于 variant） |
| `Unit`、`PASSTHROUGH`、`EMPTY`、`Codec.unit` | `codec::Unit`、`codecs::Passthrough`、`codecs::Empty`、`Codec<T>::unit` |

已实现的具体内容：基础 codec（`Bool`、`Byte`、`Short`、`Int`、`Long`、`Float`、
`Double`、`String`、`Passthrough`）、`ListCodec`、`EitherCodec`、`PairCodec`、
`UnboundedMapCodec`、`OptionalFieldCodec`、`FieldEncoder`/`FieldDecoder`、
`RecordCodecBuilder`、`KeyDispatchCodec`、`SimpleMapCodec` 的键压缩机制
（`KeyCompressor`、压缩版 record/list 构建器）、`Lifecycle`、`DataResult`
（含 `apply2`/`apply2stable`/`apply3`、`promotePartial`、`setPartial`、
`mapError`、`resultOrPartial`、`getOrThrow`），以及 `Codec`/`MapCodec` 的完整
组合子接口（`xmap`、`flatXmap`、`comapFlatMap`、`flatComapMap`、`orElse`、
`orElseGet`、`mapResult`、`withLifecycle`、`stable`、`deprecated`、`fieldOf`、
`optionalFieldOf`、`promotePartial`）。

不在移植范围内（DFU 中建立在 Codec *之上* 的部分）：`DataFixer`、`Schema`、
`TypeRewriteRule`、optics/profunctor 机制以及 `NbtOps`/`Dynamic` 包装。

## 4. 参考用例

`models/risk_def.hpp` 建模了风险定义文档，包括其递归的条件树：

```cpp
struct Condition {
  std::vector<Condition> orClauses;    // JSON "or"
  std::vector<Condition> andClauses;   // JSON "and"
  std::vector<Condition> notClauses;   // JSON "not"
  std::optional<std::string> param;
  std::optional<std::string> op;
  std::optional<JsonValue> value;      // 任意 JSON 值（Passthrough）
  std::optional<std::string> listMatch;
};

Codec<Condition> conditionCodec() {
  static const Codec<Condition> codec = recursive<Condition>([] {
    return record<Condition>(
        optionalFieldOf("or",  &Condition::orClauses,  listOf(conditionCodec()), std::vector<Condition>{}),
        optionalFieldOf("and", &Condition::andClauses, listOf(conditionCodec()), std::vector<Condition>{}),
        optionalFieldOf("not", &Condition::notClauses, listOf(conditionCodec()), std::vector<Condition>{}),
        optionalFieldOf("param",      &Condition::param,     codecs::String),
        optionalFieldOf("op",         &Condition::op,        codecs::String),
        optionalFieldOf("value",      &Condition::value,     codecs::Passthrough),
        optionalFieldOf("list_match", &Condition::listMatch, codecs::String));
  });
  return codec;
}
```

`recursive<A>(supplier)` 是唯一在 DFU 中没有对应物的补充：Java 通过 datafixer 图
表达递归，而 C++ 在首次使用时才解析 supplier（这同时打破了静态初始化环）。

实测结果（参见 `test/smoke/risk_def_test.cpp` 与示例程序）：

* 示例文档可解码为 `RiskDocument`，包含两个风险项、嵌套的 `or` 条件、叶子
  `list_match` 谓词以及 UTF-8 的 `cn`/`en` 字符串；
* 重新编码与输入文档的紧凑序列化结果**逐字节一致**（record 字段顺序与文档一致）；
* decode → encode → decode 稳定，手工构造的文档同样可以正常往返。

## 5. 行为说明（忠实性）

本移植有意复刻 DFU 的行为，包括它的一些怪癖，并由测试逐条固定下来：

* **Lifecycle** —— `Experimental` 永远优先；`Deprecated(n)` 取最小的 `n`；所有基础
  读取都是 `Experimental`，因此普通 `record<>` 解码上报 `Experimental`
  （可用 `.stable()` 覆盖）。
* **布尔/数字互转** —— `JsonOps.getNumberValue` 接受布尔值，所以 `Codec.INT` 会把
  `true` 解成 `1`，`either(Int, Bool)` 会把 `true` 归到左侧；`Number.intValue()`/
  `byteValue()` 的截断行为与 `java.lang.Number` 完全一致（`1.9 → 1`、`300 → 44`、
  `256 → 0`）。
* **`null` 成员** —— `MapLike.get` 把显式的 JSON `null` 视为"不存在"，而
  `entries()` 仍会报告它（JsonOps 的怪癖）。
* **`OptionalFieldCodec`** —— 存在但非法的可选字段会解成 `std::nullopt` 而不是报错，
  `optionalFieldOf(name, default)` 则静默退回默认值；也就是说可选字段内部的错误会被
  *丢弃*，与 DFU 一致。
* **`ListCodec`** —— 列表失败时的部分值是 `(已解码前缀, 失败的原始元素)` 这一对；
  失败元素自身的部分值也会被并入前缀，与 DFU 的 `Applicative` 链完全一致。
* **错误消息** —— 按字段声明顺序用 `"; "` 连接
  （`"No key a in MapLike[{}]; No key b in MapLike[{}]"`）。
* **压缩 map** —— `JsonOps::COMPRESSED` 把 record 编码成按键索引的列表
  （`["bob",42,["a"],["c","z"]]`），索引顺序由 `KeyCompressor` 从
  `MapCodec::keys` 推导。

有意保留的差异（均已在头文件中注明）：

| 方面 | DFU | 本移植 | 原因 |
| --- | --- | --- | --- |
| `DynamicOps` 值类型 | 泛型 `T`（JsonElement、NbtTag……） | 单一 `JsonValue` | 只有一种 DOM；`convertTo` 即恒等 |
| `Stream<T>` | Java 惰性流 | `std::vector` | 标准库没有惰性流 |
| `KeyCompressor.compress` | 未知键 → `0`（fastutil 默认值） | 未知键 → `-1` → 视为不存在 | 避免静默读取索引 0 |
| `UnboundedMapCodec` 重复键 | `ImmutableMap.Builder` 抛异常 | 后者覆盖，保持插入顺序 | 保证解码可用 |
| `CompressedMapLike` 越界索引 | `IndexOutOfBoundsException` | 视为不存在 | 解码路径中不抛异常 |
| `Encoder.error(msg)` | 追加值的 `toString` | 原样使用消息 | C++ 没有统一的 `toString` |
| `Codec.optionalFieldOf(name, Lifecycle, …)` | 4 参数重载 | 未移植 | 极少使用；`.stable()` 已可覆盖 |

## 6. 测试分层

138 个 GoogleTest 用例分布在三个独立可执行文件中。`ctest` 会为每个用例加上所属层的
前缀（`unit.*`、`smoke.*`、`perf.*`），因此任何一层都可以按组选择运行。

**`test/unit/` → `codec_unit_tests`（114 个用例）**——组件级，覆盖各种边界情况：

| 文件 | 关注点 |
| --- | --- |
| `json_test.cpp` | 基于 nlohmann 的 DOM、解析/输出往返、转义与 Unicode、Gson 相等语义、`Number` 窄化 |
| `nlohmann_interop_test.cpp` | `JsonValue` ⇄ `nlohmann::ordered_json`，直接用 nlohmann 文档喂给 codec |
| `data_result_test.cpp` | `Lifecycle.add` 规则、成功/错误/部分值、`map`/`flatMap`/`apply2`/`apply3`、`promotePartial`、`getOrThrow` |
| `primitives_test.cpp` | `Bool`/`Byte`/`Short`/`Int`/`Long`/`Float`/`Double`/`String`/`Passthrough`、`mergeToPrimitive` |
| `dynamic_ops_test.cpp` | `JsonOps` 基础操作、`mergeToList/Map`、`MapLike` 的 null 规则、构建器、`KeyCompressor`、压缩 map |
| `codec_combinators_test.cpp` | `xmap`/`flatXmap`/`comapFlatMap`/`flatComapMap`、`orElse`、`mapResult`、`either`、`pair`、`listOf`、`unboundedMap`、范围校验、unit codec、map codec 组合子 |
| `record_codec_test.cpp` | 各种形式的 `record<>`、`fieldOf`/`optionalFieldOf`/`forGetter`、错误合并、部分对象、keys、压缩 |
| `dispatch_test.cpp` | `KeyDispatchCodec`（`partialDispatch`/`dispatch`/`dispatchMap`）、map codec 载荷合并、压缩 dispatch |

**`test/smoke/` → `codec_smoke_tests`（21 个用例）**——小而快的端到端检查，回答
"这个移植到底能不能用"：

| 文件 | 关注点 |
| --- | --- |
| `library_smoke_test.cpp` | 用**每一种** codec 类型组成的一份文档做解码/编码/再解码、非法输入拒绝、压缩与普通 ops 的一致性、部分结果 |
| `risk_def_test.cpp` | 参考风险定义文档的端到端验证（逐字节重新编码、递归条件、错误上报） |

**`test/perf/` → `codec_perf_tests`（3 个用例）**——测量而非门槛（唯一的断言是所有
实现路径结果一致）：

| 文件 | 关注点 |
| --- | --- |
| `codec_perf_test.cpp` | 手写 nlohmann 提取器/构建器，与 Codec 层在 2 风险与 400 风险文档上的计时对比；打印第 7 节的表格 |

共享辅助代码位于 `test/support/test_support.hpp`（JSON 解析，以及会把 codec 错误
消息作为失败原因抛出的 `decode` / `encode` / `decodeError` 包装）。

## 7. 性能：Codec 与 nlohmann/json 对比

`test/perf/codec_perf_test.cpp` 用四种方式测量同样的工作并打印表格；运行
`codec_perf_tests.exe` 或 `ctest -R perf -V` 即可复现。基线是
`nlohmann::ordered_json::parse`——也就是本移植自身使用的解析器——因此这些数字展示的
是 Codec 层在解析器*之上*额外付出的代价。manual（手写）一列是用 nlohmann/json 单独
实现时会写出的代码，它刻意**不做**任何校验。

测量环境（本机）：Windows x64、MSVC 14.50、`Release`、5 轮校准后取最优。夹具即第 4 节
的风险定义文档。

**2 个风险项（2 351 字节）**

| 操作 | 耗时 | 相对 `ordered_json::parse` |
| --- | ---: | ---: |
| `nlohmann::json::parse`（无序） | 10.8 µs | 0.85× |
| `nlohmann::ordered_json::parse` | 12.7 µs | 1.00× |
| 解析 + 手写提取 | 14.2 µs | 1.12× |
| **`JsonValue::parse` + codec 解码** | **29.2 µs** | **2.30×** |
| codec 解码（已解析的 `JsonValue`） | 15.6 µs | 1.23× |
| 手写提取（已解析） | 1.4 µs | 0.11× |
| **codec 编码 + dump** | **30.0 µs** | **2.37×** |
| 手写构建 + dump | 13.2 µs | 1.04× |

**400 个风险项（379 611 字节）**

| 操作 | 耗时 | 相对 `ordered_json::parse` |
| --- | ---: | ---: |
| `nlohmann::ordered_json::parse` | 2.48 ms | 1.00× |
| 解析 + 手写提取 | 3.16 ms | 1.28× |
| **`JsonValue::parse` + codec 解码** | **7.43 ms** | **3.00×** |
| codec 解码（已解析的 `JsonValue`） | 4.84 ms | 1.95× |
| 手写提取（已解析） | 0.36 ms | 0.15× |
| **codec 编码 + dump** | **8.32 ms** | **3.36×** |
| 手写构建 + dump | 5.36 ms | 2.16× |

### 结论

1. **一次完整的「文本 → 结构体」解码，耗时是裸 nlohmann 解析的 2.3 倍（小文档）到
   3.0 倍（大文档）。** 解析是下限且占主导，Codec 层大致让它*翻倍*。绝对值上，
   2 个风险项的文档约 29 µs，即单核约 34 000 份/秒。
2. **仅 Codec 层（已解析 DOM）是解析器自身耗时的 1.2–2.0 倍**，是**手写 nlohmann
   提取器的 7–13 倍**。这个倍数换来的是手写提取器完全不做的事情：缺失/多余键检测、
   类型检查、数值范围、能指出出错字段名的 `error()` 消息、部分结果
   （`getOrThrow(allowPartial)`）、`Lifecycle` 追踪，以及可复用的组合能力——递归
   record、`dispatch`、`either`、压缩 map。如果需要这些，每字段 1 µs 很划算；如果
   不需要，直接用 nlohmann 即可。
3. **代价随字段数线性增长**：2 风险文档 0.71 µs/字段，400 风险文档 1.10 µs/字段
   （差异来自 380 KB DOM 的缓存压力）。不存在超线性行为。
4. **编码在小文档上是手写构建的 2.3 倍，而在大文档上只有 1.55 倍**，因为文档一大
   `dump()` 就占主导——文档越大，Codec 层在「编码+序列化」中的占比越小。
5. **`ordered_json` 的解析时间比普通 `json` 多约 15 %**（大文档上约 3 %）。这是插入
   有序对象的代价，而本移植需要它来实现逐字节一致的重新编码以及 Gson 的"原地替换
   成员"语义。
6. **Debug 构建的绝对值约慢 30 倍**（CLion 的默认配置），但比例关系不变：解析器的
   2.5–4.0 倍、手写路径的 6.7–8.3 倍、编码 2.5 倍。任何性能结论都应以 Release 为准，
   Debug 只用于查找缺陷。

### 基准测试发现了什么

写这套对比是值得的——它暴露出两处初版实现**不够忠实于 DFU**（而不仅仅是慢）的地方：

| 问题 | 原因 | 修复 | 效果（400 风险项） |
| --- | --- | --- | --- |
| 每次成员访问都深拷贝 JSON 子树 | `JsonValue` 采用值语义，而 Gson 的 `JsonElement` 是**引用**类型 | `JsonValue` 改为句柄：与文档共享所有权并指向其中一个节点 | 解码 29.4 ms → 4.8 ms |
| 编码是二次方复杂度 | 构建器在每次 `add` 时复制整个累加器，而 DFU 的 `ImmutableList.Builder`/`JsonObject` 是**按引用传递的可变对象** | 构建器状态改为指向可变累加器的 `shared_ptr` | 编码 199 ms → 8.3 ms |

两者都属于设计层面的正确性问题（本移植在规模上表现得不像 DFU），而不是微优化；它们
既被原有测试覆盖，也被新增的 `perf.StrategiesAgreeOnTheFixture` 等价性检查覆盖。
