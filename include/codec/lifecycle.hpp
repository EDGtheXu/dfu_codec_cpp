// A C++17 port of Mojang's DataFixerUpper `com.mojang.serialization` Codec API.
//
// lifecycle.hpp -- port of com.mojang.serialization.Lifecycle.
#pragma once

#include <string>
#include <utility>

namespace codec {

// Lifecycle tracks whether a (de)serialization path touched anything that is
// not guaranteed to stay stable forever.  It mirrors DFU's Lifecycle exactly,
// including the "experimental wins" combination rule and the
// "lowest `since` deprecated value wins" rule.
class Lifecycle {
 public:
  enum class Kind { Stable, Experimental, Deprecated };

  Lifecycle() = default;

  static Lifecycle stable() { return Lifecycle(Kind::Stable, 0); }
  static Lifecycle experimental() { return Lifecycle(Kind::Experimental, 0); }
  static Lifecycle deprecated(int since) { return Lifecycle(Kind::Deprecated, since); }

  Kind kind() const { return kind_; }
  bool isStable() const { return kind_ == Kind::Stable; }
  bool isExperimental() const { return kind_ == Kind::Experimental; }
  bool isDeprecated() const { return kind_ == Kind::Deprecated; }
  int since() const { return since_; }

  // Lifecycle.add(other)
  Lifecycle add(const Lifecycle& other) const {
    if (kind_ == Kind::Experimental || other.kind_ == Kind::Experimental) {
      return experimental();
    }
    if (kind_ == Kind::Deprecated) {
      if (other.kind_ == Kind::Deprecated && other.since_ < since_) {
        return other;
      }
      return *this;
    }
    if (other.kind_ == Kind::Deprecated) {
      return other;
    }
    return stable();
  }

  std::string toString() const {
    switch (kind_) {
      case Kind::Stable:
        return "Stable";
      case Kind::Experimental:
        return "Experimental";
      case Kind::Deprecated:
        return "Deprecated[" + std::to_string(since_) + "]";
    }
    return "Unknown";
  }

  bool operator==(const Lifecycle& other) const {
    return kind_ == other.kind_ && (kind_ != Kind::Deprecated || since_ == other.since_);
  }
  bool operator!=(const Lifecycle& other) const { return !(*this == other); }

 private:
  Lifecycle(Kind kind, int since) : kind_(kind), since_(since) {}

  Kind kind_ = Kind::Experimental;
  int since_ = 0;
};

}  // namespace codec
