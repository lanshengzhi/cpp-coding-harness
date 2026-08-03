#pragma once

#include <cch/ai/CredentialStore.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace cch::coding_agent {

/// File-backed CredentialStore for pi's shared `auth.json` contract.
///
/// The implementation owns whole-file proper-lockfile-compatible locking,
/// owner-only permissions, a last-valid in-memory snapshot, and the private
/// lossless serializer. Agent Config Directory path derivation remains in the
/// coding-agent layer; callers pass the concrete `<agentDir>/auth.json` path.
class AuthStorage final : public ai::CredentialStore {
public:
    explicit AuthStorage(std::filesystem::path auth_path);
    AuthStorage(AuthStorage&&) = delete;
    AuthStorage& operator=(AuthStorage&&) = delete;
    ~AuthStorage() override;

    AuthStorage(const AuthStorage&) = delete;
    AuthStorage& operator=(const AuthStorage&) = delete;

    /// Re-read the file under its whole-file lock. Invalid or unreadable
    /// content preserves the previous valid snapshot.
    void reload() noexcept;

    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<ai::Credential>>> read(
        std::string provider_id) override;
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::vector<ai::CredentialInfo>>> list() override;
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<ai::Credential>>> modify(
        std::string provider_id,
        ai::CredentialModifyHook modifier) override;
    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> remove(
        std::string provider_id) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::coding_agent
