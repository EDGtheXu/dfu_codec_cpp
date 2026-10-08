// 方案 B 阶段 0：正确性检查 + 微基准。
//
//   cmake -S . -B build-poc -G Ninja -DCMAKE_BUILD_TYPE=Release -DCODEC_BUILD_PROTOTYPE=ON
//   cmake --build build-poc && build-poc/prototype/codec_generic_ops_poc.exe
//
// 检查：
//   1. 两条路径（JsonValue 直接收发 vs 擦除句柄）解出同一个结构体；
//   2. 装箱零分配：反复把同一个文档节点装进擦除句柄，分配计数必须为 0；
//   3. 标签检查正确：JSON 句柄不是 ToyNode，反之亦然；
//   4. convertTo 真的能跨格式搬运：JSON → 玩具 DOM → 回到 JSON 后逐字节相同，
//      并且用同一个 decode 函数在玩具 DOM 上解出同一个结构体。
// 基准：
//   * 解码 ns/op（A: 直接收发 JsonValue；B: 擦除句柄）；
//   * 句柄尺寸对遍历的影响（24B vs 32B，1M 个句柄的顺序求和）；
//   * 标签检查本身的 ns/op。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "generic_ops.hpp"

// ---------------------------------------------------------------------------
// 全局分配计数（证明"装箱不分配"）
// ---------------------------------------------------------------------------
namespace {
std::atomic<long long> g_allocations{0};
}

void* operator new(std::size_t size) {
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* pointer = std::malloc(size)) {
    return pointer;
  }
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  return std::malloc(size);
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
  return ::operator new(size, tag);
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }

namespace {

long long allocations() { return g_allocations.load(std::memory_order_relaxed); }

int g_failures = 0;

void check(bool condition, const std::string& what) {
  std::printf("%-6s %s\n", condition ? "[ok]" : "[FAIL]", what.c_str());
  if (!condition) {
    ++g_failures;
  }
}

// 每个 round 计时，返回最好一次（与 README §7 的方法一致）
template <class Fn>
double timeBestNsPerOp(Fn&& body, int64_t iterations, int rounds = 7) {
  double best = 1e300;
  for (int round = 0; round < rounds; ++round) {
    const auto start = std::chrono::steady_clock::now();
    body(iterations);
    const auto stop = std::chrono::steady_clock::now();
    const double ns =
        std::chrono::duration_cast<std::chrono::duration<double, std::nano>>(stop - start).count();
    best = std::min(best, ns / static_cast<double>(iterations));
  }
  return best;
}

const char* kDocument =
    R"({"id":"r-1","severity":3,"tags":["a","b","c"],"nested":{"x":7,"y":9}})";

}  // namespace

int main() {
  using proto::Erased;
  using proto::ErasedOps;
  using proto::JsonDirectOps;
  using proto::JsonErasedOps;
  using proto::Raw;
  using proto::Row;
  using proto::ToyOps;
  using proto::Value24;
  using proto::Value32;

  std::printf("句柄尺寸: JsonValue=%zu  Value24=%zu  Value32=%zu   选中句柄=%s (%zu 字节)\n",
              sizeof(codec::JsonValue), sizeof(Value24), sizeof(Value32),
#if PROTO_TAGGED_HANDLE
              "Value32（带标签）", sizeof(Erased));
#else
              "Value24（标签由 ops 持有）", sizeof(Erased));
