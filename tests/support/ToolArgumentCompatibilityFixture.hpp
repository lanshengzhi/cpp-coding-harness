#pragma once

#include <array>
#include <string_view>

namespace cch::tests {

inline constexpr std::string_view kRecursiveCollectionContract = R"json({
  "type": "object",
  "properties": {
    "matrix": {
      "type": "array",
      "items": {
        "type": "array",
        "items": {"type": "integer"}
      }
    },
    "tuple": {
      "type": "array",
      "items": [
        {"type": "string", "minLength": 2},
        {
          "type": "object",
          "properties": {"enabled": {"type": "boolean"}},
          "required": ["enabled"],
          "additionalProperties": {"type": "number"}
        }
      ],
      "additionalItems": false
    }
  },
  "required": ["matrix", "tuple"],
  "additionalProperties": false
})json";

inline constexpr std::string_view kRecursiveCollectionArguments =
    R"json({"matrix":[["1",2],["3"]],"tuple":[42,{"enabled":"false","score":"4"}]})json";

inline constexpr std::string_view kRecursiveCollectionExpected =
    R"json({"matrix":[[1,2],[3]],"tuple":["42",{"enabled":false,"score":4}]})json";

inline constexpr std::string_view kBoundedContract = R"json({
  "type": "object",
  "properties": {
    "quantity": {
      "type": "number",
      "minimum": 1,
      "maximum": 10,
      "exclusiveMinimum": 0,
      "exclusiveMaximum": 11,
      "multipleOf": 0.5
    },
    "label": {"type": "string", "minLength": 2, "maxLength": 4},
    "metadata": {"type": "object", "minProperties": 1, "maxProperties": 2},
    "values": {"type": "array", "minItems": 2, "maxItems": 3, "uniqueItems": true}
  },
  "required": ["quantity", "label", "metadata", "values"],
  "additionalProperties": false
})json";

inline constexpr std::string_view kBoundedValidArguments =
    R"json({"quantity":"2.5","label":"ok","metadata":{"source":true},"values":[1,2]})json";

inline constexpr std::string_view kBoundedInvalidArguments =
    R"json({"quantity":2.25,"label":"x","metadata":{},"values":[1,1,2,3]})json";

inline constexpr std::string_view kBoundedUpperInvalidArguments =
    R"json({"quantity":11,"label":"abcde","metadata":{"a":1,"b":2,"c":3},"values":[]})json";

inline constexpr std::string_view kBoundedLowerInvalidArguments =
    R"json({"quantity":0,"label":"ok","metadata":{"source":true},"values":[1,2]})json";

// TypeBox 1.1.38 counts grapheme clusters for string bounds and uses a
// fixed 1e-10 tolerance for floating-point multipleOf checks.
inline constexpr std::string_view kTypeBoxBoundaryContract = R"json({
  "type": "object",
  "properties": {
    "label": {"type": "string", "maxLength": 1},
    "tiny": {"type": "number", "multipleOf": 1e-12}
  },
  "required": ["label", "tiny"],
  "additionalProperties": false
})json";

inline constexpr std::string_view kTypeBoxBoundaryArguments =
    R"json({"label":"e\u0301","tiny":5e-13})json";

struct StringBoundFixture {
    std::string_view raw_json;
    bool accepted_by_max_length_one;
    bool accepted_by_min_length_two;
};

// TypeBox's fast string-bound checks fall back to grapheme counting only for
// astral code points, basic combining marks, and ZWJ code points. Extended
// combining marks and variation selectors remain separate UTF-16 units.
inline constexpr std::array<StringBoundFixture, 12> kStringBoundFixtures{{
    {R"json("a")json", true, false},
    {R"json("a\u0301")json", true, false},
    {R"json("a\u1AB0")json", false, true},
    {R"json("a\u1DC0")json", false, true},
    {R"json("a\uFE20")json", false, true},
    {R"json("a\uFE0E")json", false, true},
    {R"json("a\uFE0F")json", false, true},
    {R"json("\u1AB0")json", true, false},
    {R"json("\uD83D\uDE00")json", true, false},
    {R"json("\uD83C\uDDFA\uD83C\uDDF8")json", true, false},
    {R"json("\uD83D\uDC69\u200D\uD83D\uDCBB")json", true, false},
    {R"json("ab")json", false, true},
}};

inline constexpr std::string_view kCompositionContract = R"json({
  "type": "object",
  "properties": {
    "combined": {
      "allOf": [
        {"type": "object", "properties": {"count": {"type": "integer"}}},
        {"type": "object", "properties": {"count": {"minimum": 2}}}
      ]
    },
    "alternative": {
      "anyOf": [
        {"type": "integer", "minimum": 2},
        {"type": "string", "const": "fallback"}
      ]
    },
    "exclusive": {
      "oneOf": [
        {"type": "integer", "minimum": 2},
        {"type": "string", "const": "fallback"}
      ]
    }
  },
  "required": ["combined", "alternative", "exclusive"],
  "additionalProperties": false
})json";

inline constexpr std::string_view kCompositionArguments =
    R"json({"combined":{"count":"2"},"alternative":"3","exclusive":"4"})json";

