# Codec — a C++17 port of Mojang's DataFixerUpper `Codec` API

**English** | [简体中文](README_zh.md)

A faithful C++17 port of the `com.mojang.serialization` package from
**DataFixerUpper 6.0.8** — the `Codec` / `MapCodec` / `RecordCodecBuilder` /
`DynamicOps` / `DataResult` mechanism that Minecraft uses to serialise and
deserialise everything from registry entries to world data.

The reference implementation was decompiled from
`com.mojang:datafixerupper:6.0.8` with
[`scripts/decompile_reference.ps1`](scripts/decompile_reference.ps1) into
`reference/dfu-6.0.8/`. Those sources are Mojang's, they are study material only
— not part of the build and deliberately not versioned — and the script exists so
the port's provenance can be reproduced.

* Single-header library: [`include/codec.hpp`](include/codec.hpp) — one file, ~4 400 lines, CMake `INTERFACE` target, nothing to build
* Comments inside the header are written in **Chinese**; API names, error messages, test names and both READMEs stay English
* Layered tests: [`test/unit/`](test/unit) (155 cases), [`test/smoke/`](test/smoke) (23 cases) and [`test/perf/`](test/perf) (3 cases, codec vs nlohmann/json benchmark) — one executable each
* Reference use case (the risk-definition document): [`models/risk_def.hpp`](models/risk_def.hpp)
* Runnable example: [`examples/risk_def_main.cpp`](examples/risk_def_main.cpp)

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
DataResult<Value> encoded = RiskDefCodec.encodeStart(JsonOps::INSTANCE, value);
// `Value` is the format-neutral handle the ops layer exchanges; with JsonOps,
// `encoded.result()->asJson()` gives the JSON node back.
```

---

## 1. Dependencies and build

| Dependency | Version | Where |
| --- | --- | --- |
| C++ | **C++17** (`/std:c++17`, `-std=c++17`) | required (C++20/23 unlock the optional diagnostics below) |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 (single header) | `third_party/nlohmann/json.hpp` |
| [GoogleTest](https://github.com/google/googletest) | 1.17.0 | `third_party/googletest-1.17.0` (tests only) |
| CMake | ≥ 3.16, Ninja or MSBuild | build |

The library itself is header-only, so there is nothing to configure — but because
its comments are Chinese, an MSVC invocation outside this project's CMake target
needs `/utf-8` (or `/source-charset:utf-8`); the `codec` target already adds it.

```powershell
# 1. fetch nlohmann/json + GoogleTest (add -Proxy http://127.0.0.1:7890 if needed)
powershell -File scripts/fetch_deps.ps1 [-Proxy http://127.0.0.1:7890]

# 2. build (uses MSVC via vcvars64.bat) and run the tests
powershell -File scripts/build.ps1 -RunTests
```

Or drive CMake directly:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure   # all layers, 181 cases
cmake --build build --target check           # same thing, one click/target
build/examples/risk_def_example.exe          # optional: sample document demo

# optional: also capture a real std::stacktrace for every error (switches to C++23)
cmake -S . -B cmake-build-stacktrace -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCODEC_RECORD_STACKTRACE=ON
```

| CMake option | Default | Effect |
| --- | --- | --- |
| `CODEC_BUILD_TESTS` | `ON` | build the three test layers |
| `CODEC_BUILD_EXAMPLES` | `ON` | build the example programs |
| `CODEC_WARNINGS_AS_ERRORS` | `OFF` | `/WX`, `-Werror` |
| `CODEC_RECORD_STACKTRACE` | `OFF` | define `CODEC_RECORD_STACKTRACE`, compile as C++23 and capture `std::stacktrace` per error |

The three test layers are independent executables, so they can also be run
directly:

```powershell
build/test/unit/codec_unit_tests.exe     [--gtest_filter=RecordCodecTest.*]
build/test/smoke/codec_smoke_tests.exe   [--gtest_filter=SmokeTest.*]
build/test/perf/codec_perf_tests.exe     [--gtest_filter=PerfTest.SmallDocument]  # prints a table
```

**Running from CLion.** The project is a normal CMake project: open the folder,
let CLion configure it, and every case shows up in the Run/Debug dropdown —
`All CTest` runs all three layers, `unit.*` / `smoke.*` / `perf.*` group them, and
each `TEST(...)` (including each benchmark fixture) can be run or debugged
individually. The `check` target is also available in the target list. The perf
layer calibrates its iteration counts and takes ~3.5 s in Release, ~13 s in
Debug; `ctest -R perf -V` prints its tables.

The suite is verified with **MSVC 14.44 (VS2022 / CLion) in Debug** and
**MSVC 14.50 (VS2026) in Release**.

> **Toolchain note (MSVC 14.44).** A raw string literal that contains backslash
> escapes must not be passed directly to a GoogleTest macro: MSVC's preprocessor
> rewrites the token while stringising it, and the escapes are then re-interpreted
> (`R"("\ud83d\ude00")"` becomes a surrogate universal character name → `C3850`,
> `R"("a\"b")"` → `C2017`). Bind such literals to a local first
> (`const std::string text = R"(...)"; EXPECT_EQ(f(text), ...)`) — see
> `test/unit/json_test.cpp`. Plainer raw strings such as `R"({"a":1})"` are fine.

CMake options: `CODEC_BUILD_TESTS`, `CODEC_BUILD_EXAMPLES`,
`CODEC_WARNINGS_AS_ERRORS`. The library target is `codec` (alias `codec::codec`);
link it and `#include "codec.hpp"`.

### Header-only, in one file

The whole library is [`include/codec.hpp`](include/codec.hpp): no translation
units, no generated files, and an `INTERFACE` CMake target — so there is nothing
to compile, link, install or keep ABI-compatible. Consuming it costs one
`target_link_libraries`:

```cmake
add_subdirectory(path/to/Codec)                     # or FetchContent_Declare(...)
target_link_libraries(my_app PRIVATE codec::codec)  # adds include/ + third_party/ + C++17
```

The file is organised in eight commented sections in dependency order — `json`,
`lifecycle`, `data_result`, `dynamic_ops`, `json_ops`, `codec`, `codecs`,
`record_codec` — so it stays navigable (search for `// 3/8  data_result`), and a
drop-in copy needs only this file plus nlohmann/json.

Everything at namespace scope is `inline` (functions), a template, or an inline
variable (`JsonOps::INSTANCE`, `codecs::Int`, ...), so including the header from
any number of translation units is safe and the singletons really are shared:

* `test/unit/odr_probe.cpp` is a second TU including the header (plus
  `models/risk_def.hpp`), so the unit test binary links every definition twice —
  a missing `inline` fails the build at link time;
* `test/unit/odr_test.cpp` compares the addresses of the codecs and of
  `JsonOps::INSTANCE` across the two TUs, which catches a definition that
  silently became translation-unit-local. Swap `inline` for `static` on
  `codecs::Int` and both tests fail;
* `test/unit/header_self_contained_test.cpp` includes `codec.hpp` *before* any
  other header — including GoogleTest — so the header cannot borrow declarations
  from anywhere else;
* eight further TUs include it as the only library header.

The only compile-time dependency is [nlohmann/json](#the-json-value-type) (a
single header in `third_party/`, or your own copy on the include path); the tests
additionally need GoogleTest, which the `INTERFACE` target does not propagate.

### The JSON value type

DFU's `JsonOps` is backed by Gson's `JsonElement`. This port backs it with
**nlohmann/json**, specifically `nlohmann::ordered_json`, so that

* objects keep insertion order, exactly like Gson's `LinkedTreeMap`-backed
  `JsonObject`,
* assigning an existing member replaces it **in place** (Gson's
  `JsonObject#add` semantics), and
* parsing, serialisation and float formatting are nlohmann's.

`JsonValue` is a thin facade over `nlohmann::ordered_json` with **reference
semantics**, like Gson's `JsonElement`: a handle shares ownership of the document
it came from and points at one node, so copying a handle, reading a member or
walking an array is O(1) and never duplicates the DOM. The codec layer stays
readable while applications keep full access:

Above `JsonValue` sits the ops layer's own value type, `codec::Value`: a
format-neutral handle (shared owner + node pointer + type tag) that every
`DynamicOps` implementation exchanges, so a second format's ops (TOML, NBT) can
plug into the same codecs. With `JsonOps` the payload is a `JsonValue` node, so
`value.asJson()` is the way back — and the erasure costs nothing measurable: the
2-risk codec decode measured 15 423 ns against the 15 500 ns it had before the
change (see §7 and [`docs/dynamic_ops_generic.md`](docs/dynamic_ops_generic.md)).

`codec::Dynamic` pairs a value with the ops that understands it, which is what
`codecs::Passthrough` carries (`Codec<Dynamic>`): a raw dynamic value is meaningless
without its ops, and only the pair can be moved between formats — `Dynamic::convertTo`
is where `DynamicOps::convertTo` starts doing real work.

```cpp
JsonValue value = nlohmann::ordered_json::parse(text);   // implicit conversion
const nlohmann::ordered_json& raw = value.raw();         // back to nlohmann
value.find("id")->asString();
value.get("missing").has_value();                        // false
```

Values are immutable once built; build new documents through
`JsonValue::object(...)`, `JsonValue::array(...)` or nlohmann directly.

## 2. Layout

```
include/codec.hpp      the entire library, in eight commented sections:
                         1 json          JsonValue (nlohmann-backed) + Number (java.lang.Number)
                         2 lifecycle     Lifecycle
                         3 data_result   DataResult, PartialResult semantics, Unit
                         4 dynamic_ops   DynamicOps, MapLike, RecordBuilder, ListBuilder, KeyCompressor
                         5 json_ops      JsonOps (INSTANCE / COMPRESSED)
                         6 codec         Encoder, Decoder, MapEncoder, MapDecoder, Codec, MapCodec
                         7 codecs        primitive + composite codecs, range checks, recursive, dispatch
                         8 record_codec  RecordCodecBuilder: record<>, fieldOf, optionalFieldOf, forGetter
models/risk_def.hpp    the risk-definition use case (codecs for RiskDef/Condition)
test/
  CMakeLists.txt       defines the layers + the one-click `check` target
  support/             shared test helpers (test_support.hpp)
  unit/                codec_unit_tests   -- component level suites (incl. the ODR guard)
  smoke/               codec_smoke_tests  -- end-to-end API checks
  perf/                codec_perf_tests   -- codec vs nlohmann/json measurements
examples/              risk_def_main.cpp
reference/dfu-6.0.8/   decompiled Java original (study only)
third_party/           nlohmann/json, googletest
scripts/               fetch_deps.ps1, build.ps1
```

## 3. Java → C++ mapping

| DataFixerUpper (Java) | This port (C++) |
| --- | --- |
| `Codec<A>`, `MapCodec<A>` (interfaces) | copyable value types holding `std::function` wrappers |
| `Encoder<A>`, `Decoder<A>`, `MapEncoder<A>`, `MapDecoder<A>` | same four wrappers, same combinators |
| `Codec.of(encoder, decoder)` / `MapCodec.of(...)` | identical factories |
| `MapCodec.MapCodecCodec` | `Codec` created from a `MapCodec` (implicit conversion, `Codec::mapCodec()` detects it) |
| `DynamicOps<T>` (`T` = `JsonElement`) | abstract `DynamicOps` over the single `JsonValue` |
| `JsonOps.INSTANCE` / `JsonOps.COMPRESSED` | `JsonOps::INSTANCE` / `JsonOps::COMPRESSED` |
| `DataResult<R>` (`Either<R, PartialResult<R>>`) | `DataResult<R>` (optional value + optional error, partial value kept) |
| `Lifecycle` | `Lifecycle` (same `add` rules) |
| `RecordCodecBuilder.create(i -> i.group(f1, f2).apply(i, Ctor::new))` | `record<O>(f1, f2)` or `record<O>(ctor, f1, f2)` |
| `Codec.STRING.fieldOf("id").forGetter(RiskDef::id)` | `fieldOf("id", &RiskDef::id, codecs::String)` or `codecs::String.fieldOf("id").forGetter(&RiskDef::id)` |
| `Codec.optionalFieldOf("x", default)` | `optionalFieldOf("x", &O::member, codec, defaultValue)` |
| `Codec.listOf()`, `Codec.either(a, b)`, `Codec.pair(a, b)`, `Codec.unboundedMap(k, v)` | `listOf(codec)`, `either(a, b)`, `pair(a, b)`, `unboundedMap(k, v)` |
| `codec.dispatch` / `partialDispatch` / `dispatchMap` | same names (`KeyDispatchCodec`) |
| `Codec.intRange` / `floatRange` / `doubleRange` | same names |
| `Codec.checkRange` | inlined into the range codecs |
| `Stream<T>` in `DynamicOps` | `std::vector<JsonValue>` (materialised) |
| `Optional<T>` | `std::optional<T>` |
| `Pair<A, B>` | `std::pair<A, B>` |
| `Either<L, R>` | `codec::Either<L, R>` (variant-backed) |
| `Unit`, `PASSTHROUGH`, `EMPTY`, `Codec.unit` | `codec::Unit`, `codecs::Passthrough` (`Codec<Dynamic>`), `codecs::Empty`, `Codec<T>::unit` |

The port implements: primitive codecs (`Bool`, `Byte`, `Short`, `Int`, `Long`,
`Float`, `Double`, `String`, `Passthrough`), `ListCodec`, `EitherCodec`,
`PairCodec`, `UnboundedMapCodec`, `OptionalFieldCodec`, `FieldEncoder`/
`FieldDecoder`, `RecordCodecBuilder`, `KeyDispatchCodec`, `SimpleMapCodec`'s
key-compression machinery (`KeyCompressor`, compressed record/list builders),
`Lifecycle`, `DataResult` (incl. `apply2`/`apply2stable`/`apply3`,
`promotePartial`, `setPartial`, `mapError`, `resultOrPartial`, `getOrThrow`) and
the full `Codec`/`MapCodec` combinator surface (`xmap`, `flatXmap`,
`comapFlatMap`, `flatComapMap`, `orElse`, `orElseGet`, `mapResult`, `withLifecycle`,
`stable`, `deprecated`, `fieldOf`, `optionalFieldOf`, `promotePartial`).

Out of scope (DFU packages that build *on* Codec): `DataFixer`, `Schema`,
`TypeRewriteRule`, the optics/profunctor machinery and `NbtOps`/`Dynamic`
wrappers.

### Scalar conversions (enum, number, string)

There are no separate "enum codecs" or "string number codecs": every combination
is built from the same two combinators, so it composes with fields, lists,
`dispatch`, optional fields and everything else.

| JSON | C++ | How |
| --- | --- | --- |
| number | `enum class` | `Int.flatXmap<E>(toEnum, toInt)` (or `Int.xmap<E>` when the mapping is total) |
| string | `enum class` | `codecs::stringEnum<E>({{"low", E::Low}, ...}, "E")`, or `String.flatXmap<E>` by hand |
| string | number | `String.flatXmap<int32_t>(parse, toString)` — use `std::from_chars` for a strict parse |
| number | `std::string` | `Int.xmap<std::string>(std::to_string, ...)` (the JSON stays a number) |
| number *or* string | number | `either(Int, String).flatXmap<int32_t>(...)` — accepts `8080` and `"8080"`, encodes the canonical number |

`test/unit/string_and_enum_test.cpp` is the runnable cookbook for all five rows,
including the error messages (`Unknown Severity: "fatal"`, `Not a number: "80x"`)
and how `optionalFieldOf` treats a `null` or invalid enum name.

### Error locations

DFU messages say *what* went wrong but never *where*: a bad value deep inside a
document only reports `Not a string: 1`. This port records the location next to the
message, so a failure reads like a compiler/validator diagnostic:

```cpp
const DataResult<RiskDocument> result = riskDocumentCodec().parse(JsonOps::INSTANCE, input);

result.isError();     // true
result.message();     // "Not a string: 1"                                    (DFU text)
result.location();    // "risks[3].condition.or[0].op"
result.describe();    // "risks[3].condition.or[0].op: Not a string: 1"        (report this)
result.report();      // ... plus the chain of codecs that handled the value (below)
result.errors();      // {ErrorPart{path, frames, message}} -- one entry per failed spot
```

`report()` is the full diagnostic: the same first line as `describe()`, then one
`  in <codec>` line per codec the value passed through, innermost first.

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

* `message()` is **byte-identical to DFU** (the local messages joined with `"; "`),
  which is why adding paths changed no existing behaviour.
* `describe()` prefixes each failed spot with its location, e.g.
  `a: No key a in MapLike[{}]; b: No key b in MapLike[{}]`. `location()` returns the
  path only when every part shares one, otherwise `""`.
* Locations are attached by the containers as the error travels outwards:
  `fieldOf` adds the key, `ListCodec` adds `[i]`, `unboundedMap` adds the entry key,
  `dispatch` adds `value` for non-map payloads, and the record/list builders add the
  field or element on the *encode* side (`small: too large to encode: 200`).
* Frames are attached the same way, one `addFrame` per container, and live on each
  `ErrorPart` — so a result that failed in several places prints one block per
  failure. Names are each codec's **own** short name (`Int`, `list`,
  `RecordCodec[a, b]`, `optional[n]`, `dispatch[type]`, `unboundedMap`, `either`,
  `pair`), never the composed name a container carries, so a chain of nested records
  does not repeat itself.