#endif
  std::printf("文档: %s\n\n", kDocument);

  // ---------------------------------------------------------------------
  // 1. 两条路径解出同一个结构体
  // ---------------------------------------------------------------------
  const auto owner = std::make_shared<const Raw>(Raw::parse(kDocument));
  const codec::JsonValue jsonDoc(owner, owner.get());
  const Erased erasedDoc = Erased::of<Raw>(owner, owner.get());

  const JsonDirectOps direct;
  const JsonErasedOps erased;
  const Row rowDirect = proto::decodeDirect(direct, jsonDoc);
  const Row rowErased = proto::decodeErased(erased, erasedDoc);

  check(rowDirect == rowErased, "两条路径解出同一个结构体 (id/severity/tags/nested)");
  std::printf("       A: id=%s severity=%lld tags=%zu nested=(%lld,%lld)\n", rowDirect.id.c_str(),
              static_cast<long long>(rowDirect.severity), rowDirect.tags.size(),
              static_cast<long long>(rowDirect.nestedX), static_cast<long long>(rowDirect.nestedY));
  check(rowDirect.id == "r-1" && rowDirect.severity == 3 && rowDirect.tags.size() == 3 &&
            rowDirect.nestedX == 7 && rowDirect.nestedY == 9,
        "解出的字段值正确");

  // ---------------------------------------------------------------------
  // 2. 装箱零分配
  // ---------------------------------------------------------------------
  {
    const long long before = allocations();
    const long long stop = 200000;
    long long sink = 0;
    for (long long i = 0; i < stop; ++i) {
      const Erased boxed = Erased::of<Raw>(owner, owner.get());
      sink += static_cast<long long>(boxed.valid());
    }
    const long long used = allocations() - before;
    check(used == 0, "20 万次装箱（复用 JsonValue 的 shared_ptr 所有者）分配次数 = " +
                         std::to_string(used));
    if (sink == 0) {
      std::printf("(sink %lld)\n", sink);
    }
  }

  // ---------------------------------------------------------------------
  // 3. 标签检查（只有 32 字节句柄才有；24 字节版把这件事交给 ops）
  // ---------------------------------------------------------------------
#if PROTO_TAGGED_HANDLE
  {
    const Erased toy = ToyOps::box(proto::toyInt(1));
    check(erasedDoc.is<Raw>() && !erasedDoc.is<proto::ToyNode>(), "JSON 句柄的标签是 Raw 而不是 ToyNode");
    check(toy.is<proto::ToyNode>() && !toy.is<Raw>(), "玩具句柄的标签是 ToyNode 而不是 Raw");
    check(erasedDoc.as<proto::ToyNode>() == nullptr, "标签不匹配时 as<T>() 返回 nullptr");
  }
#else
  std::printf("[skip] 标签检查：24 字节句柄不带标签（类型正确性由 ops 保证）\n");
