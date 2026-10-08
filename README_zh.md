# Codec —— Mojang DataFixerUpper `Codec` API 的 C++17 移植

[English](README.md) | **简体中文**

这是 **DataFixerUpper 6.0.8** 中 `com.mojang.serialization` 包的忠实 C++17 移植，
也就是 Minecraft 用来序列化/反序列化从注册表条目到世界数据的一切内容的
`Codec` / `MapCodec` / `RecordCodecBuilder` / `DynamicOps` / `DataResult` 机制。

参考实现由 `com.mojang:datafixerupper:6.0.8` 反编译而来，通过
[`scripts/decompile_reference.ps1`](scripts/decompile_reference.ps1) 生成到
`reference/dfu-6.0.8/`。这些源码属于 Mojang，仅作阅读参考——不参与构建，也刻意
不纳入版本管理——脚本的作用是让本次移植的来源可复现。

* 单头文件库：[`include/codec.hpp`](include/codec.hpp)——一个文件、约 4 400 行，CMake `INTERFACE` 目标，无需编译任何源文件
* 头文件内的注释为**中文**；API 名、错误消息、测试名与两份 README 保持中英各自原本的语言
* 分层测试：[`test/unit/`](test/unit)（148 个用例）、[`test/smoke/`](test/smoke)
  （23 个用例）、[`test/perf/`](test/perf)（3 个用例，codec 与 nlohmann/json 的
  性能对比）——每层一个独立可执行文件
* 参考用例（风险定义文档）：[`models/risk_def.hpp`](models/risk_def.hpp)
* 可运行示例：[`examples/risk_def_main.cpp`](examples/risk_def_main.cpp)

```cpp
#include "codec.hpp"

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
| C++ | **C++17**（`/std:c++17`、`-std=c++17`） | 必需（C++20/23 可解锁下面可选的诊断能力） |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0（单头文件） | `third_party/nlohmann/json.hpp` |
| [GoogleTest](https://github.com/google/googletest) | 1.17.0 | `third_party/googletest-1.17.0`（仅测试用） |
| CMake | ≥ 3.16，Ninja 或 MSBuild | 构建 |

库本身是 header-only，没有什么需要配置的；但由于头文件注释是中文，**在这个工程的
CMake 目标之外**直接用 MSVC 编译时需要 `/utf-8`（或 `/source-charset:utf-8`）——
`codec` 目标已经加上了。

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
ctest --test-dir build --output-on-failure   # 全部测试层，174 个用例
cmake --build build --target check           # 等价的一键目标
build/examples/risk_def_example.exe          # 可选：示例文档演示

# 可选：为每个错误额外抓取真正的 std::stacktrace（会把构建切到 C++23）
cmake -S . -B cmake-build-stacktrace -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCODEC_RECORD_STACKTRACE=ON
```

| CMake 选项 | 默认 | 作用 |
| --- | --- | --- |
| `CODEC_BUILD_TESTS` | `ON` | 构建三个测试层 |
| `CODEC_BUILD_EXAMPLES` | `ON` | 构建示例程序 |
| `CODEC_WARNINGS_AS_ERRORS` | `OFF` | `/WX`、`-Werror` |
| `CODEC_RECORD_STACKTRACE` | `OFF` | 定义 `CODEC_RECORD_STACKTRACE`、按 C++23 编译，并为每个错误抓取 `std::stacktrace` |

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
`#include "codec.hpp"` 即可。

### 仅头文件，且只有一个文件

整个库就是 [`include/codec.hpp`](include/codec.hpp)：没有翻译单元、没有生成文件，
CMake 目标是 `INTERFACE` 库，因此不需要编译、链接、安装，也不存在 ABI 兼容问题。
接入成本就是一条 `target_link_libraries`：

```cmake
add_subdirectory(path/to/Codec)                     # 或用 FetchContent_Declare(...)
target_link_libraries(my_app PRIVATE codec::codec)  # 附带 include/、third_party/ 与 C++17
```

文件内部按依赖顺序划分为 8 个带注释的小节——`json`、`lifecycle`、`data_result`、
`dynamic_ops`、`json_ops`、`codec`、`codecs`、`record_codec`——因此依然便于导航
（搜索 `// 3/8  data_result` 即可跳转）；要把库拷进别的工程，只需要这一个文件加上
nlohmann/json。

