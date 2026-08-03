#pragma once

#include <cch/ai/Model.hpp>

#include <vector>

namespace cch::ai::providers {

/// Frozen OpenAI Codex catalog from pi-ai@0.83.0 (generated shard
/// `providers/data/openai-codex.json` at baseline `83114817`).
///
/// The shard carries `supportsOpenAIGrammarTools`/`supportsToolSearch` compat
/// flags that are Deferred in the C++ supported subset (ADR 0033): the C++
/// `Model.compat` is typed to `AnthropicMessagesCompat` only, so the Codex
/// catalog ships with no compat value and the adapter fixes grammar/tool-search
/// behavior at pi's frozen defaults.
[[nodiscard]] std::vector<Model> codex_models();

} // namespace cch::ai::providers