* Both annotations are built *only while an error travels outwards*, so a successful
  decode allocates neither a path segment nor a frame string; the benchmark in §7
  shows no measurable difference from the annotation-free build.
* `mapError` keeps the path and the frames when every part shared them — that is how
  `unboundedMap`'s `missed input:` rewrite and a `mapResult` wording rewrite stay
  located. When the parts disagree, both are dropped rather than guessed at.
* `promotePartial`, `resultOrPartial` and `getOrThrow` hand the located form to
  their `onError` callback — that is what those callbacks are for.
* Prefer different wording (e.g. `expected string, got number`)? Rewrite the leaf
  codec with `Codec::mapResult` + `DataResult::mapError` and the location survives;
  see `ErrorPathTest.WordingCanBeRewrittenWhileKeepingTheLocation`.
* Paths and frames are an **addition** — DFU has neither.
  `test/unit/error_path_test.cpp` covers the format, nested lists, multi-part
  failures, missing keys, unbounded maps, dispatch payloads, the encode side
  (`FrameTest` for the chains, `ErrorPathTest` for the locations).

**Clickable frames, portably.** Every frame also carries the place its codec was
built, so `report()` doubles as a stack trace you can click through in an IDE:

```text
risks[3].condition.or[0].op: Not a string: 1
  0> D:\programming\cpp\Codec\include\codec.hpp(3445): String
  1> D:\programming\cpp\Codec\models\risk_def.hpp(116): optional[op]
  2> D:\programming\cpp\Codec\models\risk_def.hpp(109): RecordCodec[or, and, not, param, op, value, list_match]
  ...
```