inline constexpr std::string_view kCompositionExpected =
    R"json({"alternative":3,"combined":{"count":2},"exclusive":4})json";

struct FormatFixture {
    std::string_view format;
    std::string_view valid;
    std::string_view invalid;
};

inline constexpr std::array<FormatFixture, 21> kRecognizedFormatFixtures{{
    {"date-time", "2020-12-12T20:20:40+00:00", "2020-13-12T20:20:40+00:00"},
    {"date", "2024-02-29", "2023-02-29"},
    {"duration", "P3Y6M4DT12H30M5S", "three days"},
    {"email", "agent@example.test", "agent..name@example.test"},
    {"hostname", "example.test", "bad_host.test"},
    {"idn-email", "agent@example.test", "agent name@example.test"},
    {"idn-hostname", "example.test", "bad host.test"},
    {"ipv4", "192.0.2.1", "192.0.2.999"},
    {"ipv6", "2001:db8::1", "2001:db8::1::2"},
    {"iri-reference", "../tools/inspect", "bad reference"},
    {"iri", "https://example.test/tools", "not an iri"},
    {"json-pointer-uri-fragment", "#/tools/0", "#tools/0"},
    {"json-pointer", "/tools/0", "tools/0"},
    {"regex", "^[a-z]+$", "[unterminated"},
    {"relative-json-pointer", "1/tools/0", "01/tools/0"},
    {"time", "20:20:40+00:00", "25:20:40+00:00"},
    {"uri-reference", "../tools/inspect", "bad reference"},
    {"uri-template", "/tools/{id}", "/tools/{"},
    {"uri", "https://example.test/tools", "not a uri"},
    {"url", "https://example.test/tools", "not a url"},
    {"uuid", "123e4567-e89b-12d3-a456-426614174000", "123e4567-e89b"},
}};

struct RejectedFormatFixture {
    std::string_view format;
    std::string_view value;
};

// Differential counterexamples captured from TypeBox 1.1.38's default
// format registry. Every value below is rejected by the recorded baseline.
inline constexpr std::array<RejectedFormatFixture, 6> kRejectedFormatRegressionFixtures{{
    {"uri", "https://example.com/{x}"},
    {"uri-reference", "<>"},
    {"url", "http://192.168.0.1"},
    {"iri", "http:"},
    {"uri-template", "{foo!}"},
    {"idn-email", "😀@example.com"},
}};

struct JsonFormatFixture {
    std::string_view format;
    std::string_view raw_json;
    bool accepted;
};

// TypeBox canonicalizes Unicode hostname separators, but its IDN-email regex
// accepts only ASCII dots. Both formats reject every resulting empty label.
inline constexpr std::array<JsonFormatFixture, 18> kIdnSeparatorFixtures{{
    {"idn-hostname", R"json("\u4F8B\u3048.\u30C6\u30B9\u30C8")json", true},
    {"idn-hostname", R"json("\u4F8B\u3048\u3002\u30C6\u30B9\u30C8")json", true},
    {"idn-hostname", R"json("\u4F8B\u3048\uFF0E\u30C6\u30B9\u30C8")json", true},
    {"idn-hostname", R"json("\u4F8B\u3048\uFF61\u30C6\u30B9\u30C8")json", true},
    {"idn-hostname", R"json("\u4F8B\u3048..\u30C6\u30B9\u30C8")json", false},
    {"idn-hostname", R"json("\u4F8B\u3048\u3002\u3002\u30C6\u30B9\u30C8")json", false},
    {"idn-hostname", R"json("\u4F8B\u3048\u3002\uFF0E\u30C6\u30B9\u30C8")json", false},
    {"idn-hostname", R"json("\uFF61\u4F8B\u3048")json", false},
    {"idn-hostname", R"json("\u4F8B\u3048\u3002")json", false},
    {"idn-email", R"json("\u7528\u6237@\u4F8B\u3048.\u30C6\u30B9\u30C8")json", true},
    {"idn-email", R"json("\u7528\u6237@\u4F8B\u3048\u3002\u30C6\u30B9\u30C8")json", false},
    {"idn-email", R"json("\u7528\u6237@\u4F8B\u3048\uFF0E\u30C6\u30B9\u30C8")json", false},
    {"idn-email", R"json("\u7528\u6237@\u4F8B\u3048\uFF61\u30C6\u30B9\u30C8")json", false},
    {"idn-email", R"json("\u7528\u6237@\u4F8B\u3048..\u30C6\u30B9\u30C8")json", false},
    {"idn-email", R"json("\u7528\u6237@\u4F8B\u3048\u3002\u3002\u30C6\u30B9\u30C8")json", false},
    {"idn-email", R"json("\u7528\u6237@\u4F8B\u3048\u3002.\u30C6\u30B9\u30C8")json", false},
    {"idn-email", R"json("\u7528\u6237@\uFF61\u4F8B\u3048")json", false},
    {"idn-email", R"json("\u7528\u6237@\u4F8B\u3048\u3002")json", false},
}};

} // namespace cch::tests
