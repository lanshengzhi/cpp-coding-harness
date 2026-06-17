#include "SessionLifecycle.hpp"

#include <utility>

namespace cch::coding_agent::runtime {
namespace {

bool same_workspace(const std::filesystem::path& first, const std::filesystem::path& second) {
    std::error_code first_ec;
    std::error_code second_ec;
    auto first_canonical = std::filesystem::weakly_canonical(first, first_ec);
    auto second_canonical = std::filesystem::weakly_canonical(second, second_ec);
    if (first_ec || second_ec) {
        return first.lexically_normal() == second.lexically_normal();
    }
    return first_canonical == second_canonical;
}

} // namespace

util::Expected<OpenSession> open_session(SessionOpenRequest request) {
    OpenSession session;
    session.workspace = request.workspace;

    if (!request.resume_path.empty()) {
        auto loaded = harness::session::JsonlSessionStore::load(request.resume_path);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        if (!loaded->metadata.workspace.empty()) {
            if (request.workspace_explicit && !same_workspace(session.workspace, loaded->metadata.workspace)) {
                return std::unexpected(util::make_error(
                    util::ErrorCode::Session,
                    "resume workspace does not match session metadata",
                    "resume workspace does not match session metadata; omit --workspace to use " +
                        loaded->metadata.workspace.string() + " or start a new session"));
            }
            if (!request.workspace_explicit) {
                session.workspace = loaded->metadata.workspace;
            }
        }
        session.history = std::move(loaded->messages);
        auto opened = harness::session::JsonlSessionStore::open_existing(request.resume_path);
        if (!opened) {
            return std::unexpected(opened.error());
        }
        session.store = std::move(*opened);
        return session;
    }

    harness::session::SessionMetadata metadata{
        std::move(request.session_id),
        std::move(request.created_at),
        session.workspace,
        std::move(request.provider),
        std::move(request.model),
    };
    auto created = harness::session::JsonlSessionStore::create_new(request.session_path, metadata);
    if (!created) {
        return std::unexpected(created.error());
    }
    session.store = std::move(*created);
    return session;
}

} // namespace cch::coding_agent::runtime