* The location is a three-tier `SourceLocation`: `std::source_location` when the
  standard library has it (C++20), otherwise `__builtin_FILE()`/`__builtin_LINE()`
  (MSVC, GCC, Clang — all available in C++17), otherwise no location at all. The
  code always compiles; only the diagnostics degrade.
* It is a compile-time literal: nothing is allocated, and no debug information
  (PDB/DWARF) is needed, so it works in Release builds too.
* `report(FrameStyle::native | msvc | gnu)` picks the rendering — MSVC's
  `file(line):` shape (the same one its own `<stacktrace>` uses) or the gdb-style
  `#0 name at file:line`. Both are what CLion links, and `native` follows the
  compiler. A frame built in your model points at your `record<...>`/`fieldOf(...)`
  line; library frames point into `codec.hpp`.

**Throwing instead of returning.** Where you would rather abort than inspect a
`DataResult`:

```cpp
try {
  RiskDocument document = riskDocumentCodec().parse(JsonOps::INSTANCE, input).throwIfError();
} catch (const codec::CodecError& error) {
  error.what();       // "risks[3].condition.or[0].op: Not a string: 1"  (one line)
  error.report();     // ... plus the located codec chain
  error.location();   // "risks[3].condition.or[0].op"
  error.errors();     // the individual failures (path + frames + message)
  error.hasPartial(); // whether a usable partial value existed
}
```

