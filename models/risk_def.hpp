// The reference use case for the C++17 Codec port: the risk-definition
// document that ships with the sample in the task description.
//
//   {
//     "risks": [
//       { "id": "...", "vid": "...", "risk_type": "...", "severity": "...",
//         "name": {"cn": "...", "en": "..."},
//         "description": {...}, "solution": {...},
//         "condition": {"or": [ {"param": "...", "op": "is_true"},
//                               {"param": "...", "op": "eq", "value": "..."} ]},
//         "evidence": ["...", "..."] }
//     ]
//   }
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "codec.hpp"

namespace risk {

using codec::Codec;
using codec::JsonValue;

struct LocalizedText {
  std::string cn;
  std::string en;

  bool operator==(const LocalizedText& other) const {
    return cn == other.cn && en == other.en;
  }
  bool operator!=(const LocalizedText& other) const { return !(*this == other); }
};

// A recursive boolean condition: either a leaf predicate
// ({"param":..,"op":..,"value":..,"list_match":..}) or an and/or/not group.
struct Condition {
  std::vector<Condition> orClauses;   // JSON "or"
  std::vector<Condition> andClauses;  // JSON "and"
  std::vector<Condition> notClauses;  // JSON "not"
  std::optional<std::string> param;
  std::optional<std::string> op;
  std::optional<JsonValue> value;
  std::optional<std::string> listMatch;

  bool operator==(const Condition& other) const {
    return orClauses == other.orClauses && andClauses == other.andClauses &&
           notClauses == other.notClauses && param == other.param && op == other.op &&
           value == other.value && listMatch == other.listMatch;
  }
  bool operator!=(const Condition& other) const { return !(*this == other); }
};

struct RiskDef {
  std::string id;
  std::string vid;
  std::string riskType;
  std::string severity;
  LocalizedText name;
  LocalizedText description;
  LocalizedText solution;
  std::vector<std::string> evidence;
  std::optional<Condition> condition;

  bool operator==(const RiskDef& other) const {
    return id == other.id && vid == other.vid && riskType == other.riskType &&
           severity == other.severity && name == other.name && description == other.description &&
           solution == other.solution && evidence == other.evidence &&
           condition == other.condition;
  }
  bool operator!=(const RiskDef& other) const { return !(*this == other); }
};

struct RiskDocument {
  std::vector<RiskDef> risks;

