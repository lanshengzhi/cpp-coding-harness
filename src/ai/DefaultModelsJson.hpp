#pragma once

#include <string_view>

namespace cch::ai {

/// The bundled model catalog (ADR 0058): the single compile-time source for
/// every built-in provider's models, limits, costs, and thinking-level maps.
/// The document uses pi's `models.json` key vocabulary, so the same shapes the
/// user's config carries are parsed here. A `thinkingLevelMap` value of `null`
/// marks a level the model explicitly does not support; a missing key leaves
/// the level unconstrained.
[[nodiscard]] constexpr std::string_view default_models_json() noexcept {
    return R"JSON({
  "providers": {
    "deepseek": {
      "name": "DeepSeek",
      "baseUrl": "https://api.deepseek.com/responses",
      "api": "openai-responses",
      "models": [
        {
          "id": "deepseek-chat",
          "name": "DeepSeek Chat",
          "reasoning": false,
          "contextWindow": 64000,
          "maxTokens": 8192,
          "cost": {
            "input": 0.14,
            "output": 0.28,
            "cacheRead": 0.014,
            "cacheWrite": 0.0
          }
        },
        {
          "id": "deepseek-reasoner",
          "name": "DeepSeek Reasoner (R1)",
          "reasoning": true,
          "contextWindow": 64000,
          "maxTokens": 8192,
          "cost": {
            "input": 0.55,
            "output": 2.19,
            "cacheRead": 0.14,
            "cacheWrite": 0.0
          },
          "thinkingLevelMap": {
            "high": "high",
            "max": "max"
          }
        }
      ]
    },
    "openrouter": {
      "name": "OpenRouter",
      "baseUrl": "https://openrouter.ai/api/v1/responses",
      "api": "openai-responses",
      "models": [
        {
          "id": "anthropic/claude-3.7-sonnet",
          "name": "Claude 3.7 Sonnet (OpenRouter)",
          "reasoning": true,
          "contextWindow": 200000,
          "maxTokens": 64000,
          "cost": {
            "input": 3.0,
            "output": 15.0,
            "cacheRead": 0.3,
            "cacheWrite": 3.75
          }
        },
        {
          "id": "deepseek/deepseek-r1",
          "name": "DeepSeek R1 (OpenRouter)",
          "reasoning": true,
          "contextWindow": 64000,
          "maxTokens": 8192,
          "cost": {
            "input": 0.55,
            "output": 2.19,
            "cacheRead": 0.14,
            "cacheWrite": 0.0
          }
        },
        {
          "id": "openai/gpt-4o",
          "name": "GPT-4o (OpenRouter)",
          "contextWindow": 128000,
          "maxTokens": 16384,
          "cost": {
            "input": 2.5,
            "output": 10.0,
            "cacheRead": 1.25,
            "cacheWrite": 0.0
          }
        }
      ]
    },
    "opencode-go": {
      "name": "OpenCode Go",
      "baseUrl": "https://opencode.ai/zen/go/v1/responses",
      "api": "openai-responses",
      "models": [
        {
          "id": "gpt-5.6-luna",
          "name": "GPT-5.6 Luna (OpenCode)",
          "reasoning": true,
          "contextWindow": 1050000,
          "maxTokens": 128000,
          "cost": {
            "input": 0.1,
            "output": 0.6,
            "cacheRead": 0.01,
            "cacheWrite": 0.125
          },
          "thinkingLevelMap": {
            "low": "low",
            "medium": "medium",
            "high": "high",
            "xhigh": "xhigh",
            "max": "max"
          }
        },
        {
          "id": "grok-4.5",
          "name": "Grok 4.5 (OpenCode)",
          "reasoning": true,
          "contextWindow": 500000,
          "maxTokens": 500000,
          "cost": {
            "input": 2.0,
            "output": 6.0,
            "cacheRead": 0.5,
            "cacheWrite": 0.0
          },
          "thinkingLevelMap": {
            "low": "low",
            "medium": "medium",
            "high": "high"
          }
        }
      ]
    },
    "openai": {
      "name": "OpenAI",
      "baseUrl": "https://api.openai.com/v1/responses",
      "api": "openai-responses",
      "models": [
        {
          "id": "gpt-4o",
          "name": "GPT-4o",
          "contextWindow": 128000,
          "maxTokens": 16384,
          "cost": {
            "input": 2.5,
            "output": 10.0,
            "cacheRead": 1.25,
            "cacheWrite": 0.0
          }
        },
        {
          "id": "gpt-4o-mini",
          "name": "GPT-4o mini",
          "contextWindow": 128000,
          "maxTokens": 16384,
          "cost": {
            "input": 0.15,
            "output": 0.6,
            "cacheRead": 0.075,
            "cacheWrite": 0.0
          }
        },
        {
          "id": "o1",
          "name": "o1",
          "reasoning": true,
          "contextWindow": 200000,
          "maxTokens": 100000,
          "cost": {
            "input": 15.0,
            "output": 60.0,
            "cacheRead": 7.5,
            "cacheWrite": 0.0
          }
        },
        {
          "id": "o3-mini",
          "name": "o3-mini",
          "reasoning": true,
          "contextWindow": 200000,
          "maxTokens": 100000,
          "cost": {
            "input": 1.1,
            "output": 4.4,
            "cacheRead": 0.55,
            "cacheWrite": 0.0
          },
          "thinkingLevelMap": {
            "low": "low",
            "medium": "medium",
            "high": "high"
          }
        }
      ]
    },
    "openai-codex": {
      "name": "OpenAI Codex",
      "baseUrl": "https://chatgpt.com/backend-api",
      "api": "openai-codex-responses",
      "models": [
        {
          "id": "gpt-5.3-codex-spark",
          "name": "GPT-5.3 Codex Spark",
          "reasoning": true,
          "contextWindow": 128000,
          "maxTokens": 128000,
          "cost": {
            "input": 1.75,
            "output": 14.0,
            "cacheRead": 0.175,
            "cacheWrite": 0.0
          },
          "thinkingLevelMap": {
            "minimal": "low",
            "xhigh": "xhigh"
          }
        },
        {
          "id": "gpt-5.4",
          "name": "GPT-5.4",
          "reasoning": true,
          "contextWindow": 272000,
          "maxTokens": 128000,
          "cost": {
            "input": 2.5,
            "output": 15.0,
            "cacheRead": 0.25,
            "cacheWrite": 0.0,
            "tiers": [
              {
                "inputTokensAbove": 272000,
                "input": 5.0,
                "output": 22.5,
                "cacheRead": 0.5,
                "cacheWrite": 0.0
              }
            ]
          },
          "thinkingLevelMap": {
            "minimal": "low",
            "xhigh": "xhigh"
          }
        },
        {
          "id": "gpt-5.4-mini",
          "name": "GPT-5.4 mini",
          "reasoning": true,
          "contextWindow": 272000,
          "maxTokens": 128000,
          "cost": {
            "input": 0.75,
            "output": 4.5,
            "cacheRead": 0.075,
            "cacheWrite": 0.0
          },
          "thinkingLevelMap": {
            "minimal": "low",
            "xhigh": "xhigh"
          }
        },
        {
          "id": "gpt-5.5",
          "name": "GPT-5.5",
          "reasoning": true,
          "contextWindow": 272000,
          "maxTokens": 128000,
          "cost": {
            "input": 5.0,
            "output": 30.0,
            "cacheRead": 0.5,
            "cacheWrite": 0.0,
            "tiers": [
              {
                "inputTokensAbove": 272000,
                "input": 10.0,
                "output": 45.0,
                "cacheRead": 1.0,
                "cacheWrite": 0.0
              }
            ]
          },
          "thinkingLevelMap": {
            "minimal": "low",
            "xhigh": "xhigh"
          }
        },
        {
          "id": "gpt-5.6-luna",
          "name": "GPT-5.6 Luna",
          "reasoning": true,
          "contextWindow": 272000,
          "maxTokens": 128000,
          "cost": {
            "input": 0.2,
            "output": 1.2,
            "cacheRead": 0.02,
            "cacheWrite": 0.25,
            "tiers": [
              {
                "inputTokensAbove": 272000,
                "input": 0.4,
                "output": 1.8,
                "cacheRead": 0.04,
                "cacheWrite": 0.5
              }
            ]
          },
          "thinkingLevelMap": {
            "minimal": "low",
            "xhigh": "xhigh",
            "max": "max"
          }
        },
        {
          "id": "gpt-5.6-sol",
          "name": "GPT-5.6 Sol",
          "reasoning": true,
          "contextWindow": 272000,
          "maxTokens": 128000,
          "cost": {
            "input": 5.0,
            "output": 30.0,
            "cacheRead": 0.5,
            "cacheWrite": 6.25,
            "tiers": [
              {
                "inputTokensAbove": 272000,
                "input": 10.0,
                "output": 45.0,
                "cacheRead": 1.0,
                "cacheWrite": 12.5
              }
            ]
          },
          "thinkingLevelMap": {
            "minimal": "low",
            "xhigh": "xhigh",
            "max": "max"
          }
        },
        {
          "id": "gpt-5.6-terra",
          "name": "GPT-5.6 Terra",
          "reasoning": true,
          "contextWindow": 272000,
          "maxTokens": 128000,
          "cost": {
            "input": 2.0,
            "output": 12.0,
            "cacheRead": 0.2,
            "cacheWrite": 2.5,
            "tiers": [
              {
                "inputTokensAbove": 272000,
                "input": 4.0,
                "output": 18.0,
                "cacheRead": 0.4,
                "cacheWrite": 5.0
              }
            ]
          },
          "thinkingLevelMap": {
            "minimal": "low",
            "xhigh": "xhigh",
            "max": "max"
          }
        }
      ]
    },
    "kimi-coding": {
      "name": "Kimi For Coding",
      "baseUrl": "https://api.kimi.com/coding",
      "api": "anthropic-messages",
      "models": [
        {
          "id": "k3",
          "name": "Kimi K3",
          "reasoning": true,
          "contextWindow": 1048576,
          "maxTokens": 131072,
          "headers": {
            "User-Agent": "KimiCLI/1.5"
          },
          "cost": {
            "input": 3.0,
            "output": 15.0,
            "cacheRead": 0.3,
            "cacheWrite": 0.0
          },
          "thinkingLevelMap": {
            "off": null,
            "minimal": null,
            "low": "low",
            "medium": null,
            "high": "high",
            "xhigh": null,
            "max": "max"
          }
        },
        {
          "id": "k3-256k",
          "name": "Kimi K3-256K",
          "reasoning": true,
          "contextWindow": 262144,
          "maxTokens": 131072,
          "headers": {
            "User-Agent": "KimiCLI/1.5"
          },
          "thinkingLevelMap": {
            "off": null,
            "minimal": null,
            "low": "low",
            "medium": null,
            "high": "high",
            "xhigh": null,
            "max": "max"
          }
        },
        {
          "id": "kimi-for-coding",
          "name": "Kimi K2.7 Code",
          "reasoning": true,
          "contextWindow": 262144,
          "maxTokens": 32768,
          "headers": {
            "User-Agent": "KimiCLI/1.5"
          },
          "cost": {
            "input": 0.95,
            "output": 4.0,
            "cacheRead": 0.19,
            "cacheWrite": 0.0
          }
        },
        {
          "id": "kimi-for-coding-highspeed",
          "name": "Kimi For Coding HighSpeed",
          "reasoning": true,
          "contextWindow": 262144,
          "maxTokens": 32768,
          "headers": {
            "User-Agent": "KimiCLI/1.5"
          },
          "cost": {
            "input": 1.9,
            "output": 8.0,
            "cacheRead": 0.38,
            "cacheWrite": 0.0
          }
        }
      ]
    }
  }
})JSON";
}

} // namespace cch::ai