* `CodecError` derives from `std::runtime_error`, so existing
  `catch (const std::exception&)` blocks keep working; DFU's
  `getOrThrow(allowPartial, onError)` throws it too.
* Needs no compiler builtins, no C++20/23 and no debug info — the most portable
  way to carry the diagnostic, and a breakpoint on `CodecError` shows the real
  stack in any debugger.

**A real stack trace, when the standard library has one.** Build with
`-DCODEC_RECORD_STACKTRACE=ON` (which switches the build to C++23) to capture
`std::stacktrace` where the failure is created and append it to `report()`:

```text
[1]: Not a number: "x"
  0> D:\programming\cpp\Codec\include\codec.hpp(3445): Int
  1> D:\programming\cpp\Codec\build\zh\print_stacktrace.cpp(16): list
  stacktrace:
    0> D:\programming\cpp\Codec\include\codec.hpp(1074): demo!codec::DataResult<codec::Number>::makeErrorPart+0x66
    ...
    21> D:\programming\cpp\Codec\build\zh\print_stacktrace.cpp(17): demo!main+0x111
```

* It is rendered by the standard library itself, so it already has the shape your
  toolchain's IDE parses; with debug info (`/Zi`, i.e. Debug builds) every frame
  carries `file(line)` (MSVC) or `file:line` (libstdc++/libc++) — including the
  line that called `parse()`, which the codec chain cannot know.
