#include "SessionLifecycle.hpp"

#include "../../../include/cch/harness/session/JsonlSessionStore.hpp"

#include <memory>
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

util::Expected<PreparedResumeTarget> prepare_resume_target(
    std::filesystem::path resume_path,
    std::filesystem::path explicit_workspace,
    bool workspace_explicit) {
    auto resumed = harness::session::resume_session(resume_path);
    if (!resumed) {
        return std::unexpected(resumed.error());
    }

    std::filesystem::path workspace = explicit_workspace;
    if (!resumed->metadata.workspace.empty()) {
        if (workspace_explicit && !same_workspace(workspace, resumed->metadata.workspace)) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Session,
                "resume workspace does not match session metadata",
                "resume workspace does not match session metadata; omit --workspace to use " +
                    resumed->metadata.workspace.string() + " or start a new session"));
        }
        if (!workspace_explicit) {
            workspace = resumed->metadata.workspace;
        }
    }

    return PreparedResumeTarget{
        .resume_path = std::move(resume_path),
        .workspace = std::move(workspace),
        .resume = std::move(*resumed),
    };
}

util::Expected<OpenSession> publish_new_session(
    std::filesystem::path session_path,
    std::filesystem::path workspace,
    harness::session::SessionMetadata metadata) {
    OpenSession session;
    session.workspace = std::move(workspace);
    session.metadata = std::move(metadata);

    auto created = harness::session::JsonlSessionStore::create_new(session_path, session.metadata);
    if (!created) {
        return std::unexpected(created.error());
    }
    session.store = std::make_unique<harness::session::JsonlSessionStore>(std::move(*created));
    return session;
}

util::Expected<OpenSession> publish_resume_session(
    const PreparedResumeTarget& target) {
    OpenSession session;
    session.workspace = target.workspace;
    session.metadata = target.resume.metadata;
    session.history = target.resume.history;
    session.context_model = target.resume.model;
    session.context_thinking_level = target.resume.thinking_level;
    session.topology = target.resume.topology;
    session.stored_provider = session.metadata.provider;
    session.stored_model = session.metadata.model;

    auto opened = harness::session::JsonlSessionStore::open_existing(target.resume_path);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    session.store = std::make_unique<harness::session::JsonlSessionStore>(std::move(*opened));
    return session;
}

util::Expected<OpenSession> open_session(SessionOpenRequest request) {
    if (!request.resume_path.empty()) {
        auto prepared = prepare_resume_target(
            std::move(request.resume_path),
            std::move(request.workspace),
            request.workspace_explicit);
        if (!prepared) {
            return std::unexpected(prepared.error());
        }
        return publish_resume_session(*prepared);
    }

    harness::session::SessionMetadata metadata{
        std::move(request.session_id),
        std::move(request.created_at),
        request.workspace,
        std::move(request.provider),
        std::move(request.model),
    };
    return publish_new_session(
        std::move(request.session_path),
        std::move(request.workspace),
        std::move(metadata));
}

} // namespace cch::coding_agent::runtime
