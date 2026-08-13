#pragma once

#include <cch/ai/ModelStream.hpp>
#include <cch/coding_agent/AuthGuidance.hpp>
#include <cch/util/Error.hpp>

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace cch::coding_agent::runtime {

/// Whether a provider authenticates through an OAuth credential (pi
/// `ModelRuntime.isUsingOAuth`). Injected so the request-time branch is
/// decidable through a narrow predicate rather than a virtual Models Runtime.
using OAuthProviderPredicate =
    std::move_only_function<bool(std::string_view)>;

/// Decorate one AI-owned `ModelStream` with pi's request-time re-auth guidance
/// (pi `agent-session.ts` `_getRequiredRequestAuth`). Wraps the stream so an
/// auth-category (`auth`/`oauth`) terminal failure maps to pi's two verbatim
/// re-auth guidance branches: no usable key → `formatNoApiKeyFoundMessage`;
/// OAuth credential missing/expired → "Credentials may have expired … Run
/// '/login X'". The terminal-error-event contract is preserved: exactly one
/// `error` terminal event plus an agreeing final `AssistantMessage`, with the
/// category flowing through the single `util::Expected` channel (#326) — no
/// second exception hierarchy. Only the stream surface is decorated.
[[nodiscard]] ai::ModelStream apply_auth_guidance(
    ai::ModelStream inner,
    std::string provider,
    OAuthProviderPredicate is_using_oauth,
    std::filesystem::path docs_path =
        std::filesystem::path{kDefaultAuthGuidanceDocsPath});

} // namespace cch::coding_agent::runtime