  bool operator==(const RiskDocument& other) const { return risks == other.risks; }
  bool operator!=(const RiskDocument& other) const { return !(*this == other); }
};

// ---------------------------------------------------------------------------
// Codecs -- written exactly like the DFU originals, only with C++ syntax.
//
// Each accessor hands out a reference to a function-local singleton codec:
// codecs are value types whose captured field table is deep-copied on copy, so
// returning references keeps their use cheap.
// ---------------------------------------------------------------------------
inline const Codec<LocalizedText>& localizedTextCodec() {
  static const Codec<LocalizedText> codec = codec::record<LocalizedText>(
      codec::fieldOf("cn", &LocalizedText::cn, codec::codecs::String),
      codec::fieldOf("en", &LocalizedText::en, codec::codecs::String));
  return codec;
}

// The recursion is expressed with codec::recursive, which resolves its supplier
// on first use and therefore also breaks the static initialisation cycle.
//
// The condition subtree uses the *strict* optional variants: a malformed rule must
// not be silently dropped -- ignoring a broken condition would turn "this security
// rule is invalid" into "this rule does not apply", which is exactly the failure
// mode a risk validator exists to prevent.  The lenient DFU behaviour is still
// available via codec::optionalFieldOf when loading data you do not control.
inline const Codec<Condition>& conditionCodec() {
  static const Codec<Condition> codec = codec::recursive<Condition>([] {
    return codec::record<Condition>(
        codec::optionalFieldOfStrict("or", &Condition::orClauses, codec::listOf(conditionCodec()),
                                     std::vector<Condition>{}),
        codec::optionalFieldOfStrict("and", &Condition::andClauses, codec::listOf(conditionCodec()),
                                     std::vector<Condition>{}),
        codec::optionalFieldOfStrict("not", &Condition::notClauses, codec::listOf(conditionCodec()),
                                     std::vector<Condition>{}),
        codec::optionalFieldOfStrict("param", &Condition::param, codec::codecs::String),
        codec::optionalFieldOfStrict("op", &Condition::op, codec::codecs::String),
        codec::optionalFieldOfStrict("value", &Condition::value, codec::codecs::Passthrough),
        codec::optionalFieldOfStrict("list_match", &Condition::listMatch, codec::codecs::String));
  });
  return codec;
}

inline const Codec<RiskDef>& riskDefCodec() {
  // Field order matches the sample document so that a decode/encode round trip
  // reproduces it byte for byte.
  static const Codec<RiskDef> codec = codec::record<RiskDef>(
      codec::fieldOf("id", &RiskDef::id, codec::codecs::String),
      codec::fieldOf("vid", &RiskDef::vid, codec::codecs::String),
      codec::fieldOf("risk_type", &RiskDef::riskType, codec::codecs::String),
      codec::fieldOf("severity", &RiskDef::severity, codec::codecs::String),
      codec::fieldOf("name", &RiskDef::name, localizedTextCodec()),
      codec::fieldOf("description", &RiskDef::description, localizedTextCodec()),
      codec::fieldOf("solution", &RiskDef::solution, localizedTextCodec()),
      codec::optionalFieldOfStrict("condition", &RiskDef::condition, conditionCodec()),
      codec::fieldOf("evidence", &RiskDef::evidence, codec::listOf(codec::codecs::String)));
  return codec;
}

inline const Codec<RiskDocument>& riskDocumentCodec() {
  static const Codec<RiskDocument> codec = codec::record<RiskDocument>(
      codec::fieldOf("risks", &RiskDocument::risks, codec::listOf(riskDefCodec())));
  return codec;
}

// The sample document from the task description.
inline const char* kSampleRiskJson() {
  return R"({
  "risks": [
    {
      "id": "CLAUDE-BYPASS-001",
      "vid": "VID0000004",
      "risk_type": "security_bypass",
      "severity": "critical",
      "name": {
        "cn": "Claude Code 权限审批被绕过",
        "en": "Claude Code permission approval bypassed"
      },
      "description": {
        "cn": "使用了 --dangerously-skip-permissions 启动参数，或将权限默认模式设置为 bypassPermissions，工具调用不再经过用户审批，AI 可无限制执行任意操作。",
        "en": "The --dangerously-skip-permissions launch flag is used, or the default permission mode is set to bypassPermissions: tool calls no longer require user approval, allowing the AI to perform arbitrary operations without restriction."
      },
      "solution": {
        "cn": "移除 --dangerously-skip-permissions 参数，将 permissions.defaultMode 恢复为 default。",
        "en": "Remove the --dangerously-skip-permissions flag and restore permissions.defaultMode to default."
      },
      "condition": {
        "or": [
          { "param": "skip_permissions_flag", "op": "is_true" },
          { "param": "default_mode", "op": "eq", "value": "bypassPermissions" }
        ]
      },
      "evidence": ["default_mode", "skip_permissions_flag"]
    },
    {
      "id": "CLAUDE-MCP-001",
      "vid": "VID0000005",
      "risk_type": "other",
      "severity": "medium",
      "name": {
        "cn": "Claude Code MCP server 配置了跳过权限的启动参数",
        "en": "Claude Code MCP server launched with permission-skipping arguments"
      },
      "description": {
        "cn": "任一 MCP server 的启动参数包含 --dangerously-skip-permissions，该 server 执行的操作将绕过权限审批。",
        "en": "One of the MCP servers is launched with --dangerously-skip-permissions in its arguments; operations performed by that server bypass permission approval."
      },
      "solution": {
        "cn": "从对应 MCP server 的 args 中移除 --dangerously-skip-permissions。",
        "en": "Remove --dangerously-skip-permissions from the args of the affected MCP server."
      },
      "condition": {
        "param": "mcp_args",
        "op": "contains",
        "value": "--dangerously-skip-permissions",
        "list_match": "any"
      },
      "evidence": ["mcp_args"]
    }
  ]
})";
}

}  // namespace risk