#endif

  // ---------------------------------------------------------------------
  // 4. convertTo：JSON → 玩具 DOM → 回到 JSON，并跨 ops 解码
  // ---------------------------------------------------------------------
  {
    const ToyOps toy;
    const Erased toyDoc = erased.convertTo(toy, erasedDoc);
    const Erased backToJson = toy.convertTo(erased, toyDoc);
    const codec::JsonValue roundTripped = JsonErasedOps::view(backToJson);

    check(roundTripped.dump() == jsonDoc.dump(),
          "JSON → ToyOps → JSON 逐字节相同: " + roundTripped.dump());

    const Row rowToy = proto::decodeErased(toy, toyDoc);
    check(rowDirect == rowToy, "同一个 decode 函数在玩具 DOM 上解出同一个结构体（跨格式）");
  }

  // ---------------------------------------------------------------------
  // 基准：解码 ns/op
  // ---------------------------------------------------------------------
  const int64_t iterations = 200000;
  const double directNs = timeBestNsPerOp(
      [&](int64_t count) {
        Row sink{};
        for (int64_t i = 0; i < count; ++i) {
          sink = proto::decodeDirect(direct, jsonDoc);
        }
        if (sink.severity == 12345) {
          std::printf(" ");
        }
      },
      iterations);
  const double erasedNs = timeBestNsPerOp(
      [&](int64_t count) {
        Row sink{};
        for (int64_t i = 0; i < count; ++i) {
          sink = proto::decodeErased(erased, erasedDoc);
        }
        if (sink.severity == 12345) {
          std::printf(" ");
        }
      },
      iterations);

  std::printf("\n解码（4 字段 + 3 元素数组 + 2 字段嵌套对象，各取一次 map）\n");
  std::printf("  A 直接收发 JsonValue : %8.1f ns/op\n", directNs);
  std::printf("  B 擦除句柄 (%2zu 字节) : %8.1f ns/op   %+.1f%%\n", sizeof(Erased), erasedNs,
              (erasedNs / directNs - 1.0) * 100.0);

  // 分配数/op（解码本身）
  const long long beforeDirect = allocations();
  for (int64_t i = 0; i < 1000; ++i) {
    (void)proto::decodeDirect(direct, jsonDoc);
  }
  const long long directAllocs = allocations() - beforeDirect;
  const long long beforeErased = allocations();
  for (int64_t i = 0; i < 1000; ++i) {
    (void)proto::decodeErased(erased, erasedDoc);
  }
  const long long erasedAllocs = allocations() - beforeErased;
  std::printf("  分配/op: A=%.2f  B=%.2f\n", directAllocs / 1000.0, erasedAllocs / 1000.0);

  // ---------------------------------------------------------------------
  // 基准：句柄尺寸对顺序遍历的影响（1M 个句柄）
  // ---------------------------------------------------------------------
  {
    constexpr size_t kCount = 1u << 20;
    auto array = std::make_shared<Raw>(Raw::array());
    for (size_t i = 0; i < kCount; ++i) {
      array->push_back(static_cast<int64_t>(i));
    }
    const std::shared_ptr<const Raw> bigOwner = array;

    std::vector<Value24> small;
    std::vector<Value32> large;
    small.reserve(kCount);
    large.reserve(kCount);
    for (size_t i = 0; i < kCount; ++i) {
      const Raw* node = &(*array)[i];
      small.push_back(Value24::of<Raw>(bigOwner, node));
      large.push_back(Value32::of<Raw>(bigOwner, node));
    }

    const double smallNs = timeBestNsPerOp(
        [&](int64_t count) {
          long long sink = 0;
          for (int64_t round = 0; round < count / static_cast<int64_t>(kCount); ++round) {
            for (const Value24& value : small) {
              sink += static_cast<long long>(reinterpret_cast<std::uintptr_t>(value.node));
            }
          }
          if (sink == 1) {
            std::printf(" ");
          }
        },
        static_cast<int64_t>(kCount) * 8);
    const double largeNs = timeBestNsPerOp(
        [&](int64_t count) {
          long long sink = 0;
          for (int64_t round = 0; round < count / static_cast<int64_t>(kCount); ++round) {
            for (const Value32& value : large) {
              sink += static_cast<long long>(reinterpret_cast<std::uintptr_t>(value.node));
            }
          }
          if (sink == 1) {
            std::printf(" ");
          }
        },
        static_cast<int64_t>(kCount) * 8);

    std::printf("\n顺序遍历 1M 个句柄（纯内存足迹 + 标签检查）\n");
    std::printf("  Value24 (24B) : %6.3f ns/元素\n", smallNs);
    std::printf("  Value32 (32B) : %6.3f ns/元素   %+.1f%%\n", largeNs,
                (largeNs / smallNs - 1.0) * 100.0);
  }

  // ---------------------------------------------------------------------
  // 基准：标签检查本身（只有 32 字节句柄会做；24 字节版用空转做对照）
  // ---------------------------------------------------------------------
  {
    long long matches = 0;
    const double ns = timeBestNsPerOp(
        [&](int64_t count) {
          matches = 0;
          for (int64_t i = 0; i < count; ++i) {
#if PROTO_TAGGED_HANDLE
            matches += erasedDoc.is<Raw>() ? 1 : 0;
#else
            matches += erasedDoc.valid() ? 1 : 0;
#endif
          }
        },
        50000000);
    std::printf("\n每次取值的句柄校验 : %.3f ns/次（50M 次，%s）\n", ns,
#if PROTO_TAGGED_HANDLE
                "32 字节：标签比较 is<T>()");
#else
                "24 字节：仅 valid()，不做标签比较");
#endif
    if (matches == 1) {
      std::printf(" ");
    }
  }

  std::printf("\n%s\n", g_failures == 0 ? "全部检查通过" : "有检查失败");
  return g_failures == 0 ? 0 : 1;
}
