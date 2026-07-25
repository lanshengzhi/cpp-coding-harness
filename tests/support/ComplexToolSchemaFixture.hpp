#pragma once

#include <string_view>

namespace cch::tests {

inline constexpr std::string_view kComplexToolArgumentContract = R"json({
  "$schema": "http://json-schema.org/draft-07/schema#",
  "$id": "urn:cch:test:tool-argument-contract",
  "title": "Complete tool argument contract",
  "description": "Exercises lossless executable members and annotations",
  "type": "object",
  "properties": {
    "mode": {
      "enum": ["read", "write"],
      "description": "Requested operation"
    },
    "enabled": {
      "const": true,
      "default": true
    },
    "count": {
      "type": ["integer", "string"],
      "minimum": 1,
      "maximum": 10,
      "exclusiveMinimum": 0,
      "exclusiveMaximum": 11,
      "multipleOf": 1
    },
    "host": {
      "type": "string",
      "minLength": 1,
      "maxLength": 64,
      "pattern": "^[a-z0-9.-]+$",
      "format": "hostname",
      "examples": ["example.test"],
      "deprecated": false,
      "readOnly": false,
      "writeOnly": false
    },
    "tags": {
      "type": "array",
      "items": {"type": "string"},
      "minItems": 1,
      "maxItems": 5,
      "uniqueItems": true
    },
    "tuple": {
      "type": "array",
      "items": [
        {"type": "string"},
        {"type": "integer"}
      ],
      "additionalItems": false
    },
    "metadata": {
      "type": "object",
      "properties": {
        "known": {"type": "number"}
      },
      "required": ["known"],
      "minProperties": 1,
      "maxProperties": 4,
      "additionalProperties": {"type": "string"}
    },
    "choice": {
      "oneOf": [
        {"const": "auto"},
        {"type": "integer"}
      ]
    },
    "alternative": {
      "anyOf": [true, {"type": "null"}]
    },
    "combined": {
      "allOf": [
        {"type": "number"},
        {"minimum": 0}
      ]
    }
  },
  "required": ["mode", "enabled"],
  "additionalProperties": false,
  "definitions": {
    "identifier": {
      "type": "string",
      "minLength": 1
    }
  },
  "x-cch-extension": {
    "annotation": "must survive",
    "nested": [true, false, null, {"answer": 42}]
  }
})json";

} // namespace cch::tests
