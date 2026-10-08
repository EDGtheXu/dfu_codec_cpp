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

* Single-header library: [`include/codec.hpp`](include/codec.hpp) — one file, ~5 000 lines, CMake `INTERFACE` target, nothing to build
* Comments inside the header are written in **Chinese**; API names, error messages, test names and both READMEs stay English
* Layered tests: [`test/unit/`](test/unit) (155 cases), [`test/smoke/`](test/smoke) (23 cases), [`test/perf/`](test/perf) (3 cases, codec vs nlohmann/json benchmark) and the optional TOML layer (14 + 4 + 3 cases, 202 in total) — one executable each
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
| [tinytoml](https://github.com/mayah/tinytoml) | v0.4 | `third_party/tinytoml` (optional TOML layer only) |
| CMake | ≥ 3.16, Ninja or MSBuild | build |

The library itself is header-only, so there is nothing to configure — but because
its comments are Chinese, an MSVC invocation outside this project's CMake target
needs `/utf-8` (or `/source-charset:utf-8`); the `codec` target already adds it.

```powershell
# 1. fetch nlohmann/json + GoogleTest + tinytoml (add -Proxy http://127.0.0.1:7890 if needed)
powershell -File scripts/fetch_deps.ps1 [-Proxy http://127.0.0.1:7890]

# 2. build (uses MSVC via vcvars64.bat) and run the tests
powershell -File scripts/build.ps1 -RunTests
```

Or drive CMake directly:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure   # all layers, 202 cases
cmake --build build --target check           # same thing, one click/target
build/examples/risk_def_example.exe          # optional: sample document demo

# optional: also capture a real std::stacktrace for every error (switches to C++23)
cmake -S . -B cmake-build-stacktrace -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCODEC_RECORD_STACKTRACE=ON
```

| CMake option | Default | Effect |
| --- | --- | --- |
| `CODEC_BUILD_TESTS` | `ON` | build the test layers |
| `CODEC_BUILD_EXAMPLES` | `ON` | build the example programs |
| `CODEC_WARNINGS_AS_ERRORS` | `OFF` | `/WX`, `-Werror` |
| `CODEC_RECORD_STACKTRACE` | `OFF` | define `CODEC_RECORD_STACKTRACE`, compile as C++23 and capture `std::stacktrace` per error |
| `CODEC_BUILD_TOML` | `ON` | build the optional TOML layer (`codec_toml` target + its two test executables); needs `third_party/tinytoml` |

Each test layer is an independent executable, so it can also be run directly:

```powershell
build/test/unit/codec_unit_tests.exe          [--gtest_filter=RecordCodecTest.*]
build/test/smoke/codec_smoke_tests.exe        [--gtest_filter=SmokeTest.*]
build/test/perf/codec_perf_tests.exe          [--gtest_filter=PerfTest.SmallDocument]  # prints a table
build/test/codec_toml_unit_tests.exe          [--gtest_filter=TomlOpsTest.*]          # TOML layer
build/test/codec_toml_smoke_tests.exe         [--gtest_filter=TomlRiskDefTest.*]
build/test/codec_toml_perf_tests.exe          [--gtest_filter=TomlPerfTest.SmallDocument]  # prints a table
```

**Running from CLion.** The project is a normal CMake project: open the folder,
let CLion configure it, and every case shows up in the Run/Debug dropdown —
`All CTest` runs all layers, `unit.*` / `smoke.*` / `perf.*` / `toml_unit.*` /
`toml_smoke.*` / `toml_perf.*` group them, and each `TEST(...)` (including each
benchmark fixture) can be run or debugged individually. The `check` target is also
available in the target list. Both perf layers calibrate their own iteration counts
(the JSON layer ~6.4 s, the TOML layer ~5.6 s in Release, about half that in Debug);
`ctest -R perf -V` prints their tables.

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
`CODEC_WARNINGS_AS_ERRORS`, `CODEC_RECORD_STACKTRACE`, `CODEC_BUILD_TOML`. The
library target is `codec` (alias `codec::codec`); link it and `#include
"codec.hpp"`. The optional TOML layer is the separate `codec_toml` target
(`#include "codec_toml.hpp"`), which links `codec` and adds tinytoml's include
directory — the core header never sees tinytoml.

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

The core header's only compile-time dependency is
[nlohmann/json](#the-json-value-type) (a single header in `third_party/`, or your own
copy on the include path); the tests additionally need GoogleTest, which the
`INTERFACE` target does not propagate. The optional TOML layer adds exactly one more:
tinytoml, and only for `codec_toml` — see [the TOML layer](#the-toml-layer-codec_tomlhpp-optional).

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
`DynamicOps` implementation exchanges, so a second format's ops plugs into the
same codecs unchanged — which is exactly what the TOML layer below does. With
`JsonOps` the payload is a `JsonValue` node, so `value.asJson()` is the way back —
and the erasure costs nothing measurable: the 2-risk codec decode measured 15 423 ns
against the 15 500 ns it had before the change (see §7 and
[`docs/dynamic_ops_generic.md`](docs/dynamic_ops_generic.md)).

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

### The TOML layer (`codec_toml.hpp`, optional)

`JsonOps` is not special any more: since the ops layer exchanges the format-neutral
`Value`, a second format is one more `DynamicOps` and nothing else.
[`include/codec_toml.hpp`](include/codec_toml.hpp) is that ops for TOML, built on
[tinytoml](https://github.com/mayah/tinytoml) (vendored in `third_party/tinytoml`),
and **the same codecs decode both formats**:

```cpp
#include "codec_toml.hpp"

const DataResult<TomlDocument> document = codec::parseToml(text);
const DataResult<RiskDocument> risks =
    RiskDocumentCodec.parse(codec::TomlOps::INSTANCE, document.result()->root());

// ... and back out again (a TOML document always has a table at the root).
const DataResult<std::string> encoded = codec::dumpToml(encodedValue.result());
```

`TomlDocument` owns the parsed `toml::Value` (shared, like `JsonValue` owns its
document); `parseToml` reports tinytoml's own message (`Error: line 2: Invalid
token`), so a parse failure still carries a line number. `TomlOps::INSTANCE`
implements the whole `DynamicOps` surface — including `convertTo`, so values can be
moved between JSON and TOML in either direction (`JsonOps::INSTANCE.convertTo(
TomlOps::INSTANCE, value)` and back), which is what makes `Dynamic`/`Passthrough`
payloads portable across formats.

Type mapping (tinytoml → `codec::Value`):

| TOML | `DynamicOps` view | Reads as | Notes |
| --- | --- | --- | --- |
| `INT_TYPE` | number | `getNumberValue` (int) | tinytoml's `int64_t` |
| `DOUBLE_TYPE` | number | `getNumberValue` (double) | written back as `1.500000` |
| `BOOL_TYPE` | boolean | `getBooleanValue` | **not** a number — unlike `JsonOps`, which reads `true` as `1` (DFU's Gson behaviour) |
| `STRING_TYPE` | string | `getStringValue` | |
| `TIME_TYPE` | string | `getStringValue` | rendered, e.g. `1979-05-27T07:32:00Z`; not a number |
| `ARRAY_TYPE` | list | `getStream` / `getList` | must be homogeneous to be written back |
| `TABLE_TYPE` | map | `getMap` | keys are looked up with `findChild`, so a literal `"a.b"` is never treated as a path |

Two deliberate `TomlOps`/`JsonOps` differences are pinned by tests rather than
hidden: `valueEquals` is type-strict (`1 != 1.0`, while Gson's numbers compare
equal) and `mergeToMap` with a non-string key is an error (`key is not a string: 1`,
after the DFU `createMap`-skips / `mergeToMap`-errors split).

**Limits of this layer** (all of them tinytoml v0.4, not the port):

* **dotted keys are rejected at parse** (`a.b = 1` is an error); a *quoted* key
  `"a.b"` is a literal key and works, because `TomlOps` looks keys up by direct
  child rather than by path;
* arrays must be **homogeneous**, local times (`07:32:00`) are unsupported, and
  local dates come back normalised to UTC date-times;
* keys are written **sorted** (tinytoml stores a `std::map`), so re-encoding is not
  byte-identical to the input — decode → encode is stable, the file is not;
* `null` has no TOML counterpart: `dumpToml` rejects it and names the key. Use
  `optionalFieldOf` when a field may be absent (an absent key is simply not written);
* **codec errors carry the codec path, not TOML line/column numbers**
  (`risks[3].condition.op: expected string, got number`), because tinytoml keeps no
  source positions for values; only its parse errors have a line number;
* a scalar or a list at the root cannot be written (`must be a table at the root`) —
  a TOML document is a table, and the writer validates before rendering rather than
  emitting something that cannot be parsed back.

Costs, measured (§7): encoding is **linear** — the 2-risk sample encodes in ~24 µs and
a 400-risk document (413 KB of TOML) in ~4.1–4.4 ms — because `TomlOps` uses its own
mutable-accumulator builders rather than the generic ones that call
`mergeToList`/`mergeToMap` per element (those copy the whole container per call).
`dumpToml` is ~2.6 ms for the 400-risk document, and tinytoml's parser is roughly 4×
slower than nlohmann's (≈45 µs vs ≈12 µs for ~2 KB).

The layer is on by default (`CODEC_BUILD_TOML=ON`) but entirely separate: configure
with `-DCODEC_BUILD_TOML=OFF` and nothing in the project needs tinytoml.

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
include/codec_toml.hpp optional TOML layer: TomlDocument, parseToml, TomlOps, dumpToml
                       (one more DynamicOps; needs third_party/tinytoml, never included by codec.hpp)
models/risk_def.hpp    the risk-definition use case (codecs for RiskDef/Condition)
test/
  CMakeLists.txt       defines the layers + the one-click `check` target
  support/             shared test helpers (test_support.hpp)
  unit/                codec_unit_tests   -- component level suites (incl. the ODR guard)
  smoke/               codec_smoke_tests  -- end-to-end API checks
  perf/                codec_perf_tests   -- codec vs nlohmann/json measurements
  unit/toml_ops_test.cpp, smoke/toml_risk_def_test.cpp, perf/toml_perf_test.cpp
                       -- the TOML layer's three executables
examples/              risk_def_main.cpp
reference/dfu-6.0.8/   decompiled Java original (study only)
third_party/           nlohmann/json, googletest, tinytoml
scripts/               fetch_deps.ps1, build.ps1
```

## 3. Java → C++ mapping

| DataFixerUpper (Java) | This port (C++) |
| --- | --- |
| `Codec<A>`, `MapCodec<A>` (interfaces) | copyable value types holding `std::function` wrappers |
| `Encoder<A>`, `Decoder<A>`, `MapEncoder<A>`, `MapDecoder<A>` | same four wrappers, same combinators |
| `Codec.of(encoder, decoder)` / `MapCodec.of(...)` | identical factories |
| `MapCodec.MapCodecCodec` | `Codec` created from a `MapCodec` (implicit conversion, `Codec::mapCodec()` detects it) |
| `DynamicOps<T>` (`T` = `JsonElement`, `NbtTag`, …) | abstract `DynamicOps` over the format-neutral `Value`; `JsonOps` (nlohmann) and `TomlOps` (`codec_toml.hpp`) implement it |
| `DynamicOps.convertTo(otherOps, value)` | same, and a real conversion: `JsonOps` ⇄ `TomlOps` in either direction |
| `Dynamic<T>` | `codec::Dynamic`: a `Value` plus the ops that understands it (`Passthrough` carries one) |
| `JsonOps.INSTANCE` / `JsonOps.COMPRESSED` | `JsonOps::INSTANCE` / `JsonOps::COMPRESSED` |
| `JsonOps` (Gson-backed) | `JsonOps` over `JsonValue` (nlohmann/`ordered_json`-backed), plus `TomlOps` over tinytoml |
| `DataResult<R>` (`Either<R, PartialResult<R>>`) | `DataResult<R>` (optional value + optional error, partial value kept) |
| `Lifecycle` | `Lifecycle` (same `add` rules) |
| `RecordCodecBuilder.create(i -> i.group(f1, f2).apply(i, Ctor::new))` | `record<O>(f1, f2)` or `record<O>(ctor, f1, f2)` |
| `Codec.STRING.fieldOf("id").forGetter(RiskDef::id)` | `fieldOf("id", &RiskDef::id, codecs::String)` or `codecs::String.fieldOf("id").forGetter(&RiskDef::id)` |
| `Codec.optionalFieldOf("x", default)` | `optionalFieldOf("x", &O::member, codec, defaultValue)` |
| `Codec.listOf()`, `Codec.either(a, b)`, `Codec.pair(a, b)`, `Codec.unboundedMap(k, v)` | `listOf(codec)`, `either(a, b)`, `pair(a, b)`, `unboundedMap(k, v)` |
| `codec.dispatch` / `partialDispatch` / `dispatchMap` | same names (`KeyDispatchCodec`) |
| `Codec.intRange` / `floatRange` / `doubleRange` | same names |
| `Codec.checkRange` | inlined into the range codecs |
| `Stream<T>` in `DynamicOps` | `std::vector<Value>` (materialised) |
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
`promotePartial`, `setPartial`, `mapError`, `resultOrPartial`, `getOrThrow`),
`Dynamic` (the value + ops pair, which is what `Passthrough` carries) and
the full `Codec`/`MapCodec` combinator surface (`xmap`, `flatXmap`,
`comapFlatMap`, `flatComapMap`, `orElse`, `orElseGet`, `mapResult`, `withLifecycle`,
`stable`, `deprecated`, `fieldOf`, `optionalFieldOf`, `promotePartial`).

Out of scope (DFU packages that build *on* Codec): `DataFixer`, `Schema`,
`TypeRewriteRule`, the optics/profunctor machinery and `NbtOps` (the TOML ops lives
in `codec_toml.hpp` instead, and shows what an `NbtOps` would cost: one more
`DynamicOps`).

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
| `DynamicOps` value type | generic `T` (JsonElement, NbtTag, …) | a type-erased `Value` handle (owner + node + tag), plus `Dynamic` (value + its ops) as the `Passthrough` carrier | the ops layer is format-neutral; `JsonValue` is the JSON DOM behind `JsonOps` and `TomlDocument`/`TomlOps` (in `codec_toml.hpp`) the TOML one, and `convertTo` is a real conversion between them (see [`docs/dynamic_ops_generic.md`](docs/dynamic_ops_generic.md)) |
| A second `DynamicOps` | one per format inside DFU (`JsonOps`, `NbtOps`) | `JsonOps` in the core header; `TomlOps` in the optional `codec_toml.hpp`, not linked by `codec` | proves the erasure really is format-neutral, and keeps tinytoml out of the core header |
| `JsonOps.valueEquals` (numbers) | Gson numbers compare by value | unchanged: `1 == 1.0` | faithfulness; `TomlOps` is type-strict instead (`1 != 1.0`), because tinytoml's `operator==` is |
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

202 GoogleTest cases in six independent executables. `ctest` prefixes each case
with its layer (`unit.*`, `smoke.*`, `perf.*`, `toml_unit.*`, `toml_smoke.*`,
`toml_perf.*`), so any layer can be selected as a group.

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

**The TOML layer has its own executables** (built only when `CODEC_BUILD_TOML=ON`, so
the core layers stay free of the tinytoml dependency):

| File | Focus |
| --- | --- |
| `unit/toml_ops_test.cpp` → `codec_toml_unit_tests` (14 cases) | `TomlOps` as a `DynamicOps`: scalar reads by type, `BooleanIsNotANumber`, dates as strings, literal `"a.b"` keys via `findChild`, type-strict equality, `convertTo` both ways (and same-ops identity), `dumpToml` rejecting non-table roots / `null` / mixed arrays, parse errors carrying `line 2`, the v0.4 limits (dotted keys, mixed arrays, local time), empty document, list/map merging with strict keys, and the encoding builders (`TomlListBuilder`/`TomlRecordBuilder`): prefix merging without mutating the prefix, accumulator reuse, last-wins on duplicate keys, error propagation through `add`/`withErrorsFrom`/`mapError`, non-string keys, foreign (JSON) handles |
| `smoke/toml_risk_def_test.cpp` → `codec_toml_smoke_tests` (4 cases) | the *same* `RiskDocumentCodec` on TOML and JSON (identical decoded values), dump → reparse stability, a `Passthrough` `Dynamic` moving JSON → TOML → JSON, TOML values converted back to JSON |
| `perf/toml_perf_test.cpp` → `codec_toml_perf_tests` (3 cases) | the TOML layer's cost: `parseToml`, parse + decode, pre-parsed decode, encode, encode + `dumpToml`, both `convertTo` directions, and `per-risk encode` — the guard against encoding sliding back to the generic builders (see §7) |

The TOML perf layer is a separate executable on purpose: adding cases to
`codec_perf_tests` would perturb the JSON numbers it has recorded (code layout alone
is worth ~3 %, see §7) and would drag tinytoml into the core perf binary.

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
> **A single row is also sensitive to code layout.** Rebuilding the *identical*
> source with one extra, never-called translation unit moved `codec decode
> (pre-parsed)` by 1.9 %, and a two-binary A/B of the same perf source against two
> revisions of `include/codec.hpp` spread that row over 3.4 % — while the parser row
> (byte-identical code) moved 4.7 % in the *opposite* direction. So prefer the
> end-to-end `parse + decode` row, and compare same-run ratios rather than one row
> across two binaries; the experiment is written up in
> [`docs/dynamic_ops_generic.md`](docs/dynamic_ops_generic.md) §4.3. A re-measurement
> during the TOML work (same machine, later day) put `ordered_json::parse` at
> 11.5–11.8 µs and the pre-parsed decode at 15.7–16.0 µs: the parser moved ~9 %, the
> decode ~2 %.
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

### The same trap, once more: the TOML layer

The second `DynamicOps` walked into it too, and acceptance caught it: `TomlOps`
initially left `listBuilder()`/`mapBuilder()` at their generic defaults, which call
`mergeToList`/`mergeToMap` once per element — and the TOML versions of those copy
the whole accumulated `toml::Array`/`toml::Table` on every call, so **encoding was
quadratic**: 400 risks took **248 ms** (each doubling of N quadrupled the time),
while `dumpToml` stayed linear at 1.8 ms. `TomlOps` now has TOML-side mutable
accumulators (`TomlListBuilder`/`TomlRecordBuilder`, same shape and error semantics
as `JsonOps`' `ArrayListBuilder`/`StringRecordBuilder`), and `mergeToList`/
`mergeToMap` keep their DFU per-call-copy semantics for direct calls.

Measured with `test/perf/toml_perf_test.cpp` and a scaling bench (best of 5, Release,
milliseconds); `N` doubling now doubles the time across the board:

| risks | encode | × | dumpToml | × | parseToml | decode | × |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 25 | 0.24 | — | 0.16 | — | 0.25 | 0.12 | — |
| 50 | 0.48 | 2.02× | 0.31 | 2.01× | 0.50 | 0.23 | 2.01× |
| 100 | 1.08 | 2.26× | 0.63 | 2.00× | 1.01 | 0.48 | 2.04× |
| 200 | 2.17 | 2.01× | 1.30 | 2.06× | 2.17 | 1.16 | 2.44× |
| 400 | **4.42** | 2.04× | 2.57 | 1.98× | 4.42 | 2.19 | 1.89× |

So a 400-risk document (**413 KB of TOML**) encodes in ~4.4 ms and dumps in ~2.6 ms;
encoding went from ~248 ms (single measurement; 215–219 ms in a same-process
best-of-5 A/B) to ~4.1–4.4 ms. The 2-risk sample costs ~24 µs to encode and ~45 µs
to parse (decode ~15 µs, the same order as `JsonOps`).

Two honest notes about this layer's costs: **tinytoml's parser is roughly 4× slower
than nlohmann's** on a comparable document (≈45 µs vs ≈12 µs for ~2 KB), and every
`mergeToList`/`mergeToMap` call remains O(container) by design — the builders are what
keep the codec path linear, so if you build TOML values by calling `mergeToMap` in your
own loop, you are the one paying that copy.

Like the JSON side, the TOML perf layer's numbers are printed, never asserted:
`ctest -R toml_perf -V`, and watch `per-risk encode` (~20 µs/risk for the 400-risk
fixture) — a slide back to the generic builders shows up there as ~550 µs/risk.
