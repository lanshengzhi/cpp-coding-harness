#pragma once

#include <cch/coding_agent/ProjectResources.hpp>
#include "support/BoundedText.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace cch::coding_agent::detail {

inline constexpr std::size_t kMaxResourceDiagnostics = 64;
inline constexpr std::size_t kMaxResourceDiagnosticTextBytes = 1024;

inline void bound_resource_diagnostic_text(std::string& text) {
    text = support::bounded_redacted_text(std::move(text), kMaxResourceDiagnosticTextBytes, "...[truncated]");
}

[[nodiscard]] inline ResourceDiagnostic warning_diagnostic(
        std::string message, std::optional<std::string> path = std::nullopt) {
    return ResourceDiagnostic{
            .type = ResourceDiagnosticType::Warning,
            .message = std::move(message),
            .path = std::move(path),
            .collision = std::nullopt,
    };
}

[[nodiscard]] inline ResourceCollision make_resource_collision(ResourceCollisionResourceType resource_type,
        std::string name,
        std::string winner_path,
        std::string loser_path,
        std::optional<std::string> winner_source = std::nullopt,
        std::optional<std::string> loser_source = std::nullopt) {
    return ResourceCollision{
            .resource_type = resource_type,
            .name = std::move(name),
            .winner_path = std::move(winner_path),
            .loser_path = std::move(loser_path),
            .winner_source = std::move(winner_source),
            .loser_source = std::move(loser_source),
    };
}

[[nodiscard]] inline ResourceDiagnostic make_collision_diagnostic(std::string message,
        ResourceCollisionResourceType resource_type,
        std::string name,
        std::string winner_path,
        std::string loser_path,
        std::optional<std::string> winner_source = std::nullopt,
        std::optional<std::string> loser_source = std::nullopt) {
    return ResourceDiagnostic{
            .type = ResourceDiagnosticType::Collision,
            .message = std::move(message),
            .path = loser_path,
            .collision = make_resource_collision(resource_type,
                    std::move(name),
                    std::move(winner_path),
                    std::move(loser_path),
                    std::move(winner_source),
                    std::move(loser_source)),
    };
}

} // namespace cch::coding_agent::detail
