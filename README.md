# Codec — a C++17 port of Mojang's DataFixerUpper `Codec` API

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

* Header-only library: [`include/codec/`](include/codec)
* Layered tests: [`test/unit/`](test/unit) (114 cases), [`test/smoke/`](test/smoke) (21 cases) and [`test/perf/`](test/perf) (3 cases, codec vs nlohmann/json benchmark) — one executable each
* Reference use case (the risk-definition document): [`models/risk_def.hpp`](models/risk_def.hpp)
* Runnable example: [`examples/risk_def_main.cpp`](examples/risk_def_main.cpp)

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

## 1. Dependencies and build

| Dependency | Version | Where |
| --- | --- | --- |
| C++ | **C++17** (`/std:c++17`, `-std=c++17`) | required |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 (single header) | `third_party/nlohmann/json.hpp` |
| [GoogleTest](https://github.com/google/googletest) | 1.17.0 | `third_party/googletest-1.17.0` (tests only) |
| CMake | ≥ 3.16, Ninja or MSBuild | build |

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
ctest --test-dir build --output-on-failure   # all layers, 138 cases
cmake --build build --target check           # same thing, one click/target
build/examples/risk_def_example.exe          # optional: sample document demo
```

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
link it and `#include "codec/all.hpp"`.

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
include/codec/
  json.hpp          JsonValue (nlohmann-backed) + Number (java.lang.Number)
  lifecycle.hpp     Lifecycle
  data_result.hpp   DataResult, PartialResult semantics, Unit
  dynamic_ops.hpp   DynamicOps, MapLike, RecordBuilder, ListBuilder, KeyCompressor
  json_ops.hpp      JsonOps (INSTANCE / COMPRESSED)
  codec.hpp         Encoder, Decoder, MapEncoder, MapDecoder, Codec, MapCodec
  codecs.hpp        primitive + composite codecs, range checks, recursive, dispatch
  record_codec.hpp  RecordCodecBuilder: record<>, fieldOf, optionalFieldOf, forGetter
  all.hpp           umbrella header
models/risk_def.hpp    the risk-definition use case (codecs for RiskDef/Condition)
test/
  CMakeLists.txt       defines the layers + the one-click `check` target
  support/             shared test helpers (test_support.hpp)
  unit/                codec_unit_tests   -- component level suites
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
| `Unit`, `PASSTHROUGH`, `EMPTY`, `Codec.unit` | `codec::Unit`, `codecs::Passthrough`, `codecs::Empty`, `Codec<T>::unit` |

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
  std::optional<JsonValue> value;      // any JSON value (Passthrough)
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
* decode → encode → decode is stable, and a hand-built document round-trips.

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

Intentional deviations, all documented in the headers:

| Area | DFU | Port | Why |
| --- | --- | --- | --- |
| `DynamicOps` value type | generic `T` (JsonElement, NbtTag, …) | single `JsonValue` | one concrete DOM; `convertTo` is the identity |
| `Stream<T>` | lazy Java streams | `std::vector` | no lazy streams in the standard library |
| `KeyCompressor.compress` | unknown key → `0` (fastutil default) | unknown key → `-1` → treated as absent | avoids silently reading index 0 |
| `UnboundedMapCodec` duplicates | `ImmutableMap.Builder` throws | last-wins, insertion order kept | keeps decoding usable |
| `CompressedMapLike` out-of-range index | `IndexOutOfBoundsException` | treated as absent | no exceptions in the decode path |
| `Encoder.error(msg)` | appends the value's `toString` | uses the message verbatim | C++ has no universal `toString` |
| `Codec.optionalFieldOf(name, Lifecycle, …)` | 4-argument overload | not ported | rarely used; `.stable()` covers it |

## 6. Test layers

138 GoogleTest cases in three independent executables. `ctest` prefixes each case
with its layer (`unit.*`, `smoke.*`, `perf.*`), so any layer can be selected as a
group.

**`test/unit/` → `codec_unit_tests` (114 cases)** — component level, exhaustive
on edge cases:

| File | Focus |
| --- | --- |
| `json_test.cpp` | nlohmann-backed DOM, parse/dump round-trips, escapes/unicode, Gson equality, `Number` narrowing |
| `nlohmann_interop_test.cpp` | `JsonValue` ⇄ `nlohmann::ordered_json`, codecs fed from nlohmann documents |
| `data_result_test.cpp` | `Lifecycle.add` rules, success/error/partial, `map`/`flatMap`/`apply2`/`apply3`, `promotePartial`, `getOrThrow` |
| `primitives_test.cpp` | `Bool`/`Byte`/`Short`/`Int`/`Long`/`Float`/`Double`/`String`/`Passthrough`, `mergeToPrimitive` |
| `dynamic_ops_test.cpp` | `JsonOps` primitives, `mergeToList/Map`, `MapLike` null rules, builders, `KeyCompressor`, compressed maps |
| `codec_combinators_test.cpp` | `xmap`/`flatXmap`/`comapFlatMap`/`flatComapMap`, `orElse`, `mapResult`, `either`, `pair`, `listOf`, `unboundedMap`, ranges, unit codecs, map-codec combinators |
| `record_codec_test.cpp` | `record<>` in all forms, `fieldOf`/`optionalFieldOf`/`forGetter`, error joining, partial objects, keys, compression |
| `dispatch_test.cpp` | `KeyDispatchCodec` (`partialDispatch`/`dispatch`/`dispatchMap`), map-codec payload merging, compressed dispatch |

**`test/smoke/` → `codec_smoke_tests` (21 cases)** — small and fast end-to-end
passes that answer "does the port work at all?":

| File | Focus |
| --- | --- |
| `library_smoke_test.cpp` | one document using *every* codec kind decoded/encoded/re-decoded, malformed-input rejection, compressed ⇄ plain ops agreement, partial results |
| `risk_def_test.cpp` | the reference risk-definition document end to end (byte-identical re-encode, recursive conditions, error reporting) |

**`test/perf/` → `codec_perf_tests` (3 cases)** — measurement, not a gate (the
only assertions are that all strategies produce the same result):

| File | Focus |
| --- | --- |
| `codec_perf_test.cpp` | hand-written nlohmann extractor/builder, timed comparison against the Codec layer for a 2-risk and a 400-risk document; prints the table described in §7 |

Shared helpers live in `test/support/test_support.hpp` (JSON parsing, `decode` /
`encode` / `decodeError` wrappers that fail the test with the codec message).

## 7. Performance: Codec vs nlohmann/json

`test/perf/codec_perf_test.cpp` measures the same work four ways and prints a
table; run `codec_perf_tests.exe` or `ctest -R perf -V` to reproduce it. The
baseline is `nlohmann::ordered_json::parse` — the parser the port itself uses —
so the numbers show what the Codec layer adds *on top of* the parser. The manual
column is a hand-written nlohmann extractor/builder: what you would write with
nlohmann/json alone. It deliberately does **no** validation.

Measured on this machine (Windows x64, MSVC 14.50, `Release`, best of 5
calibrated runs). The fixture is the risk-definition document from §4.

**2 risks (2 351 B)**

| operation | time | vs `ordered_json::parse` |
| --- | ---: | ---: |
| `nlohmann::json::parse` (unordered) | 10.8 µs | 0.85× |
| `nlohmann::ordered_json::parse` | 12.7 µs | 1.00× |
| parse + manual extraction | 14.2 µs | 1.12× |
| **`JsonValue::parse` + codec decode** | **29.2 µs** | **2.30×** |
| codec decode (pre-parsed `JsonValue`) | 15.6 µs | 1.23× |
| manual extraction (pre-parsed) | 1.4 µs | 0.11× |
| **codec encode + dump** | **30.0 µs** | **2.37×** |
| manual build + dump | 13.2 µs | 1.04× |

**400 risks (379 611 B)**

| operation | time | vs `ordered_json::parse` |
| --- | ---: | ---: |
| `nlohmann::ordered_json::parse` | 2.48 ms | 1.00× |
| parse + manual extraction | 3.16 ms | 1.28× |
| **`JsonValue::parse` + codec decode** | **7.43 ms** | **3.00×** |
| codec decode (pre-parsed `JsonValue`) | 4.84 ms | 1.95× |
| manual extraction (pre-parsed) | 0.36 ms | 0.15× |
| **codec encode + dump** | **8.32 ms** | **3.36×** |
| manual build + dump | 5.36 ms | 2.16× |

### Conclusion

1. **A full text → struct decode costs 2.3× (small) to 3.0× (large) a bare
   nlohmann parse.** Parsing is the floor and dominates; the Codec layer roughly
   *doubles* it. In absolute terms a 2-risk document decodes in ~29 µs, i.e.
   ~34 000 documents/s on one core.
2. **The Codec layer alone (pre-parsed DOM) is 1.2–2.0× the parser's own cost**,
   or **7–13× a hand-written nlohmann extractor**. That factor buys everything
   the manual extractor does not do: missing/extra key detection, type checks,
   numeric ranges, `error()` messages that name the failing field, partial
   results (`getOrThrow(allowPartial)`), `Lifecycle` tracking, and reusable
   composition — recursive records, `dispatch`, `either`, compressed maps. If
   those matter, 1 µs per field is cheap; if they do not, use nlohmann directly.
3. **Cost is linear in the number of fields**: 0.71 µs/field for the 2-risk
   document and 1.10 µs/field for the 400-risk one (the difference is cache
   pressure on a 380 KB DOM). No superlinear behaviour.
4. **Encoding is 2.3× manual building for the small document but only 1.55× for
   the large one**, because `dump()` dominates once the document is big — the
   Codec layer's share of an encode+serialise shrinks with size.
5. **`ordered_json` costs ~15 % more parse time than plain `json`** on the small
   document (3 % on the large one). That is the price of insertion-ordered
   objects, which the port needs for byte-identical re-encoding and for Gson's
   "replace member in place" semantics.
6. **Debug builds are ~30× slower in absolute terms** (CLion's default config)
   but the ratios hold: 2.5–4.0× the parser, 6.7–8.3× the manual path, encode
   2.5×. Use Release for any performance conclusion; Debug is only for finding
   bugs.

### What the benchmark found

Writing the comparison paid for itself — it exposed two places where the first
implementation was *less* faithful than DFU rather than merely slower:

| Issue | Cause | Fix | Effect (400 risks) |
| --- | --- | --- | --- |
| Every member access deep-copied the JSON subtree | `JsonValue` had value semantics, but Gson's `JsonElement` is a **reference** type | `JsonValue` is now a handle: it shares ownership of the document and points at one node | decode 29.4 ms → 4.8 ms |
| Encoding was quadratic | The builders copied their whole accumulator on every `add`, whereas DFU's `ImmutableList.Builder`/`JsonObject` are **mutable objects** carried by reference | Builder state is a `shared_ptr` to a mutable accumulator | encode 199 ms → 8.3 ms |

Both were correctness-of-design issues (the port did not behave like DFU under
scale), not micro-optimisations, and both are covered by the existing tests plus
the new `perf.StrategiesAgreeOnTheFixture` equivalence check.