* Off by default: capturing costs on *every* error, including the
  present-but-invalid optional values DFU deliberately swallows. Without C++23 the
  header is still a plain C++17 header and this layer simply does not exist
  (`test/unit/stacktrace_test.cpp` skips itself).

**Strict optional fields.** Paths only help if the error is not swallowed, and DFU's
`OptionalFieldCodec` deliberately swallows a present-but-invalid optional value. For
validation you want the error, so the port adds error-propagating counterparts:

| | `{"n": 1}` with `Codec<optional<int>>` | Use for |
| --- | --- | --- |
| `optionalFieldOf` (DFU) | decodes to `nullopt` | lenient loading of data you do not control |
| `optionalFieldOfStrict` | fails: `n: Not a number: 1` | validators — a broken value must not look like an absent one |

`optionalFieldOfStrict(name, codec)`, `optionalFieldOfStrict(name, codec, default)`
and the record-field wrappers (`optionalFieldOfStrict(name, &O::member, codec[, default])`)
mirror the lenient API one-to-one. The reference risk model uses the strict variants
inside `condition`, because silently dropping a malformed security rule would turn
"this rule is broken" into "this rule does not apply".

## 4. The reference use case

`models/risk_def.hpp` models the risk-definition document, including its
recursive condition tree:

```cpp
struct Condition {
  std::vector<Condition> orClauses;    // JSON "or"
  std::vector<Condition> andClauses;   // JSON "and"
  std::vector<Condition> notClauses;   // JSON "not"
  std::optional<std::string> param;
  std::optional<std::string> op;
  std::optional<Dynamic> value;       // any dynamic value (Passthrough)
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

`recursive<A>(supplier)` is the one addition without a DFU counterpart: Java
expresses recursion through the datafixer graphs, C++ resolves the supplier on
first use (which also breaks the static-initialisation cycle).

Measured results (see `test/smoke/risk_def_test.cpp` and the example):

* the sample document decodes into `RiskDocument` with both risks, the nested
  `or` condition, the leaf `list_match` predicate and UTF-8 `cn`/`en` strings;
* re-encoding is **byte-for-byte identical** to a compact re-serialisation of the
  input (the record field order mirrors the document);
* decode → encode → decode is stable, and a hand-built document round-trips;
* a malformed rule is reported with its exact location and the chain of codecs that
  handled it, each frame carrying the line that built it — `risk_def_example.exe
  bad.json` prints (CLion links every `file(line)`)

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

  The condition tree is recursive, so the same `RecordCodec` appears at both levels.
  The model uses the strict optional variants so a broken condition is never silently
  dropped (see §3 and §5).

## 5. Behaviour notes (faithfulness)

The port reproduces DFU's behaviour on purpose, including its quirks. The tests
pin these down:

* **Lifecycle** — `Experimental` always wins; `Deprecated(n)` keeps the lowest
  `n`; every primitive read is `Experimental`, so a plain `record<>` decode
  reports `Experimental` (use `.stable()` to override).
* **Boolean/number coercion** — `JsonOps.getNumberValue` accepts booleans, so
  `Codec.INT` decodes `true` as `1` and `either(Int, Bool)` puts `true` on the
  left; `Number.intValue()`/`byteValue()` truncate exactly like
  `java.lang.Number` (`1.9 → 1`, `300 → 44`, `256 → 0`).
* **`null` members** — `MapLike.get` treats an explicit JSON `null` as "absent"
  while `entries()` still reports it (a JsonOps quirk).
* **`OptionalFieldCodec`** — a present-but-invalid optional field decodes to
  `std::nullopt` instead of failing, and `optionalFieldOf(name, default)`
  silently falls back to the default. Errors inside optional fields are
  therefore *dropped*, matching DFU.
* **`ListCodec`** — the partial value of a failed list is the pair
  `(decoded prefix, raw failed elements)`; the failed elements' own partial
  values are folded into the prefix, exactly like DFU's `Applicative` chain.
* **Error messages** — `"; "`-joined in field declaration order
  (`"No key a in MapLike[{}]; No key b in MapLike[{}]"`).
* **Compressed maps** — `JsonOps::COMPRESSED` encodes records as key-indexed
  lists (`["bob",42,["a"],["c","z"]]`), with `KeyCompressor` deriving the index
  order from `MapCodec::keys`.

Intentional deviations and additions, all documented in the headers:

| Area | DFU | Port | Why |
| --- | --- | --- | --- |
| `DynamicOps` value type | generic `T` (JsonElement, NbtTag, …) | a type-erased `Value` handle (owner + node + tag), plus `Dynamic` (value + its ops) as the `Passthrough` carrier | the ops layer is format-neutral now; `JsonValue` is the JSON DOM behind `JsonOps`, and `convertTo` becomes a real conversion once a second ops exists (see [`docs/dynamic_ops_generic.md`](docs/dynamic_ops_generic.md)) |
| `Stream<T>` | lazy Java streams | `std::vector` | no lazy streams in the standard library |
| `KeyCompressor.compress` | unknown key → `0` (fastutil default) | unknown key → `-1` → treated as absent | avoids silently reading index 0 |
| `UnboundedMapCodec` duplicates | `ImmutableMap.Builder` throws | last-wins, insertion order kept | keeps decoding usable |
| `CompressedMapLike` out-of-range index | `IndexOutOfBoundsException` | treated as absent | no exceptions in the decode path |
| `Encoder.error(msg)` | appends the value's `toString` | uses the message verbatim | C++ has no universal `toString` |
| `Codec.optionalFieldOf(name, Lifecycle, …)` | 4-argument overload | not ported | rarely used; `.stable()` covers it |
| `codec::recursive<A>(supplier)` *(addition)* | — (Java expresses recursion through the datafixer graphs) | provided | resolves the supplier on first use, which also breaks the static-initialisation cycle |
| `codecs::stringEnum<E>(table, name)` *(addition)* | — (Minecraft uses `StringRepresentable.fromEnum`, which is not in DFU) | provided | name-table enums without boilerplate; implemented with `flatXmap`, so it composes like any other codec |
| `DataResult::location()` / `describe()` / `report()` *(addition)* | no locations anywhere | every field/element/map entry attaches its path, every codec its own name and construction site | messages stay DFU-identical; `describe()` reports `risks[3].condition.or[0].op: Not a string: 1`, `report()` adds the codec chain with clickable `file(line)` frames |
| `SourceLocation` + `report(FrameStyle)` *(addition)* | — | `std::source_location` / `__builtin_FILE/LINE` / nothing, in that order | cross-platform clickable frames without debug info; degrades instead of failing |
| `DataResult::throwIfError()` / `codec::CodecError` *(addition)* | `getOrThrow` throws `RuntimeException` | `std::runtime_error` subclass carrying location, frames, parts and partial flag | portable "throw, don't return", catchable as `std::exception` |
| `CODEC_RECORD_STACKTRACE` *(addition)* | no stack traces | `std::stacktrace` captured at the failure, rendered by the STL | C++23 only, opt-in; Debug info turns it into clickable `file(line)` |
| `optionalFieldStrict` / `optionalFieldOfStrict` *(addition)* | `OptionalFieldCodec` swallows a present-but-invalid value | error-propagating counterpart | a validator must not read a broken value as an absent one |

## 6. Test layers

181 GoogleTest cases in three independent executables. `ctest` prefixes each case
with its layer (`unit.*`, `smoke.*`, `perf.*`), so any layer can be selected as a
group.

**`test/unit/` → `codec_unit_tests` (155 cases)** — component level, exhaustive
on edge cases:

| File | Focus |
| --- | --- |
| `json_test.cpp` | nlohmann-backed DOM, parse/dump round-trips, escapes/unicode, Gson equality, `Number` narrowing |
| `nlohmann_interop_test.cpp` | `JsonValue` ⇄ `nlohmann::ordered_json`, codecs fed from nlohmann documents |
| `data_result_test.cpp` | `Lifecycle.add` rules, success/error/partial, `map`/`flatMap`/`apply2`/`apply3`, `promotePartial`, `getOrThrow` |
| `primitives_test.cpp` | `Bool`/`Byte`/`Short`/`Int`/`Long`/`Float`/`Double`/`String`/`Passthrough`, `mergeToPrimitive` |
| `dynamic_ops_test.cpp` | `JsonOps` primitives, `mergeToList/Map`, `MapLike` null rules, builders, `KeyCompressor`, compressed maps |
| `dynamic_test.cpp` | `Dynamic` (value + its ops): construction, `asNumber`/`asString`/`asBoolean`, `get`/`getElement`, `set`/`remove`/`update`, `convertTo`, value-based equality, `decode` returning the remaining value |
| `codec_combinators_test.cpp` | `xmap`/`flatXmap`/`comapFlatMap`/`flatComapMap`, `orElse`, `mapResult`, `either`, `pair`, `listOf`, `unboundedMap`, ranges, unit codecs, map-codec combinators |
| `record_codec_test.cpp` | `record<>` in all forms, `fieldOf`/`optionalFieldOf`/`forGetter`, error joining, partial objects, keys, compression |
| `dispatch_test.cpp` | `KeyDispatchCodec` (`partialDispatch`/`dispatch`/`dispatchMap`), map-codec payload merging, compressed dispatch |
| `string_and_enum_test.cpp` | the scalar-conversion cookbook: number↔enum, string↔enum (incl. `codecs::stringEnum`), string↔number, number-or-string via `either`, enums in records/lists/optional fields |
| `error_path_test.cpp` | error locations: the requested `risks[3].condition.or[0].op` form, message wording rewrites, nested lists, multi-part failures, missing keys, unbounded maps, dispatch payloads, the encode side, strict vs lenient optionals; `FrameTest` pins the `report()` codec chains, their `file(line)` construction sites and both rendering styles; `CodecErrorTest` covers `throwIfError()` / `getOrThrow()` |
| `stacktrace_test.cpp` | the optional `std::stacktrace` layer: feature-detection consistency, capture and STL-rendered output; skips itself when `CODEC_RECORD_STACKTRACE` is off |
| `odr_test.cpp` + `odr_probe.cpp` | header-only guarantee: two TUs including the single header link together, and the inline singletons have one shared address |
| `header_self_contained_test.cpp` | includes `codec.hpp` before every other header, proving the single header stands alone |

**`test/smoke/` → `codec_smoke_tests` (23 cases)** — small and fast end-to-end
passes that answer "does the port work at all?":

| File | Focus |
| --- | --- |
| `library_smoke_test.cpp` | one document using *every* codec kind decoded/encoded/re-decoded, malformed-input rejection, compressed ⇄ plain ops agreement, partial results |
| `risk_def_test.cpp` | the reference risk-definition document end to end (byte-identical re-encode, recursive conditions, error reporting) |

**`test/perf/` → `codec_perf_tests` (3 cases)** — measurement, not a gate (the
only assertions are that all strategies produce the same result):

| File | Focus |
| --- | --- |
| `codec_perf_test.cpp` | hand-written nlohmann extractor/builder plus nlohmann's own ADL mapping (`from_json` + `get<T>()`), timed against the Codec layer for a 2-risk and a 400-risk document; prints the table in §7 |

Shared helpers live in `test/support/test_support.hpp` (JSON parsing, `decode` /
`encode` / `decodeError` wrappers that fail the test with the codec message).

## 7. Performance: Codec vs nlohmann/json

`test/perf/codec_perf_test.cpp` measures the same work five ways and prints a
table; run `codec_perf_tests.exe` or `ctest -R perf -V` to reproduce it. The
baseline is `nlohmann::ordered_json::parse` — the parser the port itself uses — so
the numbers show what the Codec layer adds *on top of* the parser. The two manual
columns are what you would write with nlohmann/json alone, either by hand or
through nlohmann's own ADL mapping (`from_json` + `get<T>()`); neither does any
validation.

Measured on this machine (Windows x64, MSVC 14.50, `Release`); every figure is the
**median of 5 full runs**, each run reporting the best of 7 calibrated rounds. The
fixture is the risk-definition document from §4. In Debug the large fixture is
scaled down to 40 risks so the layer stays fast (~3 s); the ratios are unchanged.

**2 risks (2 351 B)**

| operation | time | vs `ordered_json::parse` |
| --- | ---: | ---: |
| `nlohmann::json::parse` (unordered) | 10.7 µs | 0.83× |
| `nlohmann::ordered_json::parse` | 12.9 µs | 1.00× |
| parse + manual extraction | 14.4 µs | 1.12× |
| parse + `get<RiskDocument>()` (nlohmann ADL mapping) | 14.9 µs | 1.16× |
| **`JsonValue::parse` + codec decode** | **28.6 µs** | **2.22×** |
| manual extraction (pre-parsed) | 1.5 µs | 0.11× |
| `get<RiskDocument>()` (pre-parsed) | 1.8 µs | 0.14× |
| codec decode (pre-parsed `JsonValue`) | 15.5 µs | 1.20× |
| **codec encode + dump** | **31.2 µs** | **2.43×** |
| manual build + dump | 13.8 µs | 1.07× |
| `to_json` + dump | 12.3 µs | 0.95× |

**400 risks (379 611 B)**

| operation | time | vs `ordered_json::parse` |
| --- | ---: | ---: |
| `nlohmann::ordered_json::parse` | 3.19 ms | 1.00× |
| `nlohmann::json::parse` (unordered) | 3.56 ms | 1.12× (within noise) |
| parse + `get<RiskDocument>()` | 4.17 ms | 1.31× |
| parse + manual extraction | 4.98 ms | 1.56× |
| **`JsonValue::parse` + codec decode** | **10.43 ms** | **3.27×** |
| `get<RiskDocument>()` (pre-parsed) | 0.55 ms | 0.17× |
| manual extraction (pre-parsed) | 0.53 ms | 0.17× |
| codec decode (pre-parsed `JsonValue`) | 7.03 ms | 2.20× |
| **codec encode + dump** | **11.24 ms** | **3.53×** |
| `to_json` + dump | 3.55 ms | 1.11× |
| manual build + dump | 3.87 ms | 1.21× |

> **Methodology.** Absolute times drift ±20 % with machine load (the baseline moves
> with them, so the ratios are the meaningful part, and they stay within ~±15 %).
> The large fixture is sensitive to how much work the process did before: with
> `--gtest_filter=PerfTest.LargeDocument` alone, codec decode measures ~4.8 ms
> instead of ~7.0 ms. Rerun locally before drawing conclusions; the harness is
> there for relative comparisons, not for absolute claims.
>
> **The annotations are free on the success path.** Paths and codec frames (§3) are
> materialised only while an error travels outwards, so adding them did not change
> these numbers: re-measuring the same 5-run medians afterwards moved the large
> fixture from 7.03 to 6.68 ms (codec decode), 10.43 to 9.62 ms (parse + decode) and
> 11.24 to 10.77 ms (encode) — all inside the spread quoted above.

### Conclusion

1. **A full text → struct decode through the Codec layer costs 2.2× (small) to
   3.3× (large) a bare nlohmann parse.** Parsing is the floor and dominates; the
   Codec layer roughly *doubles* it. In absolute terms a 2-risk document decodes
   in ~29 µs, i.e. ~35 000 documents/s on one core.
2. **The Codec layer alone (pre-parsed DOM) is 1.2–2.2× the parser's own cost**,
   or **8.5–13× nlohmann's own `get<T>()` mapping**. That factor buys everything
   the plain mapping does not do: missing/extra key detection, type checks,
   numeric ranges, `error()` messages that name the failing field, partial
   results (`getOrThrow(allowPartial)`), `Lifecycle` tracking, and reusable
   composition — recursive records, `dispatch`, `either`, compressed maps. If
   those matter, ~1 µs per field is cheap; if they do not, use nlohmann directly
   (see below).
3. **Cost is linear in the number of fields**: 0.70 µs/field for the 2-risk
   document and 1.60 µs/field for the 400-risk one. The difference is cache and
   allocation pressure on a 380 KB DOM, not algorithmic — there is no superlinear
   behaviour.
4. **Encoding is 2.5× `to_json` for the small document and 3.2× for the large
   one**; a chunk of that is the builder overhead per field, and the Codec layer's
   *share* of an encode+serialise falls as documents grow (the `dump()` dominates
   less than the field-by-field work).
5. **`ordered_json` costs ~15–20 % more parse time than plain `json`** on the small
   document (within noise on the large one). That is the price of
   insertion-ordered objects, which the port needs for byte-identical
   re-encoding and for Gson's "replace member in place" semantics.
6. **Debug builds are ~30× slower in absolute terms** (CLion's default config)
   but the ratios hold: 2.3–2.9× the parser, 6.7–8.5× the manual path / `get<T>()`,
   encode 2.7×, and a stable 24 µs per field. Use Release for any performance
   conclusion; Debug is only for finding bugs.

### If all you need is JSON → struct, you do not need the Codec layer

nlohmann/json already maps JSON onto structs through ADL, and this port does not
stand in its way:

```cpp
nlohmann::ordered_json raw = nlohmann::ordered_json::parse(text);
RiskDocument document = raw.get<RiskDocument>();   // from_json found by ADL
```

That path costs **14.9 µs / 4.17 ms** for the two fixtures — **1.16× / 1.31× a
bare parse** — versus the Codec layer's 28.6 µs / 10.4 ms. So for a fixed schema
with no validation requirements, the Codec layer is 2–3× the parse for no benefit;
`get<T>()` or the hand-written extractor is the right tool, and the port is honest
about that (`perf.StrategiesAgreeOnTheFixture` checks that all four strategies
produce identical structures, so the comparison is apples to apples).

**This is not an argument against nlohmann/json, though** — quite the opposite:

* nlohmann is the *parse floor* both paths pay: of the codec's 28.6 µs, 12.9 µs is
  nlohmann's parse of the same text. Dropping nlohmann would not remove that cost,
  it would merely replace a battle-tested parser/serializer with a hand-written one
  (the first version of this port had one: ~450 lines that still had to get
  surrogate pairs, UTF-8 validation and shortest-round-trip float printing right).
* In this port nlohmann is confined to **one section behind one type** — section 1
  of the single header is the only place that mentions it:

  ```powershell
  > $jsonEnd = (Select-String -Path include/codec.hpp -Pattern '^// 2/8').LineNumber
  > (Select-String -Path include/codec.hpp -Pattern nlohmann).LineNumber -gt $jsonEnd
  # -> no output: every nlohmann reference lives in section 1 (lines 30..547)
  ```

  Sections 2–8 (`lifecycle`, `data_result`, `dynamic_ops`, `json_ops`, `codec`,
  `codecs`, `record_codec`) only ever see `Value`, `DynamicOps` and `Dynamic`.
  Adding another format means writing one more `DynamicOps` (with its own DOM); the
  JSON-specific code is confined to sections 1 and 5.
* The intermediate DOM is not an accident of using nlohmann either: DFU's
  `DynamicOps<T>`/`MapLike` contract is **random access**. `dispatch` reads the
  type key and then re-decodes *the same map* with the selected codec,
  `ListCodec` returns the raw failed elements as its partial result,
  `unboundedMap` consumes `MapLike::entries()`, and compressed maps rebuild a
  `MapLike` over a positional list. A forward-only token stream can serve none of
  those. Avoiding the DOM entirely means a streaming design (SAX or simdjson
  on-demand straight into structs) — a different library rather than a port of this
  one, and one that gives up the composition model that is the point of DFU.

In short: keep the Codec layer when you want validation, diagnostics and reusable
composition; keep nlohmann either way, because it is the JSON layer, and the Codec
layer is a *user* of it rather than an alternative to it.


### What the benchmark found

Writing the comparison paid for itself — it exposed two places where the first
implementation was *less* faithful than DFU rather than merely slower:

| Issue | Cause | Fix | Effect (400 risks) |
| --- | --- | --- | --- |
| Every member access deep-copied the JSON subtree | `JsonValue` had value semantics, but Gson's `JsonElement` is a **reference** type | `JsonValue` is now a handle: it shares ownership of the document and points at one node | decode 29.4 ms → ~7.0 ms (≈4×) |
| Encoding was quadratic | The builders copied their whole accumulator on every `add`, whereas DFU's `ImmutableList.Builder`/`JsonObject` are **mutable objects** carried by reference | Builder state is a `shared_ptr` to a mutable accumulator | encode 199 ms → ~11.2 ms (≈18×) |

Both were correctness-of-design issues (the port did not behave like DFU under
scale), not micro-optimisations, and both are covered by the existing tests plus
the new `perf.StrategiesAgreeOnTheFixture` equivalence check.