命名空间作用域下的一切要么是 `inline` 函数、要么是模板、要么是 inline 变量
（`JsonOps::INSTANCE`、`codecs::Int` 等），因此可以放心地在任意多个翻译单元里包含这个
头文件，单例也确实是共享的：

* `test/unit/odr_probe.cpp` 是第二个翻译单元，它包含了这个头文件（外加
  `models/risk_def.hpp`），因此单元测试可执行文件会把所有定义链接两遍——漏写
  `inline` 会直接在链接期失败；
* `test/unit/odr_test.cpp` 会比较两个翻译单元里看到的 codec 与 `JsonOps::INSTANCE`
  地址，从而抓住"某个定义悄悄变成翻译单元内部链接"的情况。把 `codecs::Int` 前的
  `inline` 改成 `static`，这两个测试都会失败；
* `test/unit/header_self_contained_test.cpp` 在**任何其他头文件之前**（包括
  GoogleTest）包含 `codec.hpp`，因此这个头文件无法从别处借用任何声明；
* 另有 8 个翻译单元只包含它这一个库头文件。

唯一的编译期依赖是 [nlohmann/json](#关于-json-值类型)（`third_party/` 下的单头文件，
也可以换成你自己 include 路径上的副本）；测试额外需要 GoogleTest，而 `INTERFACE`
目标不会把它传递出去。

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
include/codec.hpp      整个库，按依赖顺序分为 8 个带注释的小节：
                         1 json          JsonValue（底层为 nlohmann）+ Number（对应 java.lang.Number）
                         2 lifecycle     Lifecycle
                         3 data_result   DataResult、PartialResult 语义、Unit
                         4 dynamic_ops   DynamicOps、MapLike、RecordBuilder、ListBuilder、KeyCompressor
                         5 json_ops      JsonOps（INSTANCE / COMPRESSED）
                         6 codec         Encoder、Decoder、MapEncoder、MapDecoder、Codec、MapCodec
                         7 codecs        基础与组合 codec、范围校验、recursive、dispatch
                         8 record_codec  RecordCodecBuilder：record<>、fieldOf、optionalFieldOf、forGetter
models/risk_def.hpp    风险定义用例（RiskDef/Condition 的 codec）
test/
  CMakeLists.txt       定义各测试层与一键 `check` 目标
  support/             共享测试辅助（test_support.hpp）
  unit/                codec_unit_tests   -- 组件级测试套件（含 ODR 守护）
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

### 标量转换（enum、数字、字符串）

库里没有单独的"枚举 codec"或"字符串数字 codec"：所有组合都由同样的两个组合子搭出来，
因此可以和字段、列表、`dispatch`、可选字段等自由组合。

| JSON | C++ | 写法 |
| --- | --- | --- |
| 数字 | `enum class` | `Int.flatXmap<E>(toEnum, toInt)`（映射不会失败时也可用 `Int.xmap<E>`） |
| 字符串 | `enum class` | `codecs::stringEnum<E>({{"low", E::Low}, ...}, "E")`，或手写 `String.flatXmap<E>` |
| 字符串 | 数字 | `String.flatXmap<int32_t>(parse, toString)`——严格解析建议用 `std::from_chars` |
| 数字 | `std::string` | `Int.xmap<std::string>(std::to_string, ...)`（JSON 里仍是数字） |
| 数字*或*字符串 | 数字 | `either(Int, String).flatXmap<int32_t>(...)`——`8080` 与 `"8080"` 都接受，编码时统一输出数字 |

`test/unit/string_and_enum_test.cpp` 是上面五行可直接运行的示例集，包含错误消息
（`Unknown Severity: "fatal"`、`Not a number: "80x"`）以及 `optionalFieldOf` 对
`null` 或非法枚举名的处理方式。

### 错误定位

DFU 的消息只说*出了什么问题*，从不指出*在哪里*：文档深处的一个坏值只会报
`Not a string: 1`。本移植在消息之外记录位置，于是报错读起来像编译器/校验器的诊断：

```cpp
const DataResult<RiskDocument> result = riskDocumentCodec().parse(JsonOps::INSTANCE, input);

result.isError();     // true
result.message();     // "Not a string: 1"                                    （DFU 原文）
result.location();    // "risks[3].condition.or[0].op"
result.describe();    // "risks[3].condition.or[0].op: Not a string: 1"        （单行上报用这个）
result.report();      // 同上，并附上处理该值的 codec 调用链（见下）
result.errors();      // {ErrorPart{path, frames, message}}——每处失败一项
```

`report()` 是完整诊断：首行与 `describe()` 相同，之后每个经手的 codec 一行
`  in <codec>`，由内向外排列。

```text
risks[3].condition.or[0].op: Not a string: 1
  in String
  in RecordCodec[op]
  in list
  in RecordCodec[or]
  in optional[condition]
  in RecordCodec[condition]
  in list
  in RecordCodec[risks]
```

* `message()` 与 DFU **逐字节一致**（把各个局部消息用 `"; "` 连接），这也是新增路径
  不改变任何既有行为的原因。
* `describe()` 为每处失败加上位置前缀，例如
  `a: No key a in MapLike[{}]; b: No key b in MapLike[{}]`；`location()` 只在所有
  失败部分位置一致时返回路径，否则返回 `""`。
* 位置由容器在错误向外传播时逐层附加：`fieldOf` 加键名、`ListCodec` 加 `[i]`、
  `unboundedMap` 加条目键、`dispatch` 为非 map 载荷加 `value`；**编码侧**由 record/list
  构建器加字段名或下标（`small: too large to encode: 200`）。
* 调用链同理，每个容器一次 `addFrame`。栈帧存在各 `ErrorPart` 上，因此多处失败会各打印
  一段。名字用的是每个 codec **自己的**短名（`Int`、`list`、`RecordCodec[a, b]`、
  `optional[n]`、`dispatch[type]`、`unboundedMap`、`either`、`pair`），而不是容器那个
  组合名（`ListCodec[...]`），所以嵌套 record 不会自我重复。
* 两项标注都**只在错误向外传播时才构造**，所以成功解码既不会分配路径段、也不会分配
  帧字符串；§7 的基准测试显示与"无标注"版本的差异在噪声范围内。
* `mapError` 在所有失败部分路径/栈帧一致时会保留它们——`unboundedMap` 的
  `missed input:` 改写和 `mapResult` 的措辞改写正因此仍带位置。不一致时两者都会丢弃，
  而不是猜一个。
* `promotePartial`、`resultOrPartial`、`getOrThrow` 会把带位置的文本交给 `onError`
  回调——这些回调本来就是用于诊断的。
* 想换成别的措辞（例如 `expected string, got number`）？用 `Codec::mapResult` +
  `DataResult::mapError` 包一层叶子 codec，位置会保留；见
  `ErrorPathTest.WordingCanBeRewrittenWhileKeepingTheLocation`。
* 路径与栈帧属于**新增能力**（DFU 都没有）。`test/unit/error_path_test.cpp` 覆盖了格式、
  嵌套列表、多处失败、缺失键、无界 map、dispatch 载荷与编码侧（`FrameTest` 管调用链，
  `ErrorPathTest` 管位置）。

**可点击的栈帧，且跨平台。** 每一帧还带「构造该 codec 的位置」，因此 `report()` 同时也
是一份能在 IDE 里点开的调用栈：

```text
risks[3].condition.or[0].op: Not a string: 1
  0> D:\programming\cpp\Codec\include\codec.hpp(3445): String
  1> D:\programming\cpp\Codec\models\risk_def.hpp(116): optional[op]
  2> D:\programming\cpp\Codec\models\risk_def.hpp(109): RecordCodec[or, and, not, param, op, value, list_match]
  ...
```

* 位置来自三级 `SourceLocation`：标准库有 `std::source_location`（C++20）就用它，否则用
  MSVC/GCC/Clang 在 C++17 下都支持的 `__builtin_FILE()`/`__builtin_LINE()`，两者都没有
  就退化为「没有位置」。代码永远能编译，降级的只是诊断信息。
* 它是编译期字面量：不分配内存、不需要调试信息（PDB/DWARF），因此 Release 下同样可用。
* `report(FrameStyle::native | msvc | gnu)` 选择排版：MSVC 的 `file(line):` 形式（与它自家
  `<stacktrace>` 输出同形），或 gdb 风格的 `#0 name at file:line`。CLion 两种都能点开，
  `native` 跟随编译器。模型里构造的帧指向你的 `record<...>`/`fieldOf(...)` 那一行，
  库内部的帧指向 `codec.hpp`。

**用抛异常代替返回。** 不想检查 `DataResult` 时：

```cpp
try {
  RiskDocument document = riskDocumentCodec().parse(JsonOps::INSTANCE, input).throwIfError();
} catch (const codec::CodecError& error) {
  error.what();       // "risks[3].condition.or[0].op: Not a string: 1"（单行）
  error.report();     // 同上，再加带位置的 codec 调用链
  error.location();   // "risks[3].condition.or[0].op"
  error.errors();     // 各失败部分（路径 + 帧 + 消息）
  error.hasPartial(); // 是否带着可用的部分值
}
```

* `CodecError` 派生自 `std::runtime_error`，因此既有的 `catch (const std::exception&)`
  继续有效；DFU 的 `getOrThrow(allowPartial, onError)` 现在也抛它。
* 不需要编译器内建、不需要 C++20/23、也不需要调试信息——这是传播诊断信息最跨平台的
  方式；在任何调试器里对 `CodecError` 下断点都能看到真正的调用栈。

**标准库提供时，还能给出真正的调用栈。** 加上 `-DCODEC_RECORD_STACKTRACE=ON`（会把构建
切到 C++23）后，错误产生的那一刻会抓取 `std::stacktrace` 并附在 `report()` 之后：

```text
[1]: Not a number: "x"
  0> D:\programming\cpp\Codec\include\codec.hpp(3445): Int
  1> D:\programming\cpp\Codec\build\zh\print_stacktrace.cpp(16): list
  stacktrace:
    0> D:\programming\cpp\Codec\include\codec.hpp(1074): demo!codec::DataResult<codec::Number>::makeErrorPart+0x66
    ...
    21> D:\programming\cpp\Codec\build\zh\print_stacktrace.cpp(17): demo!main+0x111
```

* 排版由标准库自己完成，因此天然就是你工具链 IDE 能解析的格式；带调试信息时
  （MSVC `/Zi`，即 Debug 构建）每帧都有 `file(line)`，libstdc++/libc++ 则是 `file:line`
  ——**包括调用 `parse()` 的那一行**，这是 codec 调用链本身给不出的信息。
* 默认关闭：抓栈对*每个*错误都要付费，包括 DFU 有意吞掉的「存在但非法的可选字段」。
  没有 C++23 时头文件仍是普通的 C++17 头文件，这一层根本不存在
  （`test/unit/stacktrace_test.cpp` 会跳过自己）。

**严格可选字段。** 路径只有在错误不被吞掉时才有意义，而 DFU 的 `OptionalFieldCodec`
恰恰会吞掉"存在但非法"的可选值。做校验时你需要那个错误，因此本移植提供了会传播错误的
对应版本：

| | `{"n": 1}`，codec 为 `Codec<optional<int>>` | 适用场景 |
| --- | --- | --- |
| `optionalFieldOf`（DFU） | 解成 `nullopt` | 宽松加载你无法控制的数据 |
| `optionalFieldOfStrict` | 失败：`n: Not a number: 1` | 校验器——坏值不能看起来像"不存在" |

`optionalFieldOfStrict(name, codec)`、`optionalFieldOfStrict(name, codec, default)` 以及
record 字段版本（`optionalFieldOfStrict(name, &O::member, codec[, default])`）与宽松版
一一对应。参考模型的风险条件子树使用的就是严格版本：静默丢弃一条非法的安全规则，等于把
"这条规则坏了"变成"这条规则不适用"。

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
* decode → encode → decode 稳定，手工构造的文档同样可以正常往返；
* 非法规则会带上准确位置和处理它的 codec 调用链，每一帧还带构造它的那一行——
  `risk_def_example.exe bad.json` 输出（CLion 里每个 `文件(行号)` 都可点击）：

  ```text
  risks[0].condition.or[0].op: Not a string: 1
    0> D:\programming\cpp\Codec\include\codec.hpp(3477): String
    1> D:\programming\cpp\Codec\models\risk_def.hpp(116): optional[op]
    2> D:\programming\cpp\Codec\models\risk_def.hpp(109): RecordCodec[or, and, not, param, op, value, list_match]
    3> D:\programming\cpp\Codec\models\risk_def.hpp(109): list
    4> D:\programming\cpp\Codec\include\codec.hpp(3326): optional[or]
    5> D:\programming\cpp\Codec\models\risk_def.hpp(109): RecordCodec[or, and, not, param, op, value, list_match]
    6> D:\programming\cpp\Codec\models\risk_def.hpp(134): optional[condition]
    7> D:\programming\cpp\Codec\models\risk_def.hpp(127): RecordCodec[id, vid, risk_type, severity, name, description, solution, condition, evidence]
    8> D:\programming\cpp\Codec\models\risk_def.hpp(141): list
    9> D:\programming\cpp\Codec\models\risk_def.hpp(141): RecordCodec[risks]
  ```

  条件树是递归的，所以同一个 `RecordCodec` 会在两层出现。模型使用严格可选字段，
  因此坏掉的条件不会被静默丢弃（见第 3、5 节）。

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

有意保留的差异与补充（均已在头文件中注明）：

| 方面 | DFU | 本移植 | 原因 |
| --- | --- | --- | --- |
| `DynamicOps` 值类型 | 泛型 `T`（JsonElement、NbtTag……） | 单一 `JsonValue` | 只有一种 DOM；`convertTo` 即恒等 |
| `Stream<T>` | Java 惰性流 | `std::vector` | 标准库没有惰性流 |
| `KeyCompressor.compress` | 未知键 → `0`（fastutil 默认值） | 未知键 → `-1` → 视为不存在 | 避免静默读取索引 0 |
| `UnboundedMapCodec` 重复键 | `ImmutableMap.Builder` 抛异常 | 后者覆盖，保持插入顺序 | 保证解码可用 |
| `CompressedMapLike` 越界索引 | `IndexOutOfBoundsException` | 视为不存在 | 解码路径中不抛异常 |
| `Encoder.error(msg)` | 追加值的 `toString` | 原样使用消息 | C++ 没有统一的 `toString` |
| `Codec.optionalFieldOf(name, Lifecycle, …)` | 4 参数重载 | 未移植 | 极少使用；`.stable()` 已可覆盖 |
| `codec::recursive<A>(supplier)`（**新增**） | —（Java 通过 datafixer 图表达递归） | 提供 | 首次使用时才解析 supplier，同时打破静态初始化环 |
| `codecs::stringEnum<E>(table, name)`（**新增**） | —（MC 用 `StringRepresentable.fromEnum`，它不在 DFU 里） | 提供 | 用名字表处理枚举、免去样板代码；基于 `flatXmap` 实现，因此可像其他 codec 一样组合 |
| `DataResult::location()` / `describe()` / `report()`（**新增**） | 任何地方都没有位置信息 | 每个字段/元素/map 条目都附加路径，每个 codec 附加自己的名字与构造位置 | 消息保持与 DFU 一致；`describe()` 上报 `risks[3].condition.or[0].op: Not a string: 1`，`report()` 再附上带可点击 `file(line)` 帧的 codec 调用链 |
| `SourceLocation` + `report(FrameStyle)`（**新增**） | — | 依次尝试 `std::source_location` / `__builtin_FILE/LINE` / 无 | 跨平台的可点击栈帧，不需要调试信息；缺能力时降级而不是编译失败 |
| `DataResult::throwIfError()` / `codec::CodecError`（**新增**） | `getOrThrow` 抛 `RuntimeException` | 携带位置、帧、各部分与部分值标志的 `std::runtime_error` 子类 | 跨平台的「抛而不是返回」，仍可按 `std::exception` 捕获 |
| `CODEC_RECORD_STACKTRACE`（**新增**） | 没有任何栈回溯 | 失败时抓取 `std::stacktrace`，由标准库排版 | 仅 C++23、且需显式开启；有调试信息时成为可点击的 `file(line)` |
| `optionalFieldStrict` / `optionalFieldOfStrict`（**新增**） | `OptionalFieldCodec` 会吞掉"存在但非法"的值 | 会传播错误的对应版本 | 校验器不能把坏值当成"不存在" |

## 6. 测试分层

174 个 GoogleTest 用例分布在三个独立可执行文件中。`ctest` 会为每个用例加上所属层的
前缀（`unit.*`、`smoke.*`、`perf.*`），因此任何一层都可以按组选择运行。

**`test/unit/` → `codec_unit_tests`（148 个用例）**——组件级，覆盖各种边界情况：

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
| `string_and_enum_test.cpp` | 标量转换示例集：数字↔枚举、字符串↔枚举（含 `codecs::stringEnum`）、字符串↔数字、`either` 实现"数字或字符串"、枚举用于字段/列表/可选字段 |
| `error_path_test.cpp` | 错误定位：目标格式 `risks[3].condition.or[0].op`、改写消息措辞、嵌套列表、多字段同时失败、缺失键、无界 map、dispatch 载荷、编码侧、严格/宽松可选字段对比；`FrameTest` 固定 `report()` 的 codec 调用链、每帧的 `file(line)` 构造位置与两种排版，`CodecErrorTest` 覆盖 `throwIfError()` / `getOrThrow()` |
| `stacktrace_test.cpp` | 可选的 `std::stacktrace` 层：特性探测一致性、抓栈与标准库排版输出；未打开 `CODEC_RECORD_STACKTRACE` 时跳过自己 |
| `odr_test.cpp` + `odr_probe.cpp` | 仅头文件保证：两个都包含该单头文件的翻译单元能一起链接，且 inline 单例在两个 TU 中地址一致 |
| `header_self_contained_test.cpp` | 在其余所有头文件之前包含 `codec.hpp`，证明单头文件可独立编译 |

**`test/smoke/` → `codec_smoke_tests`（23 个用例）**——小而快的端到端检查，回答
"这个移植到底能不能用"：

| 文件 | 关注点 |
| --- | --- |
| `library_smoke_test.cpp` | 用**每一种** codec 类型组成的一份文档做解码/编码/再解码、非法输入拒绝、压缩与普通 ops 的一致性、部分结果 |
| `risk_def_test.cpp` | 参考风险定义文档的端到端验证（逐字节重新编码、递归条件、错误上报） |

**`test/perf/` → `codec_perf_tests`（3 个用例）**——测量而非门槛（唯一的断言是所有
实现路径结果一致）：

| 文件 | 关注点 |
| --- | --- |
| `codec_perf_test.cpp` | 手写 nlohmann 提取器/构建器，外加 nlohmann 自带的 ADL 映射（`from_json` + `get<T>()`），与 Codec 层在 2 风险与 400 风险文档上的计时对比；打印第 7 节的表格 |

共享辅助代码位于 `test/support/test_support.hpp`（JSON 解析，以及会把 codec 错误
消息作为失败原因抛出的 `decode` / `encode` / `decodeError` 包装）。

## 7. 性能：Codec 与 nlohmann/json 对比

`test/perf/codec_perf_test.cpp` 用五种方式测量同样的工作并打印表格；运行
`codec_perf_tests.exe` 或 `ctest -R perf -V` 即可复现。基线是
`nlohmann::ordered_json::parse`——也就是本移植自身使用的解析器——因此这些数字展示的
是 Codec 层在解析器*之上*额外付出的代价。两个 manual（手写）列分别代表只使用
nlohmann/json 时会写出的代码：一种是手写提取，另一种是 nlohmann 自带的 ADL 映射
（`from_json` + `get<T>()`）；两者都刻意**不做**任何校验。

测量环境：Windows x64、MSVC 14.50、`Release`；每个数字都是**5 次完整运行的
中位数**，每次运行取 7 轮校准后的最优值。夹具即第 4 节的风险定义文档。Debug 下会把
大文档夹具缩小到 40 个风险项以保持速度（约 3 秒），比例关系不变。

**2 个风险项（2 351 字节）**

| 操作 | 耗时 | 相对 `ordered_json::parse` |
| --- | ---: | ---: |
| `nlohmann::json::parse`（无序） | 10.7 µs | 0.83× |
| `nlohmann::ordered_json::parse` | 12.9 µs | 1.00× |
| 解析 + 手写提取 | 14.4 µs | 1.12× |
| 解析 + `get<RiskDocument>()`（nlohmann ADL 映射） | 14.9 µs | 1.16× |
| **`JsonValue::parse` + codec 解码** | **28.6 µs** | **2.22×** |
| 手写提取（已解析） | 1.5 µs | 0.11× |
| `get<RiskDocument>()`（已解析） | 1.8 µs | 0.14× |
| codec 解码（已解析的 `JsonValue`） | 15.5 µs | 1.20× |
| **codec 编码 + dump** | **31.2 µs** | **2.43×** |
| 手写构建 + dump | 13.8 µs | 1.07× |
| `to_json` + dump | 12.3 µs | 0.95× |

**400 个风险项（379 611 字节）**

| 操作 | 耗时 | 相对 `ordered_json::parse` |
| --- | ---: | ---: |
| `nlohmann::ordered_json::parse` | 3.19 ms | 1.00× |
| `nlohmann::json::parse`（无序） | 3.56 ms | 1.12×（在噪声范围内） |
| 解析 + `get<RiskDocument>()` | 4.17 ms | 1.31× |
| 解析 + 手写提取 | 4.98 ms | 1.56× |
| **`JsonValue::parse` + codec 解码** | **10.43 ms** | **3.27×** |
| `get<RiskDocument>()`（已解析） | 0.55 ms | 0.17× |
| 手写提取（已解析） | 0.53 ms | 0.17× |
| codec 解码（已解析的 `JsonValue`） | 7.03 ms | 2.20× |
| **codec 编码 + dump** | **11.24 ms** | **3.53×** |
| `to_json` + dump | 3.55 ms | 1.11× |
| 手写构建 + dump | 3.87 ms | 1.21× |

> **测量方法。** 绝对耗时随机器负载波动约 ±20 %（基线会同向波动，因此真正有意义的是
> 比例，而比例稳定在约 ±15 % 以内）。大文档对"进程此前做了多少工作"很敏感：若只运行
> `--gtest_filter=PerfTest.LargeDocument`，codec 解码实测约 4.8 ms 而不是约 7.0 ms。
> 下结论前请在本地重跑；这套基准的用途是相对比较，而不是绝对断言。
>
> **标注在成功路径上是零成本的。** 路径与栈帧（§3）只在错误向外传播时才构造，因此加入
> 它们并没有改变上面的数字：同样取 5 次运行中位数复测，大文档从 7.03 ms 变为 6.68 ms
> （codec 解码）、10.43 ms 变为 9.62 ms（解析 + 解码）、11.24 ms 变为 10.77 ms（编码），
> 全部落在上面所说的波动范围内。

### 结论

1. **经 Codec 层完成一次「文本 → 结构体」解码，耗时是裸 nlohmann 解析的 2.2 倍
   （小文档）到 3.3 倍（大文档）。** 解析是下限且占主导，Codec 层大致让它*翻倍*。
   绝对值上，2 个风险项的文档约 29 µs，即单核约 35 000 份/秒。
2. **仅 Codec 层（已解析 DOM）是解析器自身耗时的 1.2–2.2 倍**，是 **nlohmann 自带
   `get<T>()` 映射的 8.5–13 倍**。这个倍数换来的是普通映射完全不做的事情：缺失/多余
   键检测、类型检查、数值范围、能指出出错字段名的 `error()` 消息、部分结果
   （`getOrThrow(allowPartial)`）、`Lifecycle` 追踪，以及可复用的组合能力——递归
   record、`dispatch`、`either`、压缩 map。如果需要这些，每字段约 1 µs 很划算；
   如果不需要，直接用 nlohmann（见下文）。
3. **代价随字段数线性增长**：2 风险文档 0.70 µs/字段，400 风险文档 1.60 µs/字段。
   差异来自 380 KB DOM 的缓存与分配压力，而非算法问题——不存在超线性行为。
4. **编码在小文档上是 `to_json` 的 2.5 倍、在大文档上是 3.2 倍**；其中相当一部分是
   逐字段的构建器开销。文档越大，`dump()` 越占主导，Codec 层在「编码+序列化」中的
   占比也随之下降。
5. **`ordered_json` 的解析时间比普通 `json` 多约 15–20 %**（大文档上在噪声范围内）。
   这是插入有序对象的代价，而本移植需要它来实现逐字节一致的重新编码以及 Gson 的
   "原地替换成员"语义。
6. **Debug 构建的绝对值约慢 30 倍**（CLion 的默认配置），但比例关系不变：解析器的
   2.3–2.9 倍、手写路径 / `get<T>()` 的 6.7–8.5 倍、编码 2.7 倍，且每字段稳定在
   24 µs。任何性能结论都应以 Release 为准，Debug 只用于查找缺陷。

### 如果只需要「JSON → 结构体」，其实并不需要 Codec 层

nlohmann/json 本身就能通过 ADL 把 JSON 映射到结构体，本移植也不会妨碍这条路：

```cpp
nlohmann::ordered_json raw = nlohmann::ordered_json::parse(text);
RiskDocument document = raw.get<RiskDocument>();   // from_json 由 ADL 找到
```

这条路径在两个夹具上分别耗时 **14.9 µs / 4.17 ms**——仅为裸解析的 **1.16 倍 /
1.31 倍**——而 Codec 层是 28.6 µs / 10.4 ms。因此对于「模式固定、无需校验」的场景，
Codec 层付出了 2–3 倍解析代价却没有收益；此时 `get<T>()` 或手写提取才是正确的工具，
本移植也如实呈现这一点（`perf.StrategiesAgreeOnTheFixture` 会校验四种方式产出的结构
完全一致，因此这个比较是公平的）。

**但这并不构成"不该用 nlohmann"的理由，恰恰相反：**

* nlohmann 是两条路径共同支付的**解析下限**：codec 的 28.6 µs 里有 12.9 µs 就是
  nlohmann 对同一段文本的解析。去掉 nlohmann 并不会消除这笔开销，只会把经过实战
  检验的解析器/序列化器换成自己写的（本移植的第一版就是如此：约 450 行代码，仍然
  必须正确处理代理对、UTF-8 校验和最短往返浮点输出）。
* 在本移植中，nlohmann 被限制在**一个小节、一个类型**之后——单头文件的第 1 节是唯一
  提到它的地方：

  ```powershell
  > $jsonEnd = (Select-String -Path include/codec.hpp -Pattern '^// 2/8').LineNumber
  > (Select-String -Path include/codec.hpp -Pattern nlohmann).LineNumber -gt $jsonEnd
  # -> 无输出：所有 nlohmann 引用都在第 1 节内（第 30..547 行）
  ```

  第 2–8 节（`lifecycle`、`data_result`、`dynamic_ops`、`json_ops`、`codec`、
  `codecs`、`record_codec`）只会看到 `JsonValue` 与 `DynamicOps`。要替换 DOM——或
  干脆去掉这个依赖改用自研实现——只需改动 `codec.hpp` 的第 1 节与
  `scripts/fetch_deps.ps1`，其他部分一行都不用动。
* 中间的 DOM 也不是"用了 nlohmann"才产生的：DFU 的 `DynamicOps<T>`/`MapLike` 契约
  本身就是**随机访问**的。`dispatch` 先读类型键，再用选中的 codec 对*同一个 map*
  重新解码；`ListCodec` 把失败的原始元素作为部分结果返回；`unboundedMap` 消费
  `MapLike::entries()`；压缩 map 会在位置列表之上重建一个 `MapLike`。只向前的 token
  流无法支撑其中任何一项。想彻底避开 DOM，就得改用流式方案（SAX 或 simdjson 的
  on-demand 直接写入结构体）——那是另一个库，而不是这个库的移植，并且会放弃 DFU 的
  组合模型，而那正是 DFU 的价值所在。

一句话：需要校验、诊断与可复用组合时保留 Codec 层；而 nlohmann 两种情况都该保留——
它就是 JSON 层，Codec 层是它的*使用者*，而不是它的替代品。


### 基准测试发现了什么

写这套对比是值得的——它暴露出两处初版实现**不够忠实于 DFU**（而不仅仅是慢）的地方：

| 问题 | 原因 | 修复 | 效果（400 风险项） |
| --- | --- | --- | --- |
| 每次成员访问都深拷贝 JSON 子树 | `JsonValue` 采用值语义，而 Gson 的 `JsonElement` 是**引用**类型 | `JsonValue` 改为句柄：与文档共享所有权并指向其中一个节点 | 解码 29.4 ms → 约 7.0 ms（≈4 倍） |
| 编码是二次方复杂度 | 构建器在每次 `add` 时复制整个累加器，而 DFU 的 `ImmutableList.Builder`/`JsonObject` 是**按引用传递的可变对象** | 构建器状态改为指向可变累加器的 `shared_ptr` | 编码 199 ms → 约 11.2 ms（≈18 倍） |

两者都属于设计层面的正确性问题（本移植在规模上表现得不像 DFU），而不是微优化；它们
既被原有测试覆盖，也被新增的 `perf.StrategiesAgreeOnTheFixture` 等价性检查覆盖。
