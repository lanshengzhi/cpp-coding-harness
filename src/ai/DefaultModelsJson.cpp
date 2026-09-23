#include "DefaultModelsJson.hpp"

#include <array>
#include <cstddef>

namespace cch::ai {

namespace {

constexpr char kCatalogPart0[] = R"cch_catalog(
{
  "providers": {
    "deepseek": {
      "models": [
        {
          "api": "openai-completions",
          "baseUrl": "https://api.deepseek.com",
          "compat": {
            "maxTokensField": "max_tokens",
            "requiresReasoningContentOnAssistantMessages": true,
            "supportsDeveloperRole": false,
            "supportsStore": false,
            "supportsStrictMode": true,
            "thinkingFormat": "deepseek"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.006,
            "cacheWrite": 0,
            "input": 0.3,
            "output": 1.2
          },
          "id": "deepseek-flash",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 384000,
          "name": "DeepSeek V4.1 Flash",
          "provider": "deepseek",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://api.deepseek.com",
          "compat": {
            "maxTokensField": "max_tokens",
            "requiresReasoningContentOnAssistantMessages": true,
            "supportsDeveloperRole": false,
            "supportsMidConvoSystemMessages": true,
            "supportsStore": false,
            "supportsStrictMode": true,
            "thinkingFormat": "deepseek"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.044,
            "cacheWrite": 0,
            "input": 1.32,
            "output": 3.96
          },
          "id": "deepseek-v4-pro",
          "input": [
            "text"
          ],
          "maxTokens": 384000,
          "name": "DeepSeek V4 Pro",
          "provider": "deepseek",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": "max",
            "medium": null,
            "minimal": null
          }
        }
      ]
    },
    "kimi-coding": {
      "models": [
        {
          "api": "openai-completions",
          "baseUrl": "https://api.kimi.com/coding/v1",
          "compat": {
            "supportsDeveloperRole": false
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.3,
            "cacheWrite": 0,
            "input": 3,
            "output": 15
          },
          "id": "k3",
          "input": [
            "text",
            "image"
          ],
          "maxTokens": 131072,
          "name": "Kimi K3",
          "provider": "kimi-coding",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://api.kimi.com/coding/v1",
          "compat": {
            "supportsDeveloperRole": false
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0,
            "output": 0
          },
          "id": "k3-256k",
          "input": [
            "text",
            "image"
          ],
          "maxTokens": 131072,
          "name": "Kimi K3-256K",
          "provider": "kimi-coding",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://api.kimi.com/coding/v1",
          "compat": {
            "supportsDeveloperRole": false
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.19,
            "cacheWrite": 0,
            "input": 0.95,
            "output": 4
          },
          "id": "kimi-for-coding",
          "input": [
            "text",
            "image"
          ],
          "maxTokens": 32768,
          "name": "Kimi K2.7 Code",
          "provider": "kimi-coding",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://api.kimi.com/coding/v1",
          "compat": {
            "supportsDeveloperRole": false
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.38,
            "cacheWrite": 0,
            "input": 1.9,
            "output": 8
          },
          "id": "kimi-for-coding-highspeed",
          "input": [
            "text",
            "image"
          ],
          "maxTokens": 32768,
          "name": "Kimi For Coding HighSpeed",
          "provider": "kimi-coding",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": null,
            "low": null,
            "max": null,
            "medium": null,
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        }
      ]
    },
    "openai": {
      "models": [
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsStrictMode": true
          },
          "contextWindow": 8192,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 30,
            "output": 60
          },
          "id": "gpt-4",
          "input": [
            "text"
          ],
          "maxTokens": 8192,
          "name": "GPT-4",
          "provider": "openai",
          "reasoning": false
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsStrictMode": true
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 10,
            "output": 30
          },
          "id": "gpt-4-turbo",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 4096,
          "name": "GPT-4 Turbo",
          "provider": "openai",
          "reasoning": false
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsStrictMode": true
          },
          "contextWindow": 1047576,
          "cost": {
            "cacheRead": 0.5,
            "cacheWrite": 0,
            "input": 2,
            "output": 8
          },
          "id": "gpt-4.1",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 32768,
          "name": "GPT-4.1",
          "provider": "openai",
          "reasoning": false
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsStrictMode": true
          },
          "contextWindow": 1047576,
          "cost": {
            "cacheRead": 0.1,
            "cacheWrite": 0,
            "input": 0.4,
            "output": 1.6
          },
          "id": "gpt-4.1-mini",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 32768,
          "name": "GPT-4.1 mini",
          "provider": "openai",
          "reasoning": false
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsStrictMode": true
          },
          "contextWindow": 1047576,
          "cost": {
            "cacheRead": 0.025,
            "cacheWrite": 0,
            "input": 0.1,
            "output": 0.4
          },
          "id": "gpt-4.1-nano",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 32768,
          "name": "GPT-4.1 nano",
          "provider": "openai",
          "reasoning": false
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsStrictMode": true
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 1.25,
            "cacheWrite": 0,
            "input": 2.5,
            "output": 10
          },
          "id": "gpt-4o",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 16384,
          "name": "GPT-4o",
          "provider": "openai",
          "reasoning": false
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsStrictMode": true
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 5,
            "output": 15
          },
          "id": "gpt-4o-2024-05-13",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 4096,
          "name": "GPT-4o (2024-05-13)",
          "provider": "openai",
          "reasoning": false
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsStrictMode": true
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 1.25,
            "cacheWrite": 0,
            "input": 2.5,
            "output": 10
          },
          "id": "gpt-4o-2024-08-06",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 16384,
          "name": "GPT-4o (2024-08-06)",
          "provider": "openai",
          "reasoning": false
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsStrictMode": true
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 1.25,
            "cacheWrite": 0,
            "input": 2.5,
            "output": 10
          },
          "id": "gpt-4o-2024-11-20",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 16384,
          "name": "GPT-4o (2024-11-20)",
          "provider": "openai",
          "reasoning": false
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsStrictMode": true
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0.075,
            "cacheWrite": 0,
            "input": 0.15,
            "output": 0.6
          },
          "id": "gpt-4o-mini",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 16384,
          "name": "GPT-4o mini",
          "provider": "openai",
          "reasoning": false
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsOpenAIGrammarTools": true,
            "supportsStrictMode": true
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.125,
            "cacheWrite": 0,
            "input": 1.25,
            "output": 10
          },
          "id": "gpt-5",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 128000,
          "name": "GPT-5",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsOpenAIGrammarTools": true,
            "supportsStrictMode": true
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0.125,
            "cacheWrite": 0,
            "input": 1.25,
            "output": 10
          },
          "id": "gpt-5-chat-latest",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 16384,
          "name": "GPT-5 Chat Latest",
          "provider": "openai",
          "reasoning": false,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsOpenAIGrammarTools": true,
            "supportsStrictMode": true
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.025,
            "cacheWrite": 0,
            "input": 0.25,
            "output": 2
          },
          "id": "gpt-5-mini",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 128000,
          "name": "GPT-5 Mini",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsOpenAIGrammarTools": true,
            "supportsStrictMode": true
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.005,
            "cacheWrite": 0,
            "input": 0.05,
            "output": 0.4
          },
          "id": "gpt-5-nano",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 128000,
          "name": "GPT-5 Nano",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsOpenAIGrammarTools": true,
            "supportsStrictMode": true
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 15,
            "output": 120
          },
          "id": "gpt-5-pro",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 128000,
          "name": "GPT-5 Pro",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": null,
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsOpenAIGrammarTools": true,
            "supportsStrictMode": true
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.125,
            "cacheWrite": 0,
            "input": 1.25,
            "output": 10
          },
          "id": "gpt-5.1",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 128000,
          "name": "GPT-5.1",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsOpenAIGrammarTools": true,
            "supportsStrictMode": true
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.175,
            "cacheWrite": 0,
            "input": 1.75,
            "output": 14
          },
          "id": "gpt-5.2",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 128000,
          "name": "GPT-5.2",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsOpenAIGrammarTools": true,
            "supportsStrictMode": true
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0.175,
            "cacheWrite": 0,
            "input": 1.75,
            "output": 14
          },
          "id": "gpt-5.2-chat-latest",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 16384,
          "name": "GPT-5.2 Chat",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": null,
            "low": null,
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsOpenAIGrammarTools": true,
            "supportsStrictMode": true
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 21,
            "output": 168
          },
          "id": "gpt-5.2-pro",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 128000,
          "name": "GPT-5.2 Pro",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsOpenAIGrammarTools": true,
            "supportsStrictMode": true
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0.175,
            "cacheWrite": 0,
            "input": 1.75,
            "output": 14
          },
          "id": "gpt-5.3-chat-latest",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 16384,
          "name": "GPT-5.3 Chat (latest)",
          "provider": "openai",
          "reasoning": false,
          "thinkingLevelMap": {
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsOpenAIGrammarTools": true,
            "supportsStrictMode": true
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.175,
            "cacheWrite": 0,
            "input": 1.75,
            "output": 14
          },
          "id": "gpt-5.3-codex",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 128000,
          "name": "GPT-5.3 Codex",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsOpenAIGrammarTools": true,
            "supportsStrictMode": true
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0.175,
            "cacheWrite": 0,
            "input": 1.75,
            "output": 14
          },
          "id": "gpt-5.3-codex-spark",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 32000,
          "name": "GPT-5.3 Codex Spark",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsAdditionalTools": true,
            "supportsMidConvoSystemMessages": true,
            "supportsOpenAIGrammarTools": true,
            "supportsStrictMode": true,
            "supportsToolSearch": true
          },
          "contextWindow": 272000,
          "cost": {
            "cacheRead": 0.25,
            "cacheWrite": 0,
            "input": 2.5,
            "output": 15,
            "tiers": [
              {
                "cacheRead": 0.5,
                "cacheWrite": 0,
                "input": 5,
                "inputTokensAbove": 272000,
                "output": 22.5
              }
            ]
          },
          "id": "gpt-5.4",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 128000,
          "name": "GPT-5.4",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsAdditionalTools": true,
            "supportsMidConvoSystemMessages": true,
            "supportsOpenAIGrammarTools": true,
            "supportsStrictMode": true,
            "supportsToolSearch": true
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.075,
            "cacheWrite": 0,
            "input": 0.75,
            "output": 4.5
          },
          "id": "gpt-5.4-mini",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 128000,
          "name": "GPT-5.4 mini",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsOpenAIGrammarTools": true,
            "supportsStrictMode": true
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.02,
            "cacheWrite": 0,
            "input": 0.2,
            "output": 1.25
          },
          "id": "gpt-5.4-nano",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 128000,
          "name": "GPT-5.4 nano",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsAdditionalTools": true,
            "supportsMidConvoSystemMessages": true,
            "supportsOpenAIGrammarTools": true,
            "supportsStrictMode": true,
            "supportsToolSearch": true
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 30,
            "output": 180,
            "tiers": [
              {
                "cacheRead": 0,
                "cacheWrite": 0,
                "input": 60,
                "inputTokensAbove": 272000,
                "output": 270
              }
            ]
          },
          "id": "gpt-5.4-pro",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 128000,
          "name": "GPT-5.4 Pro",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsAdditionalTools": true,
            "supportsMidConvoSystemMessages": true,
            "supportsOpenAIGrammarTools": true,
            "supportsStrictMode": true,
            "supportsToolSearch": true
          },
          "contextWindow": 272000,
          "cost": {
            "cacheRead": 0.5,
            "cacheWrite": 0,
            "input": 5,
            "output": 30,
            "tiers": [
              {
                "cacheRead": 1,
                "cacheWrite": 0,
                "input": 10,
                "inputTokensAbove": 272000,
                "output": 45
              }
            ]
          },
          "id": "gpt-5.5",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 128000,
          "name": "GPT-5.5",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsOpenAIGrammarTools": true,
            "supportsStrictMode": true
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 30,
            "output": 180,
            "tiers": [
              {
                "cacheRead": 0,
                "cacheWrite": 0,
                "input": 60,
                "inputTokensAbove": 272000,
                "output": 270
              }
            ]
          },
          "id": "gpt-5.5-pro",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 128000,
          "name": "GPT-5.5 Pro",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsAdditionalTools": true,
            "supportsExplicitPromptCacheMode": true,
            "supportsMidConvoSystemMessages": true,
            "supportsOpenAIGrammarTools": true,
            "supportsStrictMode": true,
            "supportsToolSearch": true
          },
          "contextWindow": 272000,
          "cost": {
            "cacheRead": 0.02,
            "cacheWrite": 0.25,
            "input": 0.2,
            "output": 1.2,
            "tiers": [
              {
                "cacheRead": 0.04,
                "cacheWrite": 0.5,
                "input": 0.4,
                "inputTokensAbove": 272000,
                "output": 1.8
              }
            ]
          },
          "id": "gpt-5.6-luna",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 128000,
          "name": "GPT-5.6 Luna",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsAdditionalTools": true,
            "supportsExplicitPromptCacheMode": true,
            "supportsMidConvoSystemMessages": true,
            "supportsOpenAIGrammarTools": true,
            "supportsStrictMode": true,
            "supportsToolSearch": true
          },
          "contextWindow": 272000,
          "cost": {
            "cacheRead": 0.4,
            "cacheWrite": 5,
            "input": 4,
            "output": 20,
            "tiers": [
              {
                "cacheRead": 0.8,
                "cacheWrite": 10,
                "input": 8,
                "inputTokensAbove": 272000,
                "output": 30
              }
            ]
          },
          "id": "gpt-5.6-sol",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 128000,
          "name": "GPT-5.6 Sol",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsAdditionalTools": true,
            "supportsExplicitPromptCacheMode": true,
            "supportsMidConvoSystemMessages": true,
            "supportsOpenAIGrammarTools": true,
            "supportsStrictMode": true,
            "supportsToolSearch": true
          },
          "contextWindow": 272000,
          "cost": {
            "cacheRead": 0.2,
            "cacheWrite": 2.5,
            "input": 2,
            "output": 12,
            "tiers": [
              {
                "cacheRead": 0.4,
                "cacheWrite": 5,
                "input": 4,
                "inputTokensAbove": 272000,
                "output": 18
              }
            ]
          },
          "id": "gpt-5.6-terra",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 128000,
          "name": "GPT-5.6 Terra",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsAdditionalTools": true,
            "supportsExplicitPromptCacheMode": true,
            "supportsMidConvoSystemMessages": true,
            "supportsOpenAIGrammarTools": true,
            "supportsStrictMode": true,
            "supportsToolSearch": true
          },
          "contextWindow": 272000,
          "cost": {
            "cacheRead": 1,
            "cacheWrite": 12.5,
            "input": 10,
            "output": 50,
            "tiers": [
              {
                "cacheRead": 2,
                "cacheWrite": 25,
                "input": 20,
                "inputTokensAbove": 272000,
                "output": 75
              }
            ]
          },
          "id": "gpt-6-astra",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 128000,
          "name": "GPT-6 Astra",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsStrictMode": true
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0.4,
            "cacheWrite": 0,
            "input": 4,
            "output": 24
          },
          "id": "gpt-realtime-2.1",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 32000,
          "name": "GPT-Realtime-2.1",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsStrictMode": true
          },
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 7.5,
            "cacheWrite": 0,
            "input": 15,
            "output": 60
          },
          "id": "o1",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 100000,
          "name": "o1",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsStrictMode": true
          },
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 150,
            "output": 600
          },
          "id": "o1-pro",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 100000,
          "name": "o1-pro",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsStrictMode": true
          },
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 0.5,
            "cacheWrite": 0,
            "input": 2,
            "output": 8
          },
          "id": "o3",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 100000,
          "name": "o3",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsStrictMode": true
          },
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 0.55,
            "cacheWrite": 0,
            "input": 1.1,
            "output": 4.4
          },
          "id": "o3-mini",
          "input": [
            "text"
          ],
          "maxTokens": 100000,
          "name": "o3-mini",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsStrictMode": true
          },
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 20,
            "output": 80
          },
          "id": "o3-pro",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 100000,
          "name": "o3-pro",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://api.openai.com/v1",
          "compat": {
            "supportsStrictMode": true
          },
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 0.275,
            "cacheWrite": 0,
            "input": 1.1,
            "output": 4.4
          },
          "id": "o4-mini",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "maxPerRequest": 1500,
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            },
            "maxRequestBytes": 536870912
          },
          "maxTokens": 100000,
          "name": "o4-mini",
          "provider": "openai",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        }
      ]
    },
    "openai-codex": {
      "models": [
        {
          "api": "openai-codex-responses",
          "baseUrl": "https://chatgpt.com/backend-api",
          "compat": {
            "supportsOpenAIGrammarTools": true
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0.175,
            "cacheWrite": 0,
            "input": 1.75,
            "output": 14
          },
          "id": "gpt-5.3-codex-spark",
          "input": [
            "text"
          ],
          "maxTokens": 128000,
          "name": "GPT-5.3 Codex Spark",
          "provider": "openai-codex",
          "reasoning": true,
          "thinkingLevelMap": {
            "minimal": "low",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-codex-responses",
          "baseUrl": "https://chatgpt.com/backend-api",
          "compat": {
            "supportsMidConvoSystemMessages": true,
            "supportsOpenAIGrammarTools": true,
            "supportsToolSearch": true
          },
          "contextWindow": 272000,
          "cost": {
            "cacheRead": 0.5,
            "cacheWrite": 0,
            "input": 5,
            "output": 30,
            "tiers": [
              {
                "cacheRead": 1,
                "cacheWrite": 0,
                "input": 10,
                "inputTokensAbove": 272000,
                "output": 45
              }
            ]
          },
          "id": "gpt-5.5",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "GPT-5.5",
          "provider": "openai-codex",
          "reasoning": true,
          "thinkingLevelMap": {
            "minimal": "low",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-codex-responses",
          "baseUrl": "https://chatgpt.com/backend-api",
          "compat": {
            "supportsAdditionalTools": true,
            "supportsMidConvoSystemMessages": true,
            "supportsOpenAIGrammarTools": true,
            "supportsToolSearch": true
          },
          "contextWindow": 272000,
          "cost": {
            "cacheRead": 0.02,
            "cacheWrite": 0.25,
            "input": 0.2,
            "output": 1.2,
            "tiers": [
              {
                "cacheRead": 0.04,
                "cacheWrite": 0.5,
                "input": 0.4,
                "inputTokensAbove": 272000,
                "output": 1.8
              }
            ]
          },
          "id": "gpt-5.6-luna",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "GPT-5.6 Luna",
          "provider": "openai-codex",
          "reasoning": true,
          "thinkingLevelMap": {
            "max": "max",
            "minimal": "low",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-codex-responses",
          "baseUrl": "https://chatgpt.com/backend-api",
          "compat": {
            "supportsAdditionalTools": true,
            "supportsMidConvoSystemMessages": true,
            "supportsOpenAIGrammarTools": true,
            "supportsToolSearch": true
          },
          "contextWindow": 272000,
          "cost": {
            "cacheRead": 0.5,
            "cacheWrite": 6.25,
            "input": 5,
            "output": 30,
            "tiers": [
              {
                "cacheRead": 1,
                "cacheWrite": 12.5,
                "input": 10,
                "inputTokensAbove": 272000,
                "output": 45
              }
            ]
          },
          "id": "gpt-5.6-sol",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "GPT-5.6 Sol",
          "provider": "openai-codex",
          "reasoning": true,
          "thinkingLevelMap": {
            "max": "max",
            "minimal": "low",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-codex-responses",
          "baseUrl": "https://chatgpt.com/backend-api",
          "compat": {
            "supportsAdditionalTools": true,
            "supportsMidConvoSystemMessages": true,
            "supportsOpenAIGrammarTools": true,
            "supportsToolSearch": true
          },
          "contextWindow": 272000,
          "cost": {
            "cacheRead": 0.2,
            "cacheWrite": 2.5,
            "input": 2,
            "output": 12,
            "tiers": [
              {
                "cacheRead": 0.4,
                "cacheWrite": 5,
                "input": 4,
                "inputTokensAbove": 272000,
                "output": 18
              }
            ]
          },
          "id": "gpt-5.6-terra",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "GPT-5.6 Terra",
          "provider": "openai-codex",
          "reasoning": true,
          "thinkingLevelMap": {
            "max": "max",
            "minimal": "low",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-codex-responses",
          "baseUrl": "https://chatgpt.com/backend-api",
          "compat": {
            "supportsAdditionalTools": true,
            "supportsMidConvoSystemMessages": true,
            "supportsOpenAIGrammarTools": true,
            "supportsToolSearch": true
          },
          "contextWindow": 272000,
          "cost": {
            "cacheRead": 1,
            "cacheWrite": 12.5,
            "input": 10,
            "output": 50,
            "tiers": [
              {
                "cacheRead": 2,
                "cacheWrite": 25,
                "input": 20,
                "inputTokensAbove": 272000,
                "output": 75
              }
            ]
          },
          "id": "gpt-6-astra",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "GPT-6 Astra",
          "provider": "openai-codex",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": "low",
            "off": null,
            "xhigh": "xhigh"
          }
        }
      ]
    },
    "opencode-go": {
      "models": [
        {
          "api": "openai-completions",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "maxTokensField": "max_tokens",
            "requiresReasoningContentOnAssistantMessages": true,
            "supportsDeveloperRole": false,
            "supportsStore": false,
            "supportsStrictMode": true,
            "thinkingFormat": "deepseek"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.003,
            "cacheWrite": 0,
            "input": 0.15,
            "output": 0.6
          },
          "id": "deepseek-v4-flash",
          "input": [
            "text"
          ],
          "maxTokens": 384000,
          "name": "DeepSeek V4 Flash",
          "provider": "opencode-go",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "maxTokensField": "max_tokens",
            "requiresReasoningContentOnAssistantMessages": true,
            "supportsDeveloperRole": false,
            "supportsStore": false,
            "supportsStrictMode": true,
            "thinkingFormat": "deepseek"
          },
          "contextWindow": 1000000,
)cch_catalog";
constexpr char kCatalogPart1[] = R"cch_catalog(          "cost": {
            "cacheRead": 0.003,
            "cacheWrite": 0,
            "input": 0.15,
            "output": 0.6
          },
          "id": "deepseek-v4-flash-vision-exp",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 384000,
          "name": "DeepSeek V4 Flash Vision Exp",
          "provider": "opencode-go",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "maxTokensField": "max_tokens",
            "requiresReasoningContentOnAssistantMessages": true,
            "supportsDeveloperRole": false,
            "supportsStore": false,
            "supportsStrictMode": true,
            "thinkingFormat": "deepseek"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.022,
            "cacheWrite": 0,
            "input": 0.66,
            "output": 1.98
          },
          "id": "deepseek-v4-pro",
          "input": [
            "text"
          ],
          "maxTokens": 384000,
          "name": "DeepSeek V4 Pro (New)",
          "provider": "opencode-go",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": "max",
            "medium": null,
            "minimal": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "maxTokensField": "max_tokens",
            "requiresReasoningContentOnAssistantMessages": true,
            "supportsDeveloperRole": false,
            "supportsStore": false,
            "supportsStrictMode": true,
            "thinkingFormat": "deepseek"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.003,
            "cacheWrite": 0,
            "input": 0.15,
            "output": 0.6
          },
          "id": "deepseek-v4.1-flash",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 384000,
          "name": "DeepSeek V4.1 Flash",
          "provider": "opencode-go",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "maxTokensField": "max_tokens",
            "supportsDeveloperRole": false,
            "supportsStore": false,
            "supportsStrictMode": true
          },
          "contextWindow": 202752,
          "cost": {
            "cacheRead": 0.26,
            "cacheWrite": 0,
            "input": 1.4,
            "output": 4.4
          },
          "id": "glm-5.1",
          "input": [
            "text"
          ],
          "maxTokens": 32768,
          "name": "GLM-5.1",
          "provider": "opencode-go",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "maxTokensField": "max_tokens",
            "supportsDeveloperRole": false,
            "supportsStore": false,
            "supportsStrictMode": true
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.26,
            "cacheWrite": 0,
            "input": 1.4,
            "output": 4.4
          },
          "id": "glm-5.2",
          "input": [
            "text"
          ],
          "maxTokens": 131072,
          "name": "GLM-5.2",
          "provider": "opencode-go",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "maxTokensField": "max_tokens",
            "supportsDeveloperRole": false,
            "supportsStore": false,
            "supportsStrictMode": true
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.26,
            "cacheWrite": 0,
            "input": 1.4,
            "output": 4.4
          },
          "id": "glm-5.3",
          "input": [
            "text"
          ],
          "maxTokens": 131072,
          "name": "GLM-5.3",
          "provider": "opencode-go",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "maxTokensField": "max_tokens",
            "supportsDeveloperRole": false,
            "supportsStore": false,
            "supportsStrictMode": true
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.03,
            "cacheWrite": 0,
            "input": 0.15,
            "output": 0.5
          },
          "id": "glm-5.3-flash",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 131072,
          "name": "GLM-5.3-Flash",
          "provider": "opencode-go",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "sessionAffinityFormat": "openai-nosession",
            "supportsAdditionalTools": true,
            "supportsMidConvoSystemMessages": true
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0.02,
            "cacheWrite": 0.25,
            "input": 0.2,
            "output": 1.2
          },
          "id": "gpt-5.6-luna",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "GPT-5.6 Luna",
          "provider": "opencode-go",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "sessionAffinityFormat": "openai-nosession"
          },
          "contextWindow": 500000,
          "cost": {
            "cacheRead": 0.5,
            "cacheWrite": 0,
            "input": 2,
            "output": 6
          },
          "id": "grok-4.6",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 500000,
          "name": "Grok 4.6",
          "provider": "opencode-go",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "sessionAffinityFormat": "openai-nosession"
          },
          "contextWindow": 500000,
          "cost": {
            "cacheRead": 0.5,
            "cacheWrite": 0,
            "input": 2,
            "output": 6
          },
          "id": "grok-4.7",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 500000,
          "name": "Grok 4.7",
          "provider": "opencode-go",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "maxTokensField": "max_tokens",
            "supportsDeveloperRole": false,
            "supportsStore": false,
            "supportsStrictMode": true
          },
          "contextWindow": 256000,
          "cost": {
            "cacheRead": 0.035,
            "cacheWrite": 0,
            "input": 0.14,
            "output": 0.58
          },
          "id": "hy3",
          "input": [
            "text"
          ],
          "maxTokens": 128000,
          "name": "Hy3",
          "provider": "opencode-go",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "maxTokensField": "max_tokens",
            "supportsDeveloperRole": false,
            "supportsStore": false,
            "supportsStrictMode": true
          },
          "contextWindow": 1024000,
          "cost": {
            "cacheRead": 0.042,
            "cacheWrite": 0,
            "input": 0.834,
            "output": 2.501
          },
          "id": "hy4-preview",
          "input": [
            "text"
          ],
          "maxTokens": 64000,
          "name": "Hy4 preview",
          "provider": "opencode-go",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "maxTokensField": "max_tokens",
            "supportsDeveloperRole": false,
            "supportsLongCacheRetention": false,
            "supportsReasoningEffort": false,
            "supportsStore": false,
            "supportsStrictMode": true,
            "thinkingFormat": "deepseek"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.16,
            "cacheWrite": 0,
            "input": 0.95,
            "output": 4
          },
          "id": "kimi-k2.6",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Kimi K2.6",
          "provider": "opencode-go",
          "reasoning": true,
          "thinkingLevelMap": {
            "low": null,
            "medium": null,
            "minimal": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "maxTokensField": "max_tokens",
            "supportsDeveloperRole": false,
            "supportsStore": false,
            "supportsStrictMode": true
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.19,
            "cacheWrite": 0,
            "input": 0.95,
            "output": 4
          },
          "id": "kimi-k2.7-code",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 262144,
          "name": "Kimi K2.7 Code",
          "provider": "opencode-go",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "maxTokensField": "max_tokens",
            "supportsDeveloperRole": false,
            "supportsMidConvoSystemMessages": true,
            "supportsMidConvoToolAdditions": true,
            "supportsStore": false,
            "supportsStrictMode": true
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.3,
            "cacheWrite": 0,
            "input": 3,
            "output": 15
          },
          "id": "kimi-k3",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 131072,
          "name": "Kimi K3",
          "provider": "opencode-go",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": null,
            "low": null,
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "maxTokensField": "max_tokens",
            "supportsDeveloperRole": false,
            "supportsStore": false,
            "supportsStrictMode": true
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.006,
            "cacheWrite": 0,
            "input": 0.3,
            "output": 1.2
          },
          "id": "longcat-2.0",
          "input": [
            "text"
          ],
          "maxTokens": 131072,
          "name": "LongCat-2.0",
          "provider": "opencode-go",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "maxTokensField": "max_tokens",
            "supportsDeveloperRole": false,
            "supportsStore": false,
            "supportsStrictMode": true
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.0028,
            "cacheWrite": 0,
            "input": 0.14,
            "output": 0.28
          },
          "id": "mimo-v2.5",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "MiMo V2.5",
          "provider": "opencode-go",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "maxTokensField": "max_tokens",
            "supportsDeveloperRole": false,
            "supportsStore": false,
            "supportsStrictMode": true
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.003625,
            "cacheWrite": 0,
            "input": 0.435,
            "output": 0.87
          },
          "id": "mimo-v2.5-pro",
          "input": [
            "text"
          ],
          "maxTokens": 128000,
          "name": "MiMo V2.5 Pro",
          "provider": "opencode-go",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "maxTokensField": "max_tokens",
            "supportsDeveloperRole": false,
            "supportsStore": false,
            "supportsStrictMode": true
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.0028,
            "cacheWrite": 0,
            "input": 0.14,
            "output": 0.28
          },
          "id": "mimo-v2.6-flash",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 131072,
          "name": "MiMo-V2.6-Flash",
          "provider": "opencode-go",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "maxTokensField": "max_tokens",
            "supportsDeveloperRole": false,
            "supportsStore": false,
            "supportsStrictMode": true
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.003625,
            "cacheWrite": 0,
            "input": 0.435,
            "output": 0.87
          },
          "id": "mimo-v2.6-pro",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 131072,
          "name": "MiMo-V2.6-Pro",
          "provider": "opencode-go",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "maxTokensField": "max_tokens",
            "supportsDeveloperRole": false,
            "supportsStore": false,
            "supportsStrictMode": true
          },
          "contextWindow": 204800,
          "cost": {
            "cacheRead": 0.06,
            "cacheWrite": 0.375,
            "input": 0.3,
            "output": 1.2
          },
          "id": "minimax-m2.7",
          "input": [
            "text"
          ],
          "maxTokens": 131072,
          "name": "MiniMax-M2.7",
          "provider": "opencode-go",
          "reasoning": true
        },
        {
          "api": "anthropic-messages",
          "baseUrl": "https://opencode.ai/zen/go",
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.06,
            "cacheWrite": 0,
            "input": 0.3,
            "output": 1.2
          },
          "id": "minimax-m3",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 131072,
          "name": "MiniMax-M3",
          "provider": "opencode-go",
          "reasoning": true
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "sessionAffinityFormat": "openai-nosession"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.002,
            "cacheWrite": 0,
            "input": 0.1,
            "output": 0.2
          },
          "id": "muse-spark-1.2-contributor",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 131072,
          "name": "Muse Spark 1.2 Contributor",
          "provider": "opencode-go",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-responses",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "sessionAffinityFormat": "openai-nosession"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.002,
            "cacheWrite": 0,
            "input": 0.1,
            "output": 0.2
          },
          "id": "muse-spark-1.3-contributor",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 131072,
          "name": "Muse Spark 1.3 Contributor",
          "provider": "opencode-go",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "maxTokensField": "max_tokens",
            "supportsDeveloperRole": false,
            "supportsStore": false,
            "supportsStrictMode": true,
            "thinkingFormat": "qwen"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.05,
            "cacheWrite": 0.625,
            "input": 0.5,
            "output": 3
          },
          "id": "qwen3.6-plus",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Qwen3.6 Plus",
          "provider": "opencode-go",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "maxTokensField": "max_tokens",
            "supportsDeveloperRole": false,
            "supportsStore": false,
            "supportsStrictMode": true
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.5,
            "cacheWrite": 3.125,
            "input": 2.5,
            "output": 7.5
          },
          "id": "qwen3.7-max",
          "input": [
            "text"
          ],
          "maxTokens": 65536,
          "name": "Qwen3.7 Max",
          "provider": "opencode-go",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "maxTokensField": "max_tokens",
            "supportsDeveloperRole": false,
            "supportsStore": false,
            "supportsStrictMode": true
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.04,
            "cacheWrite": 0.5,
            "input": 0.4,
            "output": 1.6
          },
          "id": "qwen3.7-plus",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Qwen3.7 Plus",
          "provider": "opencode-go",
          "reasoning": true
        },
        {
          "api": "anthropic-messages",
          "baseUrl": "https://opencode.ai/zen/go",
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.016,
            "cacheWrite": 0.2,
            "input": 0.15,
            "output": 0.47
          },
          "id": "qwen3.8-flash",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 131072,
          "name": "Qwen3.8 Flash",
          "provider": "opencode-go",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://opencode.ai/zen/go/v1",
          "compat": {
            "maxTokensField": "max_tokens",
            "supportsDeveloperRole": false,
            "supportsStore": false,
            "supportsStrictMode": true
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.25,
            "cacheWrite": 2.5,
            "input": 2,
            "output": 6
          },
          "id": "qwen3.8-max",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 131072,
          "name": "Qwen3.8 Max",
          "provider": "opencode-go",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": null,
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        }
      ]
    },
    "openrouter": {
      "models": [
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0.2,
            "cacheWrite": 0,
            "input": 0.8,
            "output": 1.6
          },
          "id": "aion-labs/aion-2.0",
          "input": [
            "text"
          ],
          "maxTokens": 32768,
          "name": "AionLabs: Aion-2.0",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0.75,
            "cacheWrite": 0,
            "input": 3,
            "output": 6
          },
          "id": "aion-labs/aion-3.0",
          "input": [
            "text"
          ],
          "maxTokens": 32768,
          "name": "AionLabs: Aion-3.0",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0.18,
            "cacheWrite": 0,
            "input": 0.7,
            "output": 1.4
          },
          "id": "aion-labs/aion-3.0-mini",
          "input": [
            "text"
          ],
          "maxTokens": 32768,
          "name": "AionLabs: Aion-3.0-Mini",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.3,
            "output": 2.5
          },
          "id": "amazon/nova-2-lite-v1",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65535,
          "name": "Amazon: Nova 2 Lite",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 300000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.06,
            "output": 0.24
          },
          "id": "amazon/nova-lite-v1",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 5120,
          "name": "Amazon: Nova Lite 1.0",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.035,
            "output": 0.14
          },
          "id": "amazon/nova-micro-v1",
          "input": [
            "text"
          ],
          "maxTokens": 5120,
          "name": "Amazon: Nova Micro 1.0",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.625,
            "cacheWrite": 0,
            "input": 2.5,
            "output": 12.5
          },
          "id": "amazon/nova-premier-v1",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32000,
          "name": "Amazon: Nova Premier 1.0",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 300000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.8,
            "output": 3.2
          },
          "id": "amazon/nova-pro-v1",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 5120,
          "name": "Amazon: Nova Pro 1.0",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "anthropic-messages",
          "baseUrl": "https://openrouter.ai/api",
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 0.03,
            "cacheWrite": 0.3,
            "input": 0.25,
            "output": 1.25
          },
          "id": "anthropic/claude-3-haiku",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 4096,
          "name": "Anthropic: Claude 3 Haiku",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "anthropic-messages",
          "baseUrl": "https://openrouter.ai/api",
          "compat": {
            "forceAdaptiveThinking": true
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 1,
            "cacheWrite": 12.5,
            "input": 10,
            "output": 50
          },
          "id": "anthropic/claude-fable-5",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "Anthropic: Claude Fable 5",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "anthropic-messages",
          "baseUrl": "https://openrouter.ai/api",
          "compat": {
            "forceAdaptiveThinking": true,
            "supportsMidConvoEffort": true
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.25,
            "cacheWrite": 12.5,
            "input": 10,
            "output": 50
          },
          "id": "anthropic/claude-fable-5.1",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "Anthropic: Claude Fable 5.1",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "cacheControlFormat": "anthropic",
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.125,
            "cacheWrite": 6.25,
            "input": 5,
            "output": 25
          },
          "id": "anthropic/claude-fable-5.1:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "Anthropic: Claude Fable 5.1 (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "cacheControlFormat": "anthropic",
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.5,
            "cacheWrite": 6.25,
            "input": 5,
            "output": 25
          },
          "id": "anthropic/claude-fable-5:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "Anthropic: Claude Fable 5 (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "anthropic-messages",
          "baseUrl": "https://openrouter.ai/api",
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 0.1,
            "cacheWrite": 1.25,
            "input": 1,
            "output": 5
          },
          "id": "anthropic/claude-haiku-4.5",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 64000,
          "name": "Anthropic: Claude Haiku 4.5",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "cacheControlFormat": "anthropic",
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 0.05,
            "cacheWrite": 0.625,
            "input": 0.5,
            "output": 2.5
          },
          "id": "anthropic/claude-haiku-4.5:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 64000,
          "name": "Anthropic: Claude Haiku 4.5 (batch)",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "anthropic-messages",
          "baseUrl": "https://openrouter.ai/api",
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 1.5,
            "cacheWrite": 18.75,
            "input": 15,
            "output": 75
          },
          "id": "anthropic/claude-opus-4.1",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32000,
          "name": "Anthropic: Claude Opus 4.1",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "cacheControlFormat": "anthropic",
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 0.75,
            "cacheWrite": 9.375,
            "input": 7.5,
            "output": 37.5
          },
          "id": "anthropic/claude-opus-4.1:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32000,
          "name": "Anthropic: Claude Opus 4.1 (batch)",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "anthropic-messages",
          "baseUrl": "https://openrouter.ai/api",
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 0.5,
            "cacheWrite": 6.25,
            "input": 5,
            "output": 25
          },
          "id": "anthropic/claude-opus-4.5",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 64000,
          "name": "Anthropic: Claude Opus 4.5",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "cacheControlFormat": "anthropic",
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 0.25,
            "cacheWrite": 3.125,
            "input": 2.5,
            "output": 12.5
          },
          "id": "anthropic/claude-opus-4.5:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 64000,
          "name": "Anthropic: Claude Opus 4.5 (batch)",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "anthropic-messages",
          "baseUrl": "https://openrouter.ai/api",
          "compat": {
            "forceAdaptiveThinking": true
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.5,
            "cacheWrite": 6.25,
            "input": 5,
            "output": 25
          },
          "id": "anthropic/claude-opus-4.6",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "Anthropic: Claude Opus 4.6",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "cacheControlFormat": "anthropic",
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.25,
            "cacheWrite": 3.125,
            "input": 2.5,
            "output": 12.5
          },
          "id": "anthropic/claude-opus-4.6:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "Anthropic: Claude Opus 4.6 (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "anthropic-messages",
          "baseUrl": "https://openrouter.ai/api",
          "compat": {
            "forceAdaptiveThinking": true,
            "supportsTemperature": false
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.5,
            "cacheWrite": 6.25,
            "input": 5,
            "output": 25
          },
          "id": "anthropic/claude-opus-4.7",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "Anthropic: Claude Opus 4.7",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "cacheControlFormat": "anthropic",
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.25,
            "cacheWrite": 3.125,
            "input": 2.5,
            "output": 12.5
          },
          "id": "anthropic/claude-opus-4.7:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "Anthropic: Claude Opus 4.7 (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "anthropic-messages",
          "baseUrl": "https://openrouter.ai/api",
          "compat": {
            "forceAdaptiveThinking": true,
            "supportsTemperature": false
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.5,
            "cacheWrite": 6.25,
            "input": 5,
            "output": 25
          },
          "id": "anthropic/claude-opus-4.8",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "Anthropic: Claude Opus 4.8",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "cacheControlFormat": "anthropic",
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.25,
            "cacheWrite": 3.125,
            "input": 2.5,
            "output": 12.5
          },
          "id": "anthropic/claude-opus-4.8:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "Anthropic: Claude Opus 4.8 (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "anthropic-messages",
          "baseUrl": "https://openrouter.ai/api",
          "compat": {
            "forceAdaptiveThinking": true,
            "supportsTemperature": false
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.5,
            "cacheWrite": 6.25,
            "input": 5,
            "output": 25
          },
          "id": "anthropic/claude-opus-5",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "Anthropic: Claude Opus 5",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "cacheControlFormat": "anthropic",
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.25,
            "cacheWrite": 3.125,
            "input": 2.5,
            "output": 12.5
          },
          "id": "anthropic/claude-opus-5:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "Anthropic: Claude Opus 5 (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "anthropic-messages",
          "baseUrl": "https://openrouter.ai/api",
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 0.3,
            "cacheWrite": 3.75,
            "input": 3,
            "output": 15
          },
          "id": "anthropic/claude-sonnet-4",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 64000,
          "name": "Anthropic: Claude Sonnet 4",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "anthropic-messages",
          "baseUrl": "https://openrouter.ai/api",
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.3,
            "cacheWrite": 3.75,
            "input": 3,
            "output": 15
          },
          "id": "anthropic/claude-sonnet-4.5",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 64000,
          "name": "Anthropic: Claude Sonnet 4.5",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "cacheControlFormat": "anthropic",
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.15,
            "cacheWrite": 1.875,
            "input": 1.5,
            "output": 7.5
          },
          "id": "anthropic/claude-sonnet-4.5:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 64000,
          "name": "Anthropic: Claude Sonnet 4.5 (batch)",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "anthropic-messages",
          "baseUrl": "https://openrouter.ai/api",
          "compat": {
            "forceAdaptiveThinking": true
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.3,
            "cacheWrite": 3.75,
            "input": 3,
            "output": 15
          },
          "id": "anthropic/claude-sonnet-4.6",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "Anthropic: Claude Sonnet 4.6",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "cacheControlFormat": "anthropic",
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.15,
            "cacheWrite": 1.875,
            "input": 1.5,
            "output": 7.5
          },
)cch_catalog";
constexpr char kCatalogPart2[] = R"cch_catalog(          "id": "anthropic/claude-sonnet-4.6:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "Anthropic: Claude Sonnet 4.6 (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "anthropic-messages",
          "baseUrl": "https://openrouter.ai/api",
          "compat": {
            "forceAdaptiveThinking": true
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.2,
            "cacheWrite": 2.5,
            "input": 2,
            "output": 10
          },
          "id": "anthropic/claude-sonnet-5",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "Anthropic: Claude Sonnet 5",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "cacheControlFormat": "anthropic",
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.1,
            "cacheWrite": 1.25,
            "input": 1,
            "output": 5
          },
          "id": "anthropic/claude-sonnet-5:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "Anthropic: Claude Sonnet 5 (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.06,
            "cacheWrite": 0,
            "input": 0.25,
            "output": 0.8
          },
          "id": "arcee-ai/trinity-large-thinking",
          "input": [
            "text"
          ],
          "maxTokens": 80000,
          "name": "Arcee AI: Trinity Large Thinking",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 2000000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0,
            "output": 0
          },
          "id": "auto",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 30000,
          "name": "Auto",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.25,
            "output": 2
          },
          "id": "bytedance-seed/seed-1.6",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32768,
          "name": "ByteDance Seed: Seed 1.6",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.075,
            "output": 0.3
          },
          "id": "bytedance-seed/seed-1.6-flash",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32768,
          "name": "ByteDance Seed: Seed 1.6 Flash",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.5,
            "output": 2.5
          },
          "id": "bytedance-seed/seed-2-1-turbo",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 235929,
          "name": "ByteDance Seed: Seed 2.1 Turbo",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.5,
            "output": 3
          },
          "id": "bytedance-seed/seed-2.0-code",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 131072,
          "name": "ByteDance Seed: Seed-2.0-Code",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.25,
            "output": 2
          },
          "id": "bytedance-seed/seed-2.0-lite",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 131072,
          "name": "ByteDance Seed: Seed-2.0-Lite",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.1,
            "output": 0.4
          },
          "id": "bytedance-seed/seed-2.0-mini",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 131072,
          "name": "ByteDance Seed: Seed-2.0-Mini",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.15,
            "output": 0.6
          },
          "id": "cohere/command-r-08-2024",
          "input": [
            "text"
          ],
          "maxTokens": 4000,
          "name": "Cohere: Command R (08-2024)",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 2.5,
            "output": 10
          },
          "id": "cohere/command-r-plus-08-2024",
          "input": [
            "text"
          ],
          "maxTokens": 4000,
          "name": "Cohere: Command R+ (08-2024)",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 256000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0,
            "output": 0
          },
          "id": "cohere/north-mini-code:free",
          "input": [
            "text"
          ],
          "maxTokens": 64000,
          "name": "Cohere: North Mini Code (free)",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 163840,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.32,
            "output": 0.89
          },
          "id": "deepseek/deepseek-chat",
          "input": [
            "text"
          ],
          "maxTokens": 16384,
          "name": "DeepSeek: DeepSeek V3",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 163840,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.25,
            "output": 1
          },
          "id": "deepseek/deepseek-chat-v3-0324",
          "input": [
            "text"
          ],
          "maxTokens": 147456,
          "name": "DeepSeek: DeepSeek V3 0324",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 163840,
          "cost": {
            "cacheRead": 0.13,
            "cacheWrite": 0,
            "input": 0.25,
            "output": 0.95
          },
          "id": "deepseek/deepseek-chat-v3.1",
          "input": [
            "text"
          ],
          "maxTokens": 32768,
          "name": "DeepSeek: DeepSeek V3.1",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 64000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.7,
            "output": 2.5
          },
          "id": "deepseek/deepseek-r1",
          "input": [
            "text"
          ],
          "maxTokens": 16000,
          "name": "DeepSeek: R1",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 163840,
          "cost": {
            "cacheRead": 0.35,
            "cacheWrite": 0,
            "input": 0.5,
            "output": 2.15
          },
          "id": "deepseek/deepseek-r1-0528",
          "input": [
            "text"
          ],
          "maxTokens": 32768,
          "name": "DeepSeek: R1 0528",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0.135,
            "cacheWrite": 0,
            "input": 0.27,
            "output": 1
          },
          "id": "deepseek/deepseek-v3.1-terminus",
          "input": [
            "text"
          ],
          "maxTokens": 32768,
          "name": "DeepSeek: DeepSeek V3.1 Terminus",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 163840,
          "cost": {
            "cacheRead": 0.1345,
            "cacheWrite": 0,
            "input": 0.269,
            "output": 0.4
          },
          "id": "deepseek/deepseek-v3.2",
          "input": [
            "text"
          ],
          "maxTokens": 65536,
          "name": "DeepSeek: DeepSeek V3.2",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 163840,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.27,
            "output": 0.41
          },
          "id": "deepseek/deepseek-v3.2-exp",
          "input": [
            "text"
          ],
          "maxTokens": 65536,
          "name": "DeepSeek: DeepSeek V3.2 Exp",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "requiresReasoningContentOnAssistantMessages": true,
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1024000,
          "cost": {
            "cacheRead": 0.017721,
            "cacheWrite": 0,
            "input": 0.088606,
            "output": 0.177212
          },
          "id": "deepseek/deepseek-v4-flash",
          "input": [
            "text"
          ],
          "maxTokens": 384000,
          "name": "DeepSeek: DeepSeek V4 Flash 0423",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "requiresReasoningContentOnAssistantMessages": true,
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.016,
            "cacheWrite": 0,
            "input": 0.04,
            "output": 0.64
          },
          "id": "deepseek/deepseek-v4-flash-0731",
          "input": [
            "text"
          ],
          "maxTokens": 943718,
          "name": "DeepSeek: DeepSeek V4 Flash 0731",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "requiresReasoningContentOnAssistantMessages": true,
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.0035,
            "cacheWrite": 0,
            "input": 0.11,
            "output": 0.33
          },
          "id": "deepseek/deepseek-v4-flash-0731:batch",
          "input": [
            "text"
          ],
          "maxTokens": 943718,
          "name": "DeepSeek: DeepSeek V4 Flash 0731 (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "requiresReasoningContentOnAssistantMessages": true,
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.007,
            "cacheWrite": 0,
            "input": 0.22,
            "output": 0.66
          },
          "id": "deepseek/deepseek-v4-flash-vision-exp",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 943718,
          "name": "DeepSeek: DeepSeek V4 Flash Vision Exp",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "requiresReasoningContentOnAssistantMessages": true,
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.0035,
            "cacheWrite": 0,
            "input": 0.11,
            "output": 0.33
          },
          "id": "deepseek/deepseek-v4-flash-vision-exp:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 943718,
          "name": "DeepSeek: DeepSeek V4 Flash Vision Exp (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "requiresReasoningContentOnAssistantMessages": true,
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1024000,
          "cost": {
            "cacheRead": 0.079605,
            "cacheWrite": 0,
            "input": 0.95526,
            "output": 1.91052
          },
          "id": "deepseek/deepseek-v4-pro",
          "input": [
            "text"
          ],
          "maxTokens": 384000,
          "name": "DeepSeek: DeepSeek V4 Pro 0423",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "requiresReasoningContentOnAssistantMessages": true,
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.022,
            "cacheWrite": 0,
            "input": 0.66,
            "output": 1.98
          },
          "id": "deepseek/deepseek-v4-pro-0813",
          "input": [
            "text"
          ],
          "maxTokens": 384000,
          "name": "DeepSeek: DeepSeek V4 Pro 0813",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "requiresReasoningContentOnAssistantMessages": true,
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.022,
            "cacheWrite": 0,
            "input": 0.66,
            "output": 1.98
          },
          "id": "deepseek/deepseek-v4-pro-0813:batch",
          "input": [
            "text"
          ],
          "maxTokens": 943718,
          "name": "DeepSeek: DeepSeek V4 Pro 0813 (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "requiresReasoningContentOnAssistantMessages": true,
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.003,
            "cacheWrite": 0,
            "input": 0.15,
            "output": 0.6
          },
          "id": "deepseek/deepseek-v4.1-flash",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 384000,
          "name": "DeepSeek: DeepSeek V4.1 Flash",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 512000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0,
            "output": 0
          },
          "id": "dots-studio/dots-3-note-preview:free",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 460800,
          "name": "Dots Studio: Dots3-Note Preview (free)",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.03,
            "cacheWrite": 0.083333,
            "input": 0.3,
            "output": 2.5
          },
          "id": "google/gemini-2.5-flash",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65535,
          "name": "Google: Gemini 2.5 Flash",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.01,
            "cacheWrite": 0.083333,
            "input": 0.1,
            "output": 0.4
          },
          "id": "google/gemini-2.5-flash-lite",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65535,
          "name": "Google: Gemini 2.5 Flash Lite",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.01,
            "cacheWrite": 0,
            "input": 0.05,
            "output": 0.2
          },
          "id": "google/gemini-2.5-flash-lite:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65535,
          "name": "Google: Gemini 2.5 Flash Lite (batch)",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.03,
            "cacheWrite": 0,
            "input": 0.15,
            "output": 1.25
          },
          "id": "google/gemini-2.5-flash:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65535,
          "name": "Google: Gemini 2.5 Flash (batch)",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.125,
            "cacheWrite": 0.375,
            "input": 1.25,
            "output": 10
          },
          "id": "google/gemini-2.5-pro",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Google: Gemini 2.5 Pro",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.125,
            "cacheWrite": 0.375,
            "input": 1.25,
            "output": 10
          },
          "id": "google/gemini-2.5-pro-preview",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Google: Gemini 2.5 Pro Preview 06-05",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.125,
            "cacheWrite": 0,
            "input": 0.625,
            "output": 5
          },
          "id": "google/gemini-2.5-pro:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Google: Gemini 2.5 Pro (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.05,
            "cacheWrite": 0.083333,
            "input": 0.5,
            "output": 3
          },
          "id": "google/gemini-3-flash-preview",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Google: Gemini 3 Flash Preview",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.25,
            "output": 1.5
          },
          "id": "google/gemini-3-flash-preview:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Google: Gemini 3 Flash Preview (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 65536,
          "cost": {
            "cacheRead": 0.2,
            "cacheWrite": 0.375,
            "input": 2,
            "output": 12
          },
          "id": "google/gemini-3-pro-image",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32768,
          "name": "Google: Nano Banana Pro (Gemini 3 Pro Image)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.025,
            "cacheWrite": 0.083333,
            "input": 0.25,
            "output": 1.5
          },
          "id": "google/gemini-3.1-flash-lite",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Google: Gemini 3.1 Flash Lite",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.025,
            "cacheWrite": 0.083333,
            "input": 0.25,
            "output": 1.5
          },
          "id": "google/gemini-3.1-flash-lite-preview",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Google: Gemini 3.1 Flash Lite Preview",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.0125,
            "cacheWrite": 0,
            "input": 0.125,
            "output": 0.75
          },
          "id": "google/gemini-3.1-flash-lite:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Google: Gemini 3.1 Flash Lite (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.2,
            "cacheWrite": 0.375,
            "input": 2,
            "output": 12
          },
          "id": "google/gemini-3.1-pro-preview",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Google: Gemini 3.1 Pro Preview",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.2,
            "cacheWrite": 0.375,
            "input": 2,
            "output": 12
          },
          "id": "google/gemini-3.1-pro-preview-customtools",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Google: Gemini 3.1 Pro Preview Custom Tools",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 1,
            "output": 6
          },
          "id": "google/gemini-3.1-pro-preview:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Google: Gemini 3.1 Pro Preview (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.15,
            "cacheWrite": 0.083333,
            "input": 1.5,
            "output": 9
          },
          "id": "google/gemini-3.5-flash",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Google: Gemini 3.5 Flash",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.03,
            "cacheWrite": 0.083333,
            "input": 0.3,
            "output": 2.5
          },
          "id": "google/gemini-3.5-flash-lite",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Google: Gemini 3.5 Flash Lite",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.015,
            "cacheWrite": 0,
            "input": 0.15,
            "output": 1.25
          },
          "id": "google/gemini-3.5-flash-lite:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Google: Gemini 3.5 Flash Lite (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.075,
            "cacheWrite": 0,
            "input": 0.75,
            "output": 4.5
          },
          "id": "google/gemini-3.5-flash:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Google: Gemini 3.5 Flash (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.075,
            "cacheWrite": 0.041667,
            "input": 0.75,
            "output": 3.75
          },
          "id": "google/gemini-3.6-flash",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Google: Gemini 3.6 Flash",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.0375,
            "cacheWrite": 0.041667,
            "input": 0.375,
            "output": 1.875
          },
          "id": "google/gemini-3.6-flash:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Google: Gemini 3.6 Flash (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.075,
            "cacheWrite": 0.041667,
            "input": 0.75,
            "output": 3.75
          },
          "id": "google/gemini-3.7-flash",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Google: Gemini 3.7 Flash",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.0375,
            "cacheWrite": 0.041667,
            "input": 0.375,
            "output": 1.875
          },
          "id": "google/gemini-3.7-flash:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Google: Gemini 3.7 Flash (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.075,
            "cacheWrite": 0.041667,
            "input": 0.75,
            "output": 3.75
          },
          "id": "google/gemini-3.8-flash",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
)cch_catalog";
constexpr char kCatalogPart3[] = R"cch_catalog(                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Google: Gemini 3.8 Flash",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.0375,
            "cacheWrite": 0.041667,
            "input": 0.375,
            "output": 1.875
          },
          "id": "google/gemini-3.8-flash:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Google: Gemini 3.8 Flash (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.05,
            "output": 0.15
          },
          "id": "google/gemma-3-12b-it",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 16384,
          "name": "Google: Gemma 3 12B",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0.04,
            "cacheWrite": 0,
            "input": 0.08,
            "output": 0.45
          },
          "id": "google/gemma-3-27b-it",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 117964,
          "name": "Google: Gemma 3 27B",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.05,
            "cacheWrite": 0,
            "input": 0.09,
            "output": 0.3
          },
          "id": "google/gemma-4-26b-a4b-it",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 235929,
          "name": "Google: Gemma 4 26B A4B ",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0,
            "output": 0
          },
          "id": "google/gemma-4-26b-a4b-it:free",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32768,
          "name": "Google: Gemma 4 26B A4B  (free)",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.05,
            "cacheWrite": 0,
            "input": 0.09,
            "output": 0.34
          },
          "id": "google/gemma-4-31b-it",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 16384,
          "name": "Google: Gemma 4 31B",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0,
            "output": 0
          },
          "id": "google/gemma-4-31b-it:free",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32768,
          "name": "Google: Gemma 4 31B (free)",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0.015,
            "cacheWrite": 0,
            "input": 0.06,
            "output": 0.25
          },
          "id": "ibm-granite/granite-4.2-8b",
          "input": [
            "text"
          ],
          "maxTokens": 117964,
          "name": "IBM: Granite 4.2 8B",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0.025,
            "cacheWrite": 0,
            "input": 0.25,
            "output": 0.75
          },
          "id": "inception/mercury-2",
          "input": [
            "text"
          ],
          "maxTokens": 50000,
          "name": "Inception: Mercury 2",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 260000,
          "cost": {
            "cacheRead": 0.004,
            "cacheWrite": 0,
            "input": 0.04,
            "output": 0.15
          },
          "id": "inception/mercury-2.5",
          "input": [
            "text"
          ],
          "maxTokens": 65536,
          "name": "Inception: Mercury 2.5",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.0042,
            "cacheWrite": 0,
            "input": 0.021,
            "output": 0.063
          },
          "id": "inclusionai/ling-3.0-flash",
          "input": [
            "text"
          ],
          "maxTokens": 32768,
          "name": "inclusionAI: Ling 3.0 Flash",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.012,
            "cacheWrite": 0,
            "input": 0.06,
            "output": 0.18
          },
          "id": "inclusionai/ling-3.0-flash-fin",
          "input": [
            "text"
          ],
          "maxTokens": 235929,
          "name": "inclusionAI: Ling 3.0 Flash Fin",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0,
            "output": 0
          },
          "id": "inclusionai/ling-3.0-flash-fin:free",
          "input": [
            "text"
          ],
          "maxTokens": 32768,
          "name": "inclusionAI: Ling 3.0 Flash Fin (free)",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0,
            "output": 0
          },
          "id": "inclusionai/ling-3.0-flash-sante:free",
          "input": [
            "text"
          ],
          "maxTokens": 32768,
          "name": "inclusionAI: Ling 3.0 Flash Sante (free)",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0.012,
            "cacheWrite": 0,
            "input": 0.06,
            "output": 0.18
          },
          "id": "inclusionai/ling-3.0-flash-vl",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32768,
          "name": "inclusionAI: Ling 3.0 Flash VL",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0,
            "output": 0
          },
          "id": "inclusionai/ling-3.0-flash-vl:free",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32768,
          "name": "inclusionAI: Ling 3.0 Flash VL (free)",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.06,
            "cacheWrite": 0,
            "input": 0.3,
            "output": 1.2
          },
          "id": "kwaipilot/kat-coder-pro-v2",
          "input": [
            "text"
          ],
          "maxTokens": 144000,
          "name": "Kwaipilot: KAT-Coder-Pro V2",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.15,
            "cacheWrite": 0,
            "input": 0.74,
            "output": 2.96
          },
          "id": "kwaipilot/kat-coder-pro-v2.5",
          "input": [
            "text"
          ],
          "maxTokens": 235929,
          "name": "Kwaipilot: KAT-Coder-Pro V2.5",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 65536,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0,
            "output": 0
          },
          "id": "liquid/lfm-2.5-2.6b:free",
          "input": [
            "text"
          ],
          "maxTokens": 8192,
          "name": "LiquidAI: LFM2.5-2.6B (free)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048756,
          "cost": {
            "cacheRead": 0.006,
            "cacheWrite": 0,
            "input": 0.3,
            "output": 1.2
          },
          "id": "meituan/longcat-2.0",
          "input": [
            "text"
          ],
          "maxTokens": 262144,
          "name": "Meituan: LongCat 2.0",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.4,
            "output": 0.4
          },
          "id": "meta-llama/llama-3.1-70b-instruct",
          "input": [
            "text"
          ],
          "maxTokens": 16384,
          "name": "Meta: Llama 3.1 70B Instruct",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0.025,
            "cacheWrite": 0,
            "input": 0.05,
            "output": 0.08
          },
          "id": "meta-llama/llama-3.1-8b-instruct",
          "input": [
            "text"
          ],
          "maxTokens": 117964,
          "name": "Meta: Llama 3.1 8B Instruct",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.1,
            "output": 0.32
          },
          "id": "meta-llama/llama-3.3-70b-instruct",
          "input": [
            "text"
          ],
          "maxTokens": 16384,
          "name": "Meta: Llama 3.3 70B Instruct",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.1875,
            "output": 0.6525
          },
          "id": "meta-llama/llama-4-maverick",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 16384,
          "name": "Meta: Llama 4 Maverick",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 327680,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.1,
            "output": 0.3
          },
          "id": "meta-llama/llama-4-scout",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 16384,
          "name": "Meta: Llama 4 Scout",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0.04,
            "cacheWrite": 0,
            "input": 0.3,
            "output": 1.2
          },
          "id": "meta/muse-glimmer-30b",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 16384,
          "name": "Meta: Muse Glimmer 30B",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0.02,
            "cacheWrite": 0,
            "input": 0.175,
            "output": 0.75
          },
          "id": "meta/muse-glimmer-30b:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 117964,
          "name": "Meta: Muse Glimmer 30B (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.15,
            "cacheWrite": 0,
            "input": 1.25,
            "output": 4.25
          },
          "id": "meta/muse-spark-1.1",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 943718,
          "name": "Meta: Muse Spark 1.1",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.15,
            "cacheWrite": 0,
            "input": 1.25,
            "output": 4.25
          },
          "id": "meta/muse-spark-1.2",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 943718,
          "name": "Meta: Muse Spark 1.2",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.002,
            "cacheWrite": 0,
            "input": 0.1,
            "output": 0.2
          },
          "id": "meta/muse-spark-1.2-contributor",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 943718,
          "name": "Meta: Muse Spark 1.2 Contributor",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.15,
            "cacheWrite": 0,
            "input": 1.25,
            "output": 4.25
          },
          "id": "meta/muse-spark-1.3",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 943718,
          "name": "Meta: Muse Spark 1.3",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": "minimal",
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.002,
            "cacheWrite": 0,
            "input": 0.1,
            "output": 0.2
          },
          "id": "meta/muse-spark-1.3-contributor",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 943718,
          "name": "Meta: Muse Spark 1.3 Contributor",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": "minimal",
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.4,
            "output": 2.2
          },
          "id": "minimax/minimax-m1",
          "input": [
            "text"
          ],
          "maxTokens": 40000,
          "name": "MiniMax: MiniMax M1",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 204800,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.255,
            "output": 1.02
          },
          "id": "minimax/minimax-m2",
          "input": [
            "text"
          ],
          "maxTokens": 131072,
          "name": "MiniMax: MiniMax M2",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 204800,
          "cost": {
            "cacheRead": 0.03,
            "cacheWrite": 0,
            "input": 0.3,
            "output": 1.2
          },
          "id": "minimax/minimax-m2.1",
          "input": [
            "text"
          ],
          "maxTokens": 131072,
          "name": "MiniMax: MiniMax M2.1",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 0.027,
            "cacheWrite": 0,
            "input": 0.27,
            "output": 1.08
          },
          "id": "minimax/minimax-m2.5",
          "input": [
            "text"
          ],
          "maxTokens": 128000,
          "name": "MiniMax: MiniMax M2.5",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 204800,
          "cost": {
            "cacheRead": 0.06,
            "cacheWrite": 0,
            "input": 0.3,
            "output": 1.2
          },
          "id": "minimax/minimax-m2.7",
          "input": [
            "text"
          ],
          "maxTokens": 131072,
          "name": "MiniMax: MiniMax M2.7",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 524288,
          "cost": {
            "cacheRead": 0.06,
            "cacheWrite": 0,
            "input": 0.3,
            "output": 1.2
          },
          "id": "minimax/minimax-m3",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 512000,
          "name": "MiniMax: MiniMax M3",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 256000,
          "cost": {
            "cacheRead": 0.03,
            "cacheWrite": 0,
            "input": 0.3,
            "output": 0.9
          },
          "id": "mistralai/codestral-2508",
          "input": [
            "text"
          ],
          "maxTokens": 204800,
          "name": "Mistral: Codestral 2508",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 256000,
          "cost": {
            "cacheRead": 0.015,
            "cacheWrite": 0,
            "input": 0.15,
            "output": 0.45
          },
          "id": "mistralai/codestral-2508:batch",
          "input": [
            "text"
          ],
          "maxTokens": 204800,
          "name": "Mistral: Codestral 2508 (batch)",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.04,
            "cacheWrite": 0,
            "input": 0.4,
            "output": 2
          },
          "id": "mistralai/devstral-2512",
          "input": [
            "text"
          ],
          "maxTokens": 209715,
          "name": "Mistral: Devstral 2 2512",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.02,
            "cacheWrite": 0,
            "input": 0.2,
            "output": 0.2
          },
          "id": "mistralai/ministral-14b-2512",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 209715,
          "name": "Mistral: Ministral 3 14B 2512",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0.01,
            "cacheWrite": 0,
            "input": 0.1,
            "output": 0.1
          },
          "id": "mistralai/ministral-3b-2512",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 104857,
          "name": "Mistral: Ministral 3 3B 2512",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.015,
            "cacheWrite": 0,
            "input": 0.15,
            "output": 0.15
          },
          "id": "mistralai/ministral-8b-2512",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 209715,
          "name": "Mistral: Ministral 3 8B 2512",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.0075,
            "cacheWrite": 0,
            "input": 0.075,
            "output": 0.075
          },
          "id": "mistralai/ministral-8b-2512:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 209715,
          "name": "Mistral: Ministral 3 8B 2512 (batch)",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0.2,
            "cacheWrite": 0,
            "input": 2,
            "output": 6
          },
          "id": "mistralai/mistral-large",
          "input": [
            "text"
          ],
          "maxTokens": 102400,
          "name": "Mistral Large",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0.2,
            "cacheWrite": 0,
            "input": 2,
            "output": 6
          },
          "id": "mistralai/mistral-large-2407",
          "input": [
            "text"
          ],
          "maxTokens": 104857,
          "name": "Mistral Large 2407",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.025,
            "cacheWrite": 0,
            "input": 0.25,
            "output": 0.75
          },
          "id": "mistralai/mistral-large-2512:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 209715,
          "name": "Mistral: Mistral Large 3 2512 (batch)",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0.04,
            "cacheWrite": 0,
            "input": 0.4,
            "output": 2
          },
          "id": "mistralai/mistral-medium-3",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 104857,
          "name": "Mistral: Mistral Medium 3",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 1.5,
            "output": 7.5
          },
          "id": "mistralai/mistral-medium-3-5",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 209715,
          "name": "Mistral: Mistral Medium 3.5",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.75,
            "output": 3.75
          },
          "id": "mistralai/mistral-medium-3-5:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 209715,
          "name": "Mistral: Mistral Medium 3.5 (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0.04,
            "cacheWrite": 0,
            "input": 0.4,
            "output": 2
          },
          "id": "mistralai/mistral-medium-3.1",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 104857,
          "name": "Mistral: Mistral Medium 3.1",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0.02,
            "cacheWrite": 0,
            "input": 0.2,
            "output": 1
          },
          "id": "mistralai/mistral-medium-3.1:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 104857,
          "name": "Mistral: Mistral Medium 3.1 (batch)",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.019,
            "output": 0.03
          },
          "id": "mistralai/mistral-nemo",
          "input": [
            "text"
          ],
          "maxTokens": 16384,
          "name": "Mistral: Mistral Nemo",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 32768,
          "cost": {
            "cacheRead": 0.02,
            "cacheWrite": 0,
            "input": 0.2,
            "output": 0.6
          },
          "id": "mistralai/mistral-saba",
          "input": [
            "text"
          ],
          "maxTokens": 26214,
          "name": "Mistral: Saba",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.015,
            "cacheWrite": 0,
            "input": 0.15,
            "output": 0.6
          },
          "id": "mistralai/mistral-small-2603",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 209715,
          "name": "Mistral: Mistral Small 4",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.0075,
            "cacheWrite": 0,
            "input": 0.075,
            "output": 0.3
          },
          "id": "mistralai/mistral-small-2603:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 209715,
          "name": "Mistral: Mistral Small 4 (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.351,
            "output": 0.555
          },
          "id": "mistralai/mistral-small-3.1-24b-instruct",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 102400,
          "name": "Mistral: Mistral Small 3.1 24B",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 256000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.09375,
            "output": 0.25
          },
          "id": "mistralai/mistral-small-3.2-24b-instruct",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 16384,
          "name": "Mistral: Mistral Small 3.2 24B",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 65536,
          "cost": {
            "cacheRead": 0.2,
            "cacheWrite": 0,
            "input": 2,
            "output": 6
          },
          "id": "mistralai/mixtral-8x22b-instruct",
          "input": [
            "text"
          ],
          "maxTokens": 52428,
          "name": "Mistral: Mixtral 8x22B Instruct",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 32768,
          "cost": {
            "cacheRead": 0.01,
            "cacheWrite": 0,
            "input": 0.1,
            "output": 0.3
          },
          "id": "mistralai/voxtral-small-24b-2507",
          "input": [
            "text"
          ],
          "maxTokens": 26214,
          "name": "Mistral: Voxtral Small 24B 2507",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.57,
            "output": 2.3
          },
          "id": "moonshotai/kimi-k2",
          "input": [
            "text"
          ],
          "maxTokens": 98304,
          "name": "MoonshotAI: Kimi K2 0711",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.6,
            "output": 2.5
          },
          "id": "moonshotai/kimi-k2-0905",
          "input": [
            "text"
          ],
          "maxTokens": 98304,
          "name": "MoonshotAI: Kimi K2 0905",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.15,
            "cacheWrite": 0,
            "input": 0.6,
            "output": 2.5
          },
          "id": "moonshotai/kimi-k2-thinking",
          "input": [
            "text"
          ],
          "maxTokens": 98304,
          "name": "MoonshotAI: Kimi K2 Thinking",
)cch_catalog";
constexpr char kCatalogPart4[] = R"cch_catalog(          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.07,
            "cacheWrite": 0,
            "input": 0.41,
            "output": 2.06
          },
          "id": "moonshotai/kimi-k2.5",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 4096,
          "name": "MoonshotAI: Kimi K2.5",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "requiresReasoningContentOnAssistantMessages": true,
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.16,
            "cacheWrite": 0,
            "input": 0.95,
            "output": 4
          },
          "id": "moonshotai/kimi-k2.6",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 235929,
          "name": "MoonshotAI: Kimi K2.6",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.18,
            "cacheWrite": 0,
            "input": 0.7062,
            "output": 3.21
          },
          "id": "moonshotai/kimi-k2.7-code",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 235929,
          "name": "MoonshotAI: Kimi K2.7 Code",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.3,
            "cacheWrite": 0,
            "input": 3,
            "output": 15
          },
          "id": "moonshotai/kimi-k3",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 131072,
          "name": "MoonshotAI: Kimi K3",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0,
            "output": 0
          },
          "id": "nex-agi/nex-n2.5-mini:free",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 235929,
          "name": "Nex AGI: Nex-N2.5-Mini (free)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.015,
            "cacheWrite": 0,
            "input": 0.075,
            "output": 0.25
          },
          "id": "nex-agi/nex-n2.5-pro",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 235929,
          "name": "Nex AGI: Nex-N2.5-Pro",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0,
            "output": 0
          },
          "id": "nex-agi/nex-n2.5-pro:free",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 235929,
          "name": "Nex AGI: Nex-N2.5-Pro (free)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.03,
            "cacheWrite": 0,
            "input": 0.05,
            "output": 0.2
          },
          "id": "nvidia/nemotron-3-nano-30b-a3b",
          "input": [
            "text"
          ],
          "maxTokens": 235929,
          "name": "NVIDIA: Nemotron 3 Nano 30B A3B",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 256000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0,
            "output": 0
          },
          "id": "nvidia/nemotron-3-nano-omni-30b-a3b-reasoning:free",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "NVIDIA: Nemotron 3 Nano Omni (free)",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.08,
            "output": 0.45
          },
          "id": "nvidia/nemotron-3-super-120b-a12b",
          "input": [
            "text"
          ],
          "maxTokens": 235929,
          "name": "NVIDIA: Nemotron 3 Super",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": null,
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0,
            "output": 0
          },
          "id": "nvidia/nemotron-3-super-120b-a12b:free",
          "input": [
            "text"
          ],
          "maxTokens": 235929,
          "name": "NVIDIA: Nemotron 3 Super (free)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": null,
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 202800,
          "cost": {
            "cacheRead": 0.12,
            "cacheWrite": 0,
            "input": 0.6,
            "output": 2.4
          },
          "id": "nvidia/nemotron-3-ultra-550b-a55b",
          "input": [
            "text"
          ],
          "maxTokens": 182520,
          "name": "NVIDIA: Nemotron 3 Ultra",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0,
            "output": 0
          },
          "id": "nvidia/nemotron-3-ultra-550b-a55b:free",
          "input": [
            "text"
          ],
          "maxTokens": 65536,
          "name": "NVIDIA: Nemotron 3 Ultra (free)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.04,
            "cacheWrite": 0,
            "input": 0.07,
            "output": 0.2
          },
          "id": "nvidia/nemotron-3.5-lightning",
          "input": [
            "text"
          ],
          "maxTokens": 235929,
          "name": "NVIDIA: Nemotron 3.5 Lightning",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0,
            "output": 0
          },
          "id": "nvidia/nemotron-3.5-lightning:free",
          "input": [
            "text"
          ],
          "maxTokens": 65536,
          "name": "NVIDIA: Nemotron 3.5 Lightning (free)",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 16385,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.5,
            "output": 1.5
          },
          "id": "openai/gpt-3.5-turbo",
          "input": [
            "text"
          ],
          "maxTokens": 4096,
          "name": "OpenAI: GPT-3.5 Turbo",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 4095,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 1,
            "output": 2
          },
          "id": "openai/gpt-3.5-turbo-0613",
          "input": [
            "text"
          ],
          "maxTokens": 3685,
          "name": "OpenAI: GPT-3.5 Turbo (older v0613)",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 16385,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 3,
            "output": 4
          },
          "id": "openai/gpt-3.5-turbo-16k",
          "input": [
            "text"
          ],
          "maxTokens": 4096,
          "name": "OpenAI: GPT-3.5 Turbo 16k",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 16385,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.25,
            "output": 0.75
          },
          "id": "openai/gpt-3.5-turbo:batch",
          "input": [
            "text"
          ],
          "maxTokens": 4096,
          "name": "OpenAI: GPT-3.5 Turbo (batch)",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 8191,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 30,
            "output": 60
          },
          "id": "openai/gpt-4",
          "input": [
            "text"
          ],
          "maxTokens": 4096,
          "name": "OpenAI: GPT-4",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 10,
            "output": 30
          },
          "id": "openai/gpt-4-turbo",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 4096,
          "name": "OpenAI: GPT-4 Turbo",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 5,
            "output": 15
          },
          "id": "openai/gpt-4-turbo:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 4096,
          "name": "OpenAI: GPT-4 Turbo (batch)",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1047576,
          "cost": {
            "cacheRead": 0.5,
            "cacheWrite": 0,
            "input": 2,
            "output": 8
          },
          "id": "openai/gpt-4.1",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32768,
          "name": "OpenAI: GPT-4.1",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1047576,
          "cost": {
            "cacheRead": 0.1,
            "cacheWrite": 0,
            "input": 0.4,
            "output": 1.6
          },
          "id": "openai/gpt-4.1-mini",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32768,
          "name": "OpenAI: GPT-4.1 Mini",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1047576,
          "cost": {
            "cacheRead": 0.05,
            "cacheWrite": 0,
            "input": 0.2,
            "output": 0.8
          },
          "id": "openai/gpt-4.1-mini:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32768,
          "name": "OpenAI: GPT-4.1 Mini (batch)",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1047576,
          "cost": {
            "cacheRead": 0.025,
            "cacheWrite": 0,
            "input": 0.1,
            "output": 0.4
          },
          "id": "openai/gpt-4.1-nano",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32768,
          "name": "OpenAI: GPT-4.1 Nano",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1047576,
          "cost": {
            "cacheRead": 0.0125,
            "cacheWrite": 0,
            "input": 0.05,
            "output": 0.2
          },
          "id": "openai/gpt-4.1-nano:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32768,
          "name": "OpenAI: GPT-4.1 Nano (batch)",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1047576,
          "cost": {
            "cacheRead": 0.25,
            "cacheWrite": 0,
            "input": 1,
            "output": 4
          },
          "id": "openai/gpt-4.1:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32768,
          "name": "OpenAI: GPT-4.1 (batch)",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 1.25,
            "cacheWrite": 0,
            "input": 2.5,
            "output": 10
          },
          "id": "openai/gpt-4o",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 16384,
          "name": "OpenAI: GPT-4o",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 5,
            "output": 15
          },
          "id": "openai/gpt-4o-2024-05-13",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 4096,
          "name": "OpenAI: GPT-4o (2024-05-13)",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 1.25,
            "cacheWrite": 0,
            "input": 2.5,
            "output": 10
          },
          "id": "openai/gpt-4o-2024-08-06",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 16384,
          "name": "OpenAI: GPT-4o (2024-08-06)",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 1.25,
            "cacheWrite": 0,
            "input": 2.5,
            "output": 10
          },
          "id": "openai/gpt-4o-2024-11-20",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 16384,
          "name": "OpenAI: GPT-4o (2024-11-20)",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0.075,
            "cacheWrite": 0,
            "input": 0.15,
            "output": 0.6
          },
          "id": "openai/gpt-4o-mini",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 16384,
          "name": "OpenAI: GPT-4o-mini",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0.075,
            "cacheWrite": 0,
            "input": 0.15,
            "output": 0.6
          },
          "id": "openai/gpt-4o-mini-2024-07-18",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 16384,
          "name": "OpenAI: GPT-4o-mini (2024-07-18)",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0.0375,
            "cacheWrite": 0,
            "input": 0.075,
            "output": 0.3
          },
          "id": "openai/gpt-4o-mini:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 16384,
          "name": "OpenAI: GPT-4o-mini (batch)",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0.625,
            "cacheWrite": 0,
            "input": 1.25,
            "output": 5
          },
          "id": "openai/gpt-4o:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 16384,
          "name": "OpenAI: GPT-4o (batch)",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.125,
            "cacheWrite": 0,
            "input": 1.25,
            "output": 10
          },
          "id": "openai/gpt-5",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.025,
            "cacheWrite": 0,
            "input": 0.25,
            "output": 2
          },
          "id": "openai/gpt-5-mini",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5 Mini",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.0125,
            "cacheWrite": 0,
            "input": 0.125,
            "output": 1
          },
          "id": "openai/gpt-5-mini:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5 Mini (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.005,
            "cacheWrite": 0,
            "input": 0.05,
            "output": 0.4
          },
          "id": "openai/gpt-5-nano",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5 Nano",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.0025,
            "cacheWrite": 0,
            "input": 0.025,
            "output": 0.2
          },
          "id": "openai/gpt-5-nano:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5 Nano (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 15,
            "output": 120
          },
          "id": "openai/gpt-5-pro",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5 Pro",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": null,
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 7.5,
            "output": 60
          },
          "id": "openai/gpt-5-pro:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5 Pro (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": null,
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.125,
            "cacheWrite": 0,
            "input": 1.25,
            "output": 10
          },
          "id": "openai/gpt-5.1",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.1",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.13,
            "cacheWrite": 0,
            "input": 1.25,
            "output": 10
          },
          "id": "openai/gpt-5.1-codex",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.1-Codex",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.125,
            "cacheWrite": 0,
            "input": 1.25,
            "output": 10
          },
          "id": "openai/gpt-5.1-codex-max",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.1-Codex-Max",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.03,
            "cacheWrite": 0,
            "input": 0.25,
            "output": 2
          },
          "id": "openai/gpt-5.1-codex-mini",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.1-Codex-Mini",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.0625,
            "cacheWrite": 0,
            "input": 0.625,
            "output": 5
          },
          "id": "openai/gpt-5.1:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.1 (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.175,
            "cacheWrite": 0,
            "input": 1.75,
            "output": 14
          },
          "id": "openai/gpt-5.2",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.2",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0.175,
            "cacheWrite": 0,
            "input": 1.75,
            "output": 14
          },
          "id": "openai/gpt-5.2-chat",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32000,
          "name": "OpenAI: GPT-5.2 Chat",
          "provider": "openrouter",
          "reasoning": false,
          "thinkingLevelMap": {
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.175,
            "cacheWrite": 0,
            "input": 1.75,
            "output": 14
          },
          "id": "openai/gpt-5.2-codex",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.2-Codex",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 21,
            "output": 168
          },
          "id": "openai/gpt-5.2-pro",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.2 Pro",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 10.5,
            "output": 84
          },
          "id": "openai/gpt-5.2-pro:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.2 Pro (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.0875,
            "cacheWrite": 0,
            "input": 0.875,
            "output": 7
          },
          "id": "openai/gpt-5.2:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.2 (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.175,
            "cacheWrite": 0,
            "input": 1.75,
            "output": 14
          },
          "id": "openai/gpt-5.3-codex",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.3-Codex",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsMidConvoSystemMessages": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0.25,
            "cacheWrite": 0,
            "input": 2.5,
            "output": 15
          },
          "id": "openai/gpt-5.4",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.4",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsMidConvoSystemMessages": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.075,
            "cacheWrite": 0,
            "input": 0.75,
            "output": 4.5
          },
          "id": "openai/gpt-5.4-mini",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.4 Mini",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.0375,
            "cacheWrite": 0,
            "input": 0.375,
            "output": 2.25
          },
          "id": "openai/gpt-5.4-mini:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.4 Mini (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
)cch_catalog";
constexpr char kCatalogPart5[] = R"cch_catalog(            "thinkingFormat": "openrouter"
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.02,
            "cacheWrite": 0,
            "input": 0.2,
            "output": 1.25
          },
          "id": "openai/gpt-5.4-nano",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.4 Nano",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.01,
            "cacheWrite": 0,
            "input": 0.1,
            "output": 0.625
          },
          "id": "openai/gpt-5.4-nano:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.4 Nano (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsMidConvoSystemMessages": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 30,
            "output": 180
          },
          "id": "openai/gpt-5.4-pro",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.4 Pro",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 15,
            "output": 90
          },
          "id": "openai/gpt-5.4-pro:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.4 Pro (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0.125,
            "cacheWrite": 0,
            "input": 1.25,
            "output": 7.5
          },
          "id": "openai/gpt-5.4:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.4 (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsMidConvoSystemMessages": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0.5,
            "cacheWrite": 0,
            "input": 5,
            "output": 30
          },
          "id": "openai/gpt-5.5",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.5",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 30,
            "output": 180
          },
          "id": "openai/gpt-5.5-pro",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.5 Pro",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 15,
            "output": 90
          },
          "id": "openai/gpt-5.5-pro:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.5 Pro (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0.25,
            "cacheWrite": 0,
            "input": 2.5,
            "output": 15
          },
          "id": "openai/gpt-5.5:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.5 (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsMidConvoSystemMessages": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0.02,
            "cacheWrite": 0.25,
            "input": 0.2,
            "output": 1.2
          },
          "id": "openai/gpt-5.6-luna",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.6 Luna",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0.02,
            "cacheWrite": 0.25,
            "input": 0.2,
            "output": 1.2
          },
          "id": "openai/gpt-5.6-luna-pro",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.6 Luna Pro",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0.01,
            "cacheWrite": 0,
            "input": 0.1,
            "output": 0.6
          },
          "id": "openai/gpt-5.6-luna-pro:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.6 Luna Pro (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0.01,
            "cacheWrite": 0,
            "input": 0.1,
            "output": 0.6
          },
          "id": "openai/gpt-5.6-luna:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.6 Luna (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsMidConvoSystemMessages": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0.2,
            "cacheWrite": 2.5,
            "input": 2,
            "output": 10
          },
          "id": "openai/gpt-5.6-sol",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.6 Sol",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0.2,
            "cacheWrite": 2.5,
            "input": 2,
            "output": 10
          },
          "id": "openai/gpt-5.6-sol-pro",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.6 Sol Pro",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0.1,
            "cacheWrite": 1.25,
            "input": 1,
            "output": 5
          },
          "id": "openai/gpt-5.6-sol-pro:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.6 Sol Pro (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0.1,
            "cacheWrite": 1.25,
            "input": 1,
            "output": 5
          },
          "id": "openai/gpt-5.6-sol:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.6 Sol (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsMidConvoSystemMessages": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0.2,
            "cacheWrite": 2.5,
            "input": 2,
            "output": 12
          },
          "id": "openai/gpt-5.6-terra",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.6 Terra",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0.2,
            "cacheWrite": 2.5,
            "input": 2,
            "output": 12
          },
          "id": "openai/gpt-5.6-terra-pro",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.6 Terra Pro",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0.1,
            "cacheWrite": 0,
            "input": 1,
            "output": 6
          },
          "id": "openai/gpt-5.6-terra-pro:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.6 Terra Pro (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0.1,
            "cacheWrite": 0,
            "input": 1,
            "output": 6
          },
          "id": "openai/gpt-5.6-terra:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5.6 Terra (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.0625,
            "cacheWrite": 0,
            "input": 0.625,
            "output": 5
          },
          "id": "openai/gpt-5:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-5 (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsMidConvoSystemMessages": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 1,
            "cacheWrite": 12.5,
            "input": 10,
            "output": 50
          },
          "id": "openai/gpt-6-astra",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-6 Astra",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 1,
            "cacheWrite": 12.5,
            "input": 10,
            "output": 50
          },
          "id": "openai/gpt-6-astra-pro",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-6 Astra Pro",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0.5,
            "cacheWrite": 6.25,
            "input": 5,
            "output": 25
          },
          "id": "openai/gpt-6-astra-pro:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-6 Astra Pro (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0.5,
            "cacheWrite": 6.25,
            "input": 5,
            "output": 25
          },
          "id": "openai/gpt-6-astra:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT-6 Astra (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 2.5,
            "output": 10
          },
          "id": "openai/gpt-audio",
          "input": [
            "text"
          ],
          "maxTokens": 16384,
          "name": "OpenAI: GPT Audio",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.6,
            "output": 2.4
          },
          "id": "openai/gpt-audio-mini",
          "input": [
            "text"
          ],
          "maxTokens": 16384,
          "name": "OpenAI: GPT Audio Mini",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.5,
            "cacheWrite": 0,
            "input": 5,
            "output": 30
          },
          "id": "openai/gpt-chat-latest",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT Chat Latest",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0.075,
            "cacheWrite": 0,
            "input": 0.15,
            "output": 0.6
          },
          "id": "openai/gpt-oss-120b",
          "input": [
            "text"
          ],
          "maxTokens": 65536,
          "name": "OpenAI: gpt-oss-120b",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0.03,
            "cacheWrite": 0,
            "input": 0.03,
            "output": 0.13
          },
          "id": "openai/gpt-oss-20b",
          "input": [
            "text"
          ],
          "maxTokens": 117964,
          "name": "OpenAI: gpt-oss-20b",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0.0375,
            "cacheWrite": 0,
            "input": 0.075,
            "output": 0.3
          },
          "id": "openai/gpt-oss-safeguard-20b",
          "input": [
            "text"
          ],
          "maxTokens": 65536,
          "name": "OpenAI: gpt-oss-safeguard-20b",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 7.5,
            "cacheWrite": 0,
            "input": 15,
            "output": 60
          },
          "id": "openai/o1",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 100000,
          "name": "OpenAI: o1",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 0.5,
            "cacheWrite": 0,
            "input": 2,
            "output": 8
          },
          "id": "openai/o3",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 100000,
          "name": "OpenAI: o3",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 0.55,
            "cacheWrite": 0,
            "input": 1.1,
            "output": 4.4
          },
          "id": "openai/o3-mini",
          "input": [
            "text"
          ],
          "maxTokens": 100000,
          "name": "OpenAI: o3 Mini",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 0.55,
            "cacheWrite": 0,
            "input": 1.1,
            "output": 4.4
          },
          "id": "openai/o3-mini-high",
          "input": [
            "text"
          ],
          "maxTokens": 100000,
          "name": "OpenAI: o3 Mini High",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": null,
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 0.275,
            "cacheWrite": 0,
            "input": 0.55,
            "output": 2.2
          },
          "id": "openai/o3-mini:batch",
          "input": [
            "text"
          ],
          "maxTokens": 100000,
          "name": "OpenAI: o3 Mini (batch)",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 20,
            "output": 80
          },
          "id": "openai/o3-pro",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 100000,
          "name": "OpenAI: o3 Pro",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 0.25,
            "cacheWrite": 0,
            "input": 1,
            "output": 4
          },
          "id": "openai/o3:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 100000,
          "name": "OpenAI: o3 (batch)",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 0.275,
            "cacheWrite": 0,
            "input": 1.1,
            "output": 4.4
          },
          "id": "openai/o4-mini",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 100000,
          "name": "OpenAI: o4 Mini",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 0.275,
            "cacheWrite": 0,
            "input": 1.1,
            "output": 4.4
          },
          "id": "openai/o4-mini-high",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 100000,
          "name": "OpenAI: o4 Mini High",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": null,
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 0.1375,
            "cacheWrite": 0,
            "input": 0.55,
            "output": 2.2
          },
          "id": "openai/o4-mini:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 100000,
          "name": "OpenAI: o4 Mini (batch)",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 2000000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": -1000000,
            "output": -1000000
          },
          "id": "openrouter/auto",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 4096,
          "name": "Auto Router",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 2000000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": -1000000,
            "output": -1000000
          },
          "id": "openrouter/auto-beta",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 4096,
          "name": "Auto Router (Beta)",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0,
            "output": 0
          },
          "id": "openrouter/free",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 4096,
          "name": "Free Models Router",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0,
            "output": 0
          },
          "id": "openrouter/fusion",
          "input": [
            "text"
          ],
          "maxTokens": 30000,
          "name": "OpenRouter: Fusion",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.009,
            "cacheWrite": 0,
            "input": 0.09,
            "output": 0.18
          },
          "id": "poolside/laguna-s-2.1",
          "input": [
            "text"
          ],
          "maxTokens": 131072,
          "name": "Poolside: Laguna S 2.1",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0,
            "output": 0
          },
          "id": "poolside/laguna-s-2.1:free",
          "input": [
            "text"
          ],
          "maxTokens": 32768,
          "name": "Poolside: Laguna S 2.1 (free)",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.03,
            "cacheWrite": 0,
            "input": 0.06,
            "output": 0.12
          },
          "id": "poolside/laguna-xs-2.1",
          "input": [
            "text"
          ],
          "maxTokens": 32768,
          "name": "Poolside: Laguna XS 2.1",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0,
            "output": 0
          },
          "id": "poolside/laguna-xs-2.1:free",
          "input": [
            "text"
          ],
          "maxTokens": 32768,
          "name": "Poolside: Laguna XS 2.1 (free)",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.075,
            "output": 0.5
          },
          "id": "prism-ml/ternary-bonsai-2-27b",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32768,
          "name": "PrismML: Ternary Bonsai 2 27B",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": null,
            "low": null,
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 32768,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.36,
            "output": 0.4
          },
          "id": "qwen/qwen-2.5-72b-instruct",
          "input": [
            "text"
          ],
          "maxTokens": 16384,
          "name": "Qwen2.5 72B Instruct",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 32768,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.1,
            "output": 0.2
          },
          "id": "qwen/qwen-2.5-7b-instruct",
          "input": [
            "text"
          ],
          "maxTokens": 29491,
          "name": "Qwen: Qwen2.5 7B Instruct",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.052,
            "cacheWrite": 0.325,
            "input": 0.26,
            "output": 0.78
          },
          "id": "qwen/qwen-plus",
          "input": [
            "text"
          ],
          "maxTokens": 32768,
          "name": "Qwen: Qwen-Plus",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.26,
            "output": 0.78
          },
          "id": "qwen/qwen-plus-2025-07-28",
          "input": [
            "text"
          ],
          "maxTokens": 32768,
          "name": "Qwen: Qwen Plus 0728",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 40960,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.12,
            "output": 0.24
          },
          "id": "qwen/qwen3-14b",
          "input": [
            "text"
          ],
          "maxTokens": 16384,
          "name": "Qwen: Qwen3 14B",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.455,
            "output": 1.82
          },
          "id": "qwen/qwen3-235b-a22b",
          "input": [
            "text"
          ],
          "maxTokens": 8192,
          "name": "Qwen: Qwen3 235B A22B",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.0175,
            "cacheWrite": 0,
            "input": 0.0875,
            "output": 0.35
          },
          "id": "qwen/qwen3-235b-a22b-2507",
          "input": [
            "text"
          ],
          "maxTokens": 235929,
          "name": "Qwen: Qwen3 235B A22B Instruct 2507",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.23,
            "output": 2.3
          },
          "id": "qwen/qwen3-235b-a22b-thinking-2507",
          "input": [
            "text"
          ],
          "maxTokens": 117964,
          "name": "Qwen: Qwen3 235B A22B Thinking 2507",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 40960,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.12,
            "output": 0.5
          },
          "id": "qwen/qwen3-30b-a3b",
          "input": [
            "text"
          ],
          "maxTokens": 16384,
          "name": "Qwen: Qwen3 30B A3B",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 128000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.04815,
            "output": 0.19305
          },
          "id": "qwen/qwen3-30b-a3b-instruct-2507",
          "input": [
            "text"
          ],
          "maxTokens": 32000,
          "name": "Qwen: Qwen3 30B A3B Instruct 2507",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
)cch_catalog";
constexpr char kCatalogPart6[] = R"cch_catalog(          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 81920,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.2,
            "output": 2.4
          },
          "id": "qwen/qwen3-30b-a3b-thinking-2507",
          "input": [
            "text"
          ],
          "maxTokens": 32768,
          "name": "Qwen: Qwen3 30B A3B Thinking 2507",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 40960,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.08,
            "output": 0.28
          },
          "id": "qwen/qwen3-32b",
          "input": [
            "text"
          ],
          "maxTokens": 16384,
          "name": "Qwen: Qwen3 32B",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.117,
            "output": 0.455
          },
          "id": "qwen/qwen3-8b",
          "input": [
            "text"
          ],
          "maxTokens": 8192,
          "name": "Qwen: Qwen3 8B",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.1,
            "cacheWrite": 0,
            "input": 0.3,
            "output": 1
          },
          "id": "qwen/qwen3-coder",
          "input": [
            "text"
          ],
          "maxTokens": 65536,
          "name": "Qwen: Qwen3 Coder 480B A35B",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.07,
            "output": 0.28
          },
          "id": "qwen/qwen3-coder-30b-a3b-instruct",
          "input": [
            "text"
          ],
          "maxTokens": 235929,
          "name": "Qwen: Qwen3 Coder 30B A3B Instruct",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.039,
            "cacheWrite": 0.24375,
            "input": 0.195,
            "output": 0.975
          },
          "id": "qwen/qwen3-coder-flash",
          "input": [
            "text"
          ],
          "maxTokens": 65536,
          "name": "Qwen: Qwen3 Coder Flash",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.07,
            "cacheWrite": 0,
            "input": 0.12,
            "output": 0.8
          },
          "id": "qwen/qwen3-coder-next",
          "input": [
            "text"
          ],
          "maxTokens": 235929,
          "name": "Qwen: Qwen3 Coder Next",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.13,
            "cacheWrite": 0.8125,
            "input": 0.65,
            "output": 3.25
          },
          "id": "qwen/qwen3-coder-plus",
          "input": [
            "text"
          ],
          "maxTokens": 65536,
          "name": "Qwen: Qwen3 Coder Plus",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.156,
            "cacheWrite": 0.975,
            "input": 0.78,
            "output": 3.9
          },
          "id": "qwen/qwen3-max",
          "input": [
            "text"
          ],
          "maxTokens": 65536,
          "name": "Qwen: Qwen3 Max",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.78,
            "output": 3.9
          },
          "id": "qwen/qwen3-max-thinking",
          "input": [
            "text"
          ],
          "maxTokens": 65536,
          "name": "Qwen: Qwen3 Max Thinking",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.09,
            "output": 1.1
          },
          "id": "qwen/qwen3-next-80b-a3b-instruct",
          "input": [
            "text"
          ],
          "maxTokens": 16384,
          "name": "Qwen: Qwen3 Next 80B A3B Instruct",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.15,
            "output": 1.2
          },
          "id": "qwen/qwen3-next-80b-a3b-thinking",
          "input": [
            "text"
          ],
          "maxTokens": 32768,
          "name": "Qwen: Qwen3 Next 80B A3B Thinking",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0.1,
            "cacheWrite": 0,
            "input": 0.21,
            "output": 1.9
          },
          "id": "qwen/qwen3-vl-235b-a22b-instruct",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32768,
          "name": "Qwen: Qwen3 VL 235B A22B Instruct",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.4,
            "output": 4
          },
          "id": "qwen/qwen3-vl-235b-a22b-thinking",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32768,
          "name": "Qwen: Qwen3 VL 235B A22B Thinking",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.13,
            "output": 0.52
          },
          "id": "qwen/qwen3-vl-30b-a3b-instruct",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32768,
          "name": "Qwen: Qwen3 VL 30B A3B Instruct",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.2,
            "output": 2.4
          },
          "id": "qwen/qwen3-vl-30b-a3b-thinking",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32768,
          "name": "Qwen: Qwen3 VL 30B A3B Thinking",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.104,
            "output": 0.416
          },
          "id": "qwen/qwen3-vl-32b-instruct",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32768,
          "name": "Qwen: Qwen3 VL 32B Instruct",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.117,
            "output": 0.455
          },
          "id": "qwen/qwen3-vl-8b-instruct",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32768,
          "name": "Qwen: Qwen3 VL 8B Instruct",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.18,
            "output": 2.1
          },
          "id": "qwen/qwen3-vl-8b-thinking",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32768,
          "name": "Qwen: Qwen3 VL 8B Thinking",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.26,
            "output": 2.08
          },
          "id": "qwen/qwen3.5-122b-a10b",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Qwen: Qwen3.5-122B-A10B",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.195,
            "output": 1.56
          },
          "id": "qwen/qwen3.5-27b",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Qwen: Qwen3.5-27B",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 256000,
          "cost": {
            "cacheRead": 0.15625,
            "cacheWrite": 0,
            "input": 0.3125,
            "output": 1.25
          },
          "id": "qwen/qwen3.5-35b-a3b",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 16384,
          "name": "Qwen: Qwen3.5-35B-A3B",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.225,
            "cacheWrite": 0,
            "input": 0.55,
            "output": 3.5
          },
          "id": "qwen/qwen3.5-397b-a17b",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 235929,
          "name": "Qwen: Qwen3.5 397B A17B",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 256000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.1,
            "output": 0.15
          },
          "id": "qwen/qwen3.5-9b",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32768,
          "name": "Qwen: Qwen3.5-9B",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.065,
            "output": 0.26
          },
          "id": "qwen/qwen3.5-flash-02-23",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Qwen: Qwen3.5-Flash",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.26,
            "output": 1.56
          },
          "id": "qwen/qwen3.5-plus-02-15",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Qwen: Qwen3.5 Plus 2026-02-15",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0.375,
            "input": 0.3,
            "output": 1.8
          },
          "id": "qwen/qwen3.5-plus-20260420",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Qwen: Qwen3.5 Plus 2026-04-20",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.03,
            "cacheWrite": 0,
            "input": 0.3,
            "output": 2
          },
          "id": "qwen/qwen3.6-27b",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Qwen: Qwen3.6 27B",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.05,
            "cacheWrite": 0,
            "input": 0.15,
            "output": 1
          },
          "id": "qwen/qwen3.6-35b-a3b",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 235929,
          "name": "Qwen: Qwen3.6 35B A3B",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0.234375,
            "input": 0.1875,
            "output": 1.125
          },
          "id": "qwen/qwen3.6-flash",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Qwen: Qwen3.6 Flash",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 1.28375,
            "input": 1.027,
            "output": 6.162
          },
          "id": "qwen/qwen3.6-max-preview",
          "input": [
            "text"
          ],
          "maxTokens": 65536,
          "name": "Qwen: Qwen3.6 Max Preview",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0.40625,
            "input": 0.325,
            "output": 1.95
          },
          "id": "qwen/qwen3.6-plus",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Qwen: Qwen3.6 Plus",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.006,
            "cacheWrite": 0.038,
            "input": 0.03,
            "output": 0.13
          },
          "id": "qwen/qwen3.7-flash",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Qwen: Qwen3.7 Flash",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.295,
            "cacheWrite": 1.84375,
            "input": 1.475,
            "output": 4.425
          },
          "id": "qwen/qwen3.7-max",
          "input": [
            "text"
          ],
          "maxTokens": 131072,
          "name": "Qwen: Qwen3.7 Max",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.064,
            "cacheWrite": 0.4,
            "input": 0.32,
            "output": 1.28
          },
          "id": "qwen/qwen3.7-plus",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 131072,
          "name": "Qwen: Qwen3.7 Plus",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.25,
            "cacheWrite": 0,
            "input": 2,
            "output": 6
          },
          "id": "qwen/qwen3.8-2.4t-a95b",
          "input": [
            "text"
          ],
          "maxTokens": 131072,
          "name": "Qwen: Qwen3.8 2.4T A95B",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": null,
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.085,
            "cacheWrite": 0,
            "input": 0.42,
            "output": 3
          },
          "id": "qwen/qwen3.8-27b",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 131072,
          "name": "Qwen: Qwen3.8 27B",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": null,
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0,
            "output": 0
          },
          "id": "qwen/qwen3.8-27b:free",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 235929,
          "name": "Qwen: Qwen3.8 27B (free)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": null,
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.016,
            "cacheWrite": 0.2,
            "input": 0.15,
            "output": 0.47
          },
          "id": "qwen/qwen3.8-flash",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 131072,
          "name": "Qwen: Qwen3.8 Flash",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.25,
            "cacheWrite": 2.5,
            "input": 2,
            "output": 6
          },
          "id": "qwen/qwen3.8-max-0902",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 131072,
          "name": "Qwen: Qwen3.8 Max (0902)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 16384,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.1,
            "output": 0.1
          },
          "id": "rekaai/reka-edge",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 14745,
          "name": "Reka Edge",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 256000,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 1,
            "output": 3
          },
          "id": "relace/relace-search",
          "input": [
            "text"
          ],
          "maxTokens": 128000,
          "name": "Relace: Relace Search",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.25,
            "cacheWrite": 0,
            "input": 2,
            "output": 6
          },
          "id": "sakana/fugu-max",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "Sakana: Fugu Max",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.5,
            "cacheWrite": 0,
            "input": 5,
            "output": 30
          },
          "id": "sakana/fugu-ultra",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "Sakana: Fugu Ultra",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.5,
            "cacheWrite": 0,
            "input": 5,
            "output": 30
          },
          "id": "sakana/fugu-ultra-v2",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "Sakana: Fugu Ultra v2",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.15,
            "cacheWrite": 0,
            "input": 0.95,
            "output": 4
          },
          "id": "sakana/sakana-namazu",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Sakana: Sakana Namazu",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.85,
            "output": 0.85
          },
          "id": "sao10k/l3.1-euryale-70b",
          "input": [
            "text"
          ],
          "maxTokens": 16384,
          "name": "Sao10K: Llama 3.1 Euryale 70B v2.2",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.1,
            "output": 0.3
          },
          "id": "stepfun/step-3.5-flash",
          "input": [
            "text"
          ],
          "maxTokens": 65536,
          "name": "StepFun: Step 3.5 Flash",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 256000,
          "cost": {
            "cacheRead": 0.04,
            "cacheWrite": 0,
            "input": 0.2,
            "output": 1.15
          },
          "id": "stepfun/step-3.7-flash",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 230400,
          "name": "StepFun: Step 3.7 Flash",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.033,
            "cacheWrite": 0,
            "input": 0.132,
            "output": 0.528
          },
          "id": "tencent/hy3",
          "input": [
            "text"
          ],
          "maxTokens": 128000,
          "name": "Tencent: Hy3",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.06,
            "cacheWrite": 0,
            "input": 0.18,
            "output": 0.6
          },
          "id": "tencent/hy3-preview",
          "input": [
            "text"
          ],
          "maxTokens": 235929,
          "name": "Tencent: Hy3 preview",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.042,
            "cacheWrite": 0,
            "input": 0.834,
            "output": 2.501
          },
          "id": "tencent/hy4-preview",
          "input": [
            "text"
          ],
          "maxTokens": 64000,
          "name": "Tencent: Hy4 preview",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 524288,
          "cost": {
            "cacheRead": 0.17,
            "cacheWrite": 0,
            "input": 1,
            "output": 4.05
          },
          "id": "thinkingmachines/inkling",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 471859,
          "name": "Thinking Machines: Inkling",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": "minimal",
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 524288,
          "cost": {
            "cacheRead": 0.1,
            "cacheWrite": 0,
            "input": 0.45,
            "output": 1.2
          },
          "id": "thinkingmachines/inkling-small",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 262144,
          "name": "Thinking Machines: Inkling Small",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": "minimal",
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0,
            "output": 0
          },
          "id": "thinkingmachines/inkling-small:free",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 262144,
          "name": "Thinking Machines: Inkling Small (free)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": "minimal",
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0,
            "output": 0
          },
          "id": "thinkingmachines/inkling:free",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 262144,
          "name": "Thinking Machines: Inkling (free)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": "minimal",
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 262144,
          "cost": {
            "cacheRead": 0.25,
            "cacheWrite": 0,
            "input": 2.5,
            "output": 7.5
          },
          "id": "unbiased/pareto",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 131072,
          "name": "Pareto",
          "provider": "openrouter",
          "reasoning": false
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0.015,
            "cacheWrite": 0,
            "input": 0.15,
            "output": 0.6
          },
          "id": "upstage/solar-pro-3",
          "input": [
            "text"
          ],
          "maxTokens": 117964,
          "name": "Upstage: Solar Pro 3",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": "minimal",
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 524288,
          "cost": {
            "cacheRead": 0.018,
            "cacheWrite": 0,
            "input": 0.09,
            "output": 0.36
          },
          "id": "upstage/solar-pro4",
          "input": [
            "text"
          ],
          "maxTokens": 131072,
          "name": "Upstage: Solar Pro 4",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": "minimal",
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 2000000,
          "cost": {
            "cacheRead": 0.2,
            "cacheWrite": 0,
            "input": 1.25,
            "output": 2.5
          },
          "id": "x-ai/grok-4.20",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 1800000,
          "name": "SpaceXAI: Grok 4.20",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.2,
            "cacheWrite": 0,
            "input": 1.25,
            "output": 2.5
          },
          "id": "x-ai/grok-4.3",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 900000,
          "name": "SpaceXAI: Grok 4.3",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.16,
            "cacheWrite": 0,
            "input": 1,
            "output": 2
          },
          "id": "x-ai/grok-4.3:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
)cch_catalog";
constexpr char kCatalogPart7[] = R"cch_catalog(          "maxTokens": 900000,
          "name": "SpaceXAI: Grok 4.3 (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 500000,
          "cost": {
            "cacheRead": 0.3,
            "cacheWrite": 0,
            "input": 2,
            "output": 6
          },
          "id": "x-ai/grok-4.5",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 450000,
          "name": "SpaceXAI: Grok 4.5",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 500000,
          "cost": {
            "cacheRead": 0.5,
            "cacheWrite": 0,
            "input": 2,
            "output": 6
          },
          "id": "x-ai/grok-4.6",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 450000,
          "name": "SpaceXAI: Grok 4.6",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 500000,
          "cost": {
            "cacheRead": 0.4,
            "cacheWrite": 0,
            "input": 1.6,
            "output": 4.8
          },
          "id": "x-ai/grok-4.7",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 450000,
          "name": "SpaceXAI: Grok 4.7",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 256000,
          "cost": {
            "cacheRead": 0.2,
            "cacheWrite": 0,
            "input": 1,
            "output": 2
          },
          "id": "x-ai/grok-build-0.1",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 230400,
          "name": "SpaceXAI: Grok Build 0.1",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "off": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.0028,
            "cacheWrite": 0,
            "input": 0.14,
            "output": 0.28
          },
          "id": "xiaomi/mimo-v2.5",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 131072,
          "name": "Xiaomi: MiMo-V2.5",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.0036,
            "cacheWrite": 0,
            "input": 0.435,
            "output": 0.87
          },
          "id": "xiaomi/mimo-v2.5-pro",
          "input": [
            "text"
          ],
          "maxTokens": 131072,
          "name": "Xiaomi: MiMo-V2.5-Pro",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.0028,
            "cacheWrite": 0,
            "input": 0.14,
            "output": 0.28
          },
          "id": "xiaomi/mimo-v2.6-flash",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 131072,
          "name": "Xiaomi: MiMo-V2.6-Flash",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.0036,
            "cacheWrite": 0,
            "input": 0.435,
            "output": 0.87
          },
          "id": "xiaomi/mimo-v2.6-pro",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 131072,
          "name": "Xiaomi: MiMo-V2.6-Pro",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.036,
            "cacheWrite": 0,
            "input": 4.35,
            "output": 8.7
          },
          "id": "xiaomi/mimo-v2.6-pro-ultraspeed",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 131072,
          "name": "Xiaomi: MiMo-V2.6-Pro-UltraSpeed",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0.11,
            "cacheWrite": 0,
            "input": 0.6,
            "output": 2.2
          },
          "id": "z-ai/glm-4.5",
          "input": [
            "text"
          ],
          "maxTokens": 98304,
          "name": "Z.ai: GLM 4.5",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0.025,
            "cacheWrite": 0,
            "input": 0.13,
            "output": 0.85
          },
          "id": "z-ai/glm-4.5-air",
          "input": [
            "text"
          ],
          "maxTokens": 98304,
          "name": "Z.ai: GLM 4.5 Air",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 65536,
          "cost": {
            "cacheRead": 0.11,
            "cacheWrite": 0,
            "input": 0.6,
            "output": 1.8
          },
          "id": "z-ai/glm-4.5v",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 16384,
          "name": "Z.ai: GLM 4.5V",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 198000,
          "cost": {
            "cacheRead": 0.08,
            "cacheWrite": 0,
            "input": 0.43,
            "output": 1.75
          },
          "id": "z-ai/glm-4.6",
          "input": [
            "text"
          ],
          "maxTokens": 16384,
          "name": "Z.ai: GLM 4.6",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0.055,
            "cacheWrite": 0,
            "input": 0.3,
            "output": 0.9
          },
          "id": "z-ai/glm-4.6v",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 32768,
          "name": "Z.ai: GLM 4.6V",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 202752,
          "cost": {
            "cacheRead": 0.08,
            "cacheWrite": 0,
            "input": 0.4,
            "output": 1.75
          },
          "id": "z-ai/glm-4.7",
          "input": [
            "text"
          ],
          "maxTokens": 131072,
          "name": "Z.ai: GLM 4.7",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 131072,
          "cost": {
            "cacheRead": 0,
            "cacheWrite": 0,
            "input": 0.0605,
            "output": 0.4
          },
          "id": "z-ai/glm-4.7-flash",
          "input": [
            "text"
          ],
          "maxTokens": 117964,
          "name": "Z.ai: GLM 4.7 Flash",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 198000,
          "cost": {
            "cacheRead": 0.119,
            "cacheWrite": 0,
            "input": 0.6,
            "output": 1.9
          },
          "id": "z-ai/glm-5",
          "input": [
            "text"
          ],
          "maxTokens": 128000,
          "name": "Z.ai: GLM 5",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 202752,
          "cost": {
            "cacheRead": 0.24,
            "cacheWrite": 0,
            "input": 1.2,
            "output": 4
          },
          "id": "z-ai/glm-5-turbo",
          "input": [
            "text"
          ],
          "maxTokens": 131072,
          "name": "Z.ai: GLM 5 Turbo",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 0.1794,
            "cacheWrite": 0,
            "input": 0.966,
            "output": 3.036
          },
          "id": "z-ai/glm-5.1",
          "input": [
            "text"
          ],
          "maxTokens": 128000,
          "name": "Z.ai: GLM 5.1",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.12064,
            "cacheWrite": 0,
            "input": 0.6496,
            "output": 2.0416
          },
          "id": "z-ai/glm-5.2",
          "input": [
            "text"
          ],
          "maxTokens": 131072,
          "name": "Z.ai: GLM 5.2",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.07,
            "cacheWrite": 0,
            "input": 0.7,
            "output": 2.2
          },
          "id": "z-ai/glm-5.2:batch",
          "input": [
            "text"
          ],
          "maxTokens": 943718,
          "name": "Z.ai: GLM 5.2 (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": null,
            "max": null,
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.156,
            "cacheWrite": 0,
            "input": 0.84,
            "output": 2.64
          },
          "id": "z-ai/glm-5.3",
          "input": [
            "text"
          ],
          "maxTokens": 131072,
          "name": "Z.ai: GLM 5.3",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.05,
            "cacheWrite": 0,
            "input": 0.15,
            "output": 0.5
          },
          "id": "z-ai/glm-5.3-flash",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 943718,
          "name": "Z.ai: GLM 5.3 Flash",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.015,
            "cacheWrite": 0,
            "input": 0.075,
            "output": 0.25
          },
          "id": "z-ai/glm-5.3-flash:batch",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 943718,
          "name": "Z.ai: GLM 5.3 Flash (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.075,
            "cacheWrite": 0,
            "input": 0.37,
            "output": 1.25
          },
          "id": "z-ai/glm-5.3-flashx",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 131072,
          "name": "Z.ai: GLM 5.3 FlashX",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.13,
            "cacheWrite": 0,
            "input": 0.7,
            "output": 2.2
          },
          "id": "z-ai/glm-5.3:batch",
          "input": [
            "text"
          ],
          "maxTokens": 943718,
          "name": "Z.ai: GLM 5.3 (batch)",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 202752,
          "cost": {
            "cacheRead": 0.24,
            "cacheWrite": 0,
            "input": 1.2,
            "output": 4
          },
          "id": "z-ai/glm-5v-turbo",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 131072,
          "name": "Z.ai: GLM 5V Turbo",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "cacheControlFormat": "anthropic",
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.25,
            "cacheWrite": 12.5,
            "input": 10,
            "output": 50
          },
          "id": "~anthropic/claude-fable-latest",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "Anthropic: Claude Fable Latest",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "cacheControlFormat": "anthropic",
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 200000,
          "cost": {
            "cacheRead": 0.1,
            "cacheWrite": 1.25,
            "input": 1,
            "output": 5
          },
          "id": "~anthropic/claude-haiku-latest",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 64000,
          "name": "Anthropic: Claude Haiku Latest",
          "provider": "openrouter",
          "reasoning": true
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "cacheControlFormat": "anthropic",
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.5,
            "cacheWrite": 6.25,
            "input": 5,
            "output": 25
          },
          "id": "~anthropic/claude-opus-latest",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "Anthropic: Claude Opus Latest",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "cacheControlFormat": "anthropic",
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1000000,
          "cost": {
            "cacheRead": 0.2,
            "cacheWrite": 2.5,
            "input": 2,
            "output": 10
          },
          "id": "~anthropic/claude-sonnet-latest",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "Anthropic: Claude Sonnet Latest",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.0036,
            "cacheWrite": 0,
            "input": 0.12,
            "output": 0.48
          },
          "id": "~deepseek/deepseek-flash-latest",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 943718,
          "name": "DeepSeek: DeepSeek Flash Latest",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1024000,
          "cost": {
            "cacheRead": 0.021287,
            "cacheWrite": 0,
            "input": 0.638616,
            "output": 1.915848
          },
          "id": "~deepseek/deepseek-pro-latest",
          "input": [
            "text"
          ],
          "maxTokens": 384000,
          "name": "DeepSeek: DeepSeek Pro Latest",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "requiresReasoningContentOnAssistantMessages": true,
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.008,
            "cacheWrite": 0,
            "input": 0.03,
            "output": 0.8
          },
          "id": "~deepseek/deepseek-v4-flash-latest",
          "input": [
            "text"
          ],
          "maxTokens": 943718,
          "name": "DeepSeek: DeepSeek V4 Flash Latest",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.075,
            "cacheWrite": 0.041667,
            "input": 0.75,
            "output": 3.75
          },
          "id": "~google/gemini-flash-latest",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Google: Gemini Flash Latest",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.2,
            "cacheWrite": 0.375,
            "input": 2,
            "output": 12
          },
          "id": "~google/gemini-pro-latest",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 65536,
          "name": "Google: Gemini Pro Latest",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.15,
            "cacheWrite": 0,
            "input": 1.5,
            "output": 7.5
          },
          "id": "~moonshotai/kimi-latest",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 131072,
          "name": "MoonshotAI: Kimi Latest",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": "none",
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 1,
            "cacheWrite": 12.5,
            "input": 10,
            "output": 50
          },
          "id": "~openai/gpt-astra-latest",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT Astra Latest",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0.02,
            "cacheWrite": 0.25,
            "input": 0.2,
            "output": 1.2
          },
          "id": "~openai/gpt-luna-latest",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT Luna Latest",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 400000,
          "cost": {
            "cacheRead": 0.075,
            "cacheWrite": 0,
            "input": 0.75,
            "output": 4.5
          },
          "id": "~openai/gpt-mini-latest",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT Mini Latest",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0.2,
            "cacheWrite": 2.5,
            "input": 2,
            "output": 10
          },
          "id": "~openai/gpt-sol-latest",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT Sol Latest",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1050000,
          "cost": {
            "cacheRead": 0.2,
            "cacheWrite": 2.5,
            "input": 2,
            "output": 12
          },
          "id": "~openai/gpt-terra-latest",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 128000,
          "name": "OpenAI: GPT Terra Latest",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": "medium",
            "minimal": null,
            "off": "none",
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 500000,
          "cost": {
            "cacheRead": 0.4,
            "cacheWrite": 0,
            "input": 1.6,
            "output": 4.8
          },
          "id": "~x-ai/grok-latest",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 450000,
          "name": "xAI: Grok Latest",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": null,
            "medium": "medium",
            "minimal": null,
            "off": null,
            "xhigh": "xhigh"
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.015,
            "cacheWrite": 0,
            "input": 0.075,
            "output": 0.25
          },
          "id": "~z-ai/glm-flash-latest",
          "input": [
            "text",
            "image"
          ],
          "inputLimits": {
            "images": {
              "resize": {
                "jpegQuality": 80,
                "maxBytes": 4718592,
                "maxHeight": 2000,
                "maxWidth": 2000
              }
            }
          },
          "maxTokens": 943718,
          "name": "Z.ai: GLM Flash Latest",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        },
        {
          "api": "openai-completions",
          "baseUrl": "https://openrouter.ai/api/v1",
          "compat": {
            "sendSessionAffinityHeaders": true,
            "supportsDeveloperRole": false,
            "supportsStrictMode": true,
            "thinkingFormat": "openrouter"
          },
          "contextWindow": 1048576,
          "cost": {
            "cacheRead": 0.107525,
            "cacheWrite": 0,
            "input": 0.6545,
            "output": 2.057
          },
          "id": "~z-ai/glm-latest",
          "input": [
            "text"
          ],
          "maxTokens": 943718,
          "name": "Z.ai: GLM Latest",
          "provider": "openrouter",
          "reasoning": true,
          "thinkingLevelMap": {
            "high": "high",
            "low": "low",
            "max": "max",
            "medium": null,
            "minimal": null,
            "off": null,
            "xhigh": null
          }
        }
      ]
    }
  }
}
)cch_catalog";

template <std::size_t DestinationSize, std::size_t PartSize>
void append_catalog_part(
        std::array<char, DestinationSize>& destination, std::size_t& offset, const char (&part)[PartSize]) {
    for (std::size_t index = 0; index + 1 < PartSize; ++index) {
        destination[offset++] = part[index];
    }
}

const std::array<char, 478323> kCatalogData = [] {
    std::array<char, 478323> result{};
    std::size_t offset = 0;
    append_catalog_part(result, offset, kCatalogPart0);
    append_catalog_part(result, offset, kCatalogPart1);
    append_catalog_part(result, offset, kCatalogPart2);
    append_catalog_part(result, offset, kCatalogPart3);
    append_catalog_part(result, offset, kCatalogPart4);
    append_catalog_part(result, offset, kCatalogPart5);
    append_catalog_part(result, offset, kCatalogPart6);
    append_catalog_part(result, offset, kCatalogPart7);
    return result;
}();

} // namespace

[[nodiscard]] std::string_view default_models_json() noexcept { return {kCatalogData.data(), kCatalogData.size()}; }

} // namespace cch::ai
