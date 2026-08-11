#include <cch/coding_agent/AuthStorage.hpp>

#include <cch/util/JsonValue.hpp>
#include "util/ExpectedMacros.hpp"
#include "util/Json.hpp"

#include <boost/asio/error.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace cch::coding_agent {
namespace {

using JsonObject = util::JsonValue::object_t;

constexpr auto kSyncLockPollInterval = std::chrono::milliseconds{20};
constexpr auto kLockStaleAfter = std::chrono::seconds{30};
constexpr auto kLockHeartbeatInterval = std::chrono::seconds{15};
constexpr auto kAsyncLockRetryCount = 10;

[[nodiscard]] std::chrono::milliseconds async_lock_retry_delay(int retry_index) {
    auto delay = std::chrono::milliseconds{100};
    for (int index = 0; index < retry_index; ++index) {
        delay = std::min(delay * 2, std::chrono::milliseconds{10000});
    }
    thread_local std::mt19937 generator{std::random_device{}()};
    std::uniform_real_distribution<double> jitter{1.0, 2.0};
    const auto randomized = std::chrono::milliseconds{
        static_cast<std::chrono::milliseconds::rep>(static_cast<double>(delay.count()) * jitter(generator))};
    return std::min(randomized, std::chrono::milliseconds{10000});
}

[[nodiscard]] util::Error storage_error(
    std::string message,
    const std::filesystem::path& path,
    std::string detail = {}) {
    if (detail.empty()) {
        detail = message;
    }
    return util::make_error(util::ErrorCode::Unknown, std::move(message), std::move(detail), path.string());
}

[[nodiscard]] util::Expected<JsonObject> parse_auth_data(
    std::string_view content,
    const std::filesystem::path& path) {
    if (content.empty()) {
        return JsonObject{};
    }
    auto parsed = util::read_json(content);
    if (!parsed) {
        return std::unexpected(util::make_error(
            util::ErrorCode::JsonParse,
            "failed to parse auth file",
            "auth file contains invalid JSON",
            path.string()));
    }
    const auto* object = parsed->get_if<JsonObject>();
    if (object == nullptr) {
        return std::unexpected(util::make_error(
            util::ErrorCode::JsonParse,
            "auth file is not a JSON object",
            "auth file root must be an object",
            path.string()));
    }
    return *object;
}

[[nodiscard]] util::ExpectedVoid append_pretty_json(
    const util::JsonValue& value,
    std::string& output,
    std::size_t indentation) {
    if (const auto* object = value.get_if<JsonObject>()) {
        output.push_back('{');
        if (!object->empty()) {
            output.push_back('\n');
            auto current = object->begin();
            while (current != object->end()) {
                output.append(indentation + 2, ' ');
                auto key = util::write_json(util::JsonValue{current->first});
                if (!key) {
                    return std::unexpected(key.error());
                }
                output.append(*key);
                output.append(": ");
                if (auto appended = append_pretty_json(current->second, output, indentation + 2); !appended) {
                    return appended;
                }
                ++current;
                if (current != object->end()) {
                    output.push_back(',');
                }
                output.push_back('\n');
            }
            output.append(indentation, ' ');
        }
        output.push_back('}');
        return {};
    }

    if (const auto* array = value.get_if<util::JsonValue::array_t>()) {
        output.push_back('[');
        if (!array->empty()) {
            output.push_back('\n');
            for (std::size_t index = 0; index < array->size(); ++index) {
                output.append(indentation + 2, ' ');
                if (auto appended = append_pretty_json((*array)[index], output, indentation + 2); !appended) {
                    return appended;
                }
                if (index + 1 != array->size()) {
                    output.push_back(',');
                }
                output.push_back('\n');
            }
            output.append(indentation, ' ');
        }
        output.push_back(']');
        return {};
    }

    auto serialized = util::write_json(value);
    if (!serialized) {
        return std::unexpected(serialized.error());
    }
    output.append(*serialized);
    return {};
}

[[nodiscard]] util::Expected<std::string> serialize_auth_data(const JsonObject& data) {
    std::string output;
    if (auto appended = append_pretty_json(util::JsonValue{data}, output, 0); !appended) {
        return std::unexpected(appended.error());
    }
    return output;
}

[[nodiscard]] const std::string* string_field(const JsonObject& object, std::string_view name) {
    const auto found = object.find(std::string{name});
    return found == object.end() ? nullptr : found->second.get_if<std::string>();
}

[[nodiscard]] std::optional<ai::Credential> credential_from_json(const util::JsonValue& value) {
    const auto* object = value.get_if<JsonObject>();
    if (object == nullptr) {
        return std::nullopt;
    }
    const auto* type = string_field(*object, "type");
    if (type == nullptr) {
        return std::nullopt;
    }

    if (*type == "api_key") {
        ai::ApiKeyCredential credential;
        if (const auto* key = string_field(*object, "key")) {
            credential.key = *key;
        }
        if (const auto env_it = object->find("env"); env_it != object->end()) {
            if (const auto* env = env_it->second.get_if<JsonObject>()) {
                for (const auto& [name, env_value] : *env) {
                    if (const auto* text = env_value.get_if<std::string>()) {
                        credential.env.emplace(name, *text);
                    }
                }
            }
        }
        return ai::Credential{std::move(credential)};
    }

    if (*type == "oauth") {
        const auto* refresh = string_field(*object, "refresh");
        const auto* access = string_field(*object, "access");
        const auto expires_it = object->find("expires");
        if (refresh == nullptr || access == nullptr || expires_it == object->end()) {
            return std::nullopt;
        }
        const auto* expires_number = expires_it->second.get_if<double>();
        const bool invalid_expiry =
            expires_number == nullptr ||
            !std::isfinite(*expires_number) ||
            std::trunc(*expires_number) != *expires_number ||
            *expires_number < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
            *expires_number > static_cast<double>(std::numeric_limits<std::int64_t>::max());
        if (invalid_expiry) {
            return std::nullopt;
        }
        ai::OAuthCredential credential{
            .refresh = *refresh,
            .access = *access,
            .expires = static_cast<std::int64_t>(*expires_number),
            .account_id = std::nullopt,
        };
        if (const auto* account_id = string_field(*object, "accountId")) {
            credential.account_id = *account_id;
        }
        return ai::Credential{std::move(credential)};
    }

    return std::nullopt;
}

[[nodiscard]] JsonObject credential_to_json(
    const ai::Credential& credential,
    const util::JsonValue* current) {
    JsonObject object;
    if (const auto* current_object = current == nullptr ? nullptr : current->get_if<JsonObject>()) {
        const auto* current_type = string_field(*current_object, "type");
        const bool same_api_key_type =
            std::holds_alternative<ai::ApiKeyCredential>(credential) &&
            current_type != nullptr &&
            *current_type == "api_key";
        const bool same_oauth_type =
            std::holds_alternative<ai::OAuthCredential>(credential) &&
            current_type != nullptr &&
            *current_type == "oauth";
        const bool same_type = same_api_key_type || same_oauth_type;
        if (same_type) {
            object = *current_object;
        }
    }

    if (const auto* api_key = std::get_if<ai::ApiKeyCredential>(&credential)) {
        object["type"] = "api_key";
        if (api_key->key) {
            object["key"] = *api_key->key;
        } else {
            object.erase("key");
        }
        if (api_key->env.empty()) {
            object.erase("env");
        } else {
            JsonObject env;
            for (const auto& [name, value] : api_key->env) {
                env.emplace(name, util::JsonValue{value});
            }
            object["env"] = util::JsonValue{std::move(env)};
        }
        return object;
    }

    const auto& oauth = std::get<ai::OAuthCredential>(credential);
    object["type"] = "oauth";
    object["refresh"] = oauth.refresh;
    object["access"] = oauth.access;
    object["expires"] = static_cast<double>(oauth.expires);
    if (oauth.account_id) {
        object["accountId"] = *oauth.account_id;
    } else {
        object.erase("accountId");
    }
    return object;
}

[[nodiscard]] util::Expected<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return std::unexpected(storage_error("failed to read auth file", path));
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        return std::unexpected(storage_error("failed to read auth file", path));
    }
    return contents.str();
}

[[nodiscard]] util::ExpectedVoid set_private_permissions(
    const std::filesystem::path& path,
    std::filesystem::perms permissions,
    std::string_view kind) {
    std::error_code error;
    std::filesystem::permissions(path, permissions, std::filesystem::perm_options::replace, error);
    if (error) {
        return std::unexpected(storage_error(
            "failed to set private " + std::string{kind} + " permissions",
            path,
            error.message()));
    }
    return {};
}

[[nodiscard]] util::ExpectedVoid ensure_storage_directory(const std::filesystem::path& path) {
    if (path.empty()) {
        return std::unexpected(storage_error("auth file path is empty", path));
    }
    const auto parent = path.parent_path();
    if (parent.empty()) {
        return std::unexpected(storage_error("auth file parent path is empty", path));
    }

    std::error_code error;
    const bool created_parent = std::filesystem::create_directories(parent, error);
    if (error) {
        return std::unexpected(storage_error("failed to create auth directory", parent, error.message()));
    }
    if (created_parent) {
        return set_private_permissions(parent, std::filesystem::perms::owner_all, "directory");
    }
    return {};
}

[[nodiscard]] util::ExpectedVoid ensure_auth_file(const std::filesystem::path& path) {
    std::error_code error;
    const bool existed = std::filesystem::exists(path, error);
    if (error) {
        return std::unexpected(storage_error("failed to inspect auth file", path, error.message()));
    }
    if (existed) {
        return {};
    }

    // C++ callers hold auth.json.lock before this check/write. A pi process may
    // race only in its pre-lock creation step; truncating the same two bytes is
    // idempotent and cannot append a second JSON document.
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return std::unexpected(storage_error("failed to create auth file", path));
    }
    output << "{}";
    if (!output.good()) {
        return std::unexpected(storage_error("failed to initialize auth file", path));
    }
    output.close();
    return set_private_permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        "file");
}

[[nodiscard]] util::ExpectedVoid write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return std::unexpected(storage_error("failed to write auth file", path));
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output.good()) {
        return std::unexpected(storage_error("failed to write auth file", path));
    }
    output.close();
    return set_private_permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        "file");
}

[[nodiscard]] util::ExpectedVoid ensure_storage_path(const std::filesystem::path& path) {
    if (auto directory = ensure_storage_directory(path); !directory) {
        return directory;
    }
    return ensure_auth_file(path);
}

[[nodiscard]] util::Expected<JsonObject> read_current_data(const std::filesystem::path& path) {
    auto content = read_file(path);
    if (!content) {
        return std::unexpected(content.error());
    }
    return parse_auth_data(*content, path);
}

class LockLease {
public:
    LockLease(
        std::filesystem::path lock_path,
        std::filesystem::file_time_type expected_mtime)
        : lock_path_(std::move(lock_path)), expected_mtime_(expected_mtime) {
        heartbeat_ = std::jthread([this](std::stop_token stop_token) {
            std::stop_callback stop_callback(stop_token, [this] { heartbeat_wakeup_.notify_all(); });
            std::unique_lock lock(heartbeat_mutex_);
            while (!stop_token.stop_requested()) {
                if (heartbeat_wakeup_.wait_for(
                        lock,
                        kLockHeartbeatInterval,
                        [&] { return stop_token.stop_requested(); })) {
                    break;
                }
                if (!refresh_heartbeat()) {
                    break;
                }
            }
        });
    }

    LockLease(LockLease&&) = delete;
    LockLease& operator=(LockLease&&) = delete;
    ~LockLease() {
        heartbeat_.request_stop();
        heartbeat_wakeup_.notify_all();
        if (heartbeat_.joinable()) {
            heartbeat_.join();
        }
        if (compromised()) {
            return;
        }

        std::scoped_lock lock(heartbeat_mutex_);
        std::error_code error;
        const auto actual_mtime = std::filesystem::last_write_time(lock_path_, error);
        if (error || actual_mtime != expected_mtime_) {
            compromised_.store(true, std::memory_order_release);
            return;
        }
        std::filesystem::remove(lock_path_, error);
        if (error) {
            compromised_.store(true, std::memory_order_release);
        }
    }

    LockLease(const LockLease&) = delete;
    LockLease& operator=(const LockLease&) = delete;

    [[nodiscard]] bool compromised() const noexcept {
        return compromised_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool valid() {
        if (compromised()) {
            return false;
        }
        std::scoped_lock lock(heartbeat_mutex_);
        std::error_code error;
        const auto actual_mtime = std::filesystem::last_write_time(lock_path_, error);
        if (error || actual_mtime != expected_mtime_) {
            compromised_.store(true, std::memory_order_release);
            return false;
        }
        return true;
    }

private:
    [[nodiscard]] bool refresh_heartbeat() {
        std::error_code error;
        const auto actual_mtime = std::filesystem::last_write_time(lock_path_, error);
        if (error || actual_mtime != expected_mtime_) {
            compromised_.store(true, std::memory_order_release);
            return false;
        }

        std::filesystem::last_write_time(
            lock_path_,
            std::filesystem::file_time_type::clock::now(),
            error);
        if (error) {
            compromised_.store(true, std::memory_order_release);
            return false;
        }
        expected_mtime_ = std::filesystem::last_write_time(lock_path_, error);
        if (error) {
            compromised_.store(true, std::memory_order_release);
            return false;
        }
        return true;
    }

    std::filesystem::path lock_path_;
    std::filesystem::file_time_type expected_mtime_;
    std::mutex heartbeat_mutex_;
    std::condition_variable heartbeat_wakeup_;
    std::jthread heartbeat_;
    std::atomic_bool compromised_{false};
};

[[nodiscard]] std::optional<std::filesystem::file_time_type> stale_lock_mtime(
    const std::filesystem::path& lock_path) {
    std::error_code error;
    const auto modified = std::filesystem::last_write_time(lock_path, error);
    if (error || std::filesystem::file_time_type::clock::now() - modified <= kLockStaleAfter) {
        return std::nullopt;
    }
    return modified;
}

[[nodiscard]] util::Expected<std::unique_ptr<LockLease>> try_acquire_lock(
    const std::filesystem::path& auth_path) {
    const auto lock_path = std::filesystem::path{auth_path.string() + ".lock"};
    std::error_code error;
    if (std::filesystem::create_directory(lock_path, error)) {
        std::filesystem::last_write_time(
            lock_path,
            std::filesystem::file_time_type::clock::now(),
            error);
        if (error) {
            std::error_code ignored;
            std::filesystem::remove(lock_path, ignored);
            return std::unexpected(storage_error("failed to initialize auth file lock", lock_path, error.message()));
        }
        const auto actual_mtime = std::filesystem::last_write_time(lock_path, error);
        if (error) {
            std::error_code ignored;
            std::filesystem::remove(lock_path, ignored);
            return std::unexpected(storage_error("failed to inspect auth file lock", lock_path, error.message()));
        }
        return std::make_unique<LockLease>(lock_path, actual_mtime);
    }
    if (error && error != std::errc::file_exists) {
        return std::unexpected(storage_error("failed to acquire auth file lock", lock_path, error.message()));
    }
    if (const auto stale_mtime = stale_lock_mtime(lock_path)) {
        const auto current_mtime = std::filesystem::last_write_time(lock_path, error);
        if (error) {
            return std::unexpected(storage_error("failed to inspect stale auth file lock", lock_path, error.message()));
        }
        if (current_mtime == *stale_mtime) {
            std::filesystem::remove_all(lock_path, error);
            if (error) {
                return std::unexpected(storage_error(
                    "failed to remove stale auth file lock",
                    lock_path,
                    error.message()));
            }
        }
    }
    return std::unique_ptr<LockLease>{};
}

[[nodiscard]] util::Expected<std::unique_ptr<LockLease>> acquire_lock_sync(
    const std::filesystem::path& auth_path) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        auto acquired = try_acquire_lock(auth_path);
        if (!acquired || *acquired) {
            return acquired;
        }
        std::this_thread::sleep_for(kSyncLockPollInterval);
    }
    return std::unexpected(storage_error(
        "auth file is locked",
        std::filesystem::path{auth_path.string() + ".lock"},
        "timed out acquiring the auth file lock"));
}

[[nodiscard]] boost::asio::awaitable<util::Expected<std::unique_ptr<LockLease>>> acquire_lock_async(
    const std::filesystem::path& auth_path) {
    auto executor = co_await boost::asio::this_coro::executor;
    for (int attempt = 0; attempt <= kAsyncLockRetryCount; ++attempt) {
        CCH_TRY(acquired, try_acquire_lock(auth_path));
        if (acquired) {
            co_return std::move(acquired);
        }
        if (attempt == kAsyncLockRetryCount) {
            break;
        }

        boost::asio::steady_timer timer(executor, async_lock_retry_delay(attempt));
        boost::system::error_code timer_error;
        co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, timer_error));
        if (timer_error == boost::asio::error::operation_aborted) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Cancelled,
                "auth file lock acquisition was cancelled"));
        }
        if (timer_error) {
            co_return std::unexpected(storage_error(
                "failed while waiting for auth file lock",
                auth_path,
                timer_error.message()));
        }
    }
    co_return std::unexpected(storage_error(
        "auth file is locked",
        std::filesystem::path{auth_path.string() + ".lock"},
        "timed out acquiring the auth file lock"));
}

} // namespace

struct AuthStorage::Impl {
    explicit Impl(std::filesystem::path auth_path) : auth_path_(std::move(auth_path)) {}

    void reload() noexcept {
        try {
            if (auto ensured = ensure_storage_path(auth_path_); !ensured) {
                return;
            }
            auto lease = acquire_lock_sync(auth_path_);
            if (!lease) {
                return;
            }
            auto parsed = read_current_data(auth_path_);
            if (!parsed) {
                return;
            }
            set_snapshot(std::move(*parsed));
        } catch (...) {
            // A reload is best-effort by contract: retain the last valid view.
        }
    }

    [[nodiscard]] std::optional<ai::Credential> read(std::string_view provider_id) const {
        std::scoped_lock lock(snapshot_mutex_);
        const auto found = snapshot_.find(std::string{provider_id});
        return found == snapshot_.end() ? std::nullopt : credential_from_json(found->second);
    }

    [[nodiscard]] std::vector<ai::CredentialInfo> list() const {
        std::vector<ai::CredentialInfo> entries;
        std::scoped_lock lock(snapshot_mutex_);
        for (const auto& [provider_id, value] : snapshot_) {
            const auto* object = value.get_if<JsonObject>();
            if (object == nullptr) {
                continue;
            }
            if (const auto* type = string_field(*object, "type")) {
                entries.push_back(ai::CredentialInfo{.provider_id = provider_id, .type = *type});
            }
        }
        return entries;
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<ai::Credential>>> modify(
        std::string provider_id,
        ai::CredentialModifyHook modifier) {
        CCH_TRY_VOID(ensure_storage_path(auth_path_));
        CCH_TRY(lease, co_await acquire_lock_async(auth_path_));
        CCH_TRY(current_data, read_current_data(auth_path_));

        const auto current_it = current_data.find(provider_id);
        auto current = current_it == current_data.end()
            ? std::optional<ai::Credential>{}
            : credential_from_json(current_it->second);

        util::Expected<std::optional<ai::Credential>> next_result;
        try {
            next_result = co_await modifier(current);
        } catch (const boost::system::system_error& exception) {
            if (exception.code() == boost::asio::error::operation_aborted) {
                co_return std::unexpected(util::make_error(
                    util::ErrorCode::Cancelled,
                    "credential modification was cancelled"));
            }
            co_return std::unexpected(storage_error(
                "credential modifier failed",
                auth_path_,
                "credential modifier raised an asynchronous exception"));
        } catch (const std::exception&) {
            co_return std::unexpected(storage_error(
                "credential modifier failed",
                auth_path_,
                "credential modifier raised an exception"));
        } catch (...) {
            co_return std::unexpected(storage_error("credential modifier failed", auth_path_));
        }
        CCH_TRY(next, std::move(next_result));
        if (!next) {
            set_snapshot(std::move(current_data));
            co_return current;
        }
        if (!lease->valid()) {
            co_return std::unexpected(storage_error("auth file lock was compromised", auth_path_));
        }

        const util::JsonValue* current_json = current_it == current_data.end() ? nullptr : &current_it->second;
        current_data[provider_id] = util::JsonValue{credential_to_json(*next, current_json)};
        CCH_TRY(serialized, serialize_auth_data(current_data));
        CCH_TRY_VOID(write_file(auth_path_, serialized));
        if (!lease->valid()) {
            co_return std::unexpected(storage_error("auth file lock was compromised", auth_path_));
        }
        set_snapshot(std::move(current_data));
        co_return std::move(next);
    }

    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> remove(std::string provider_id) {
        CCH_TRY_VOID(ensure_storage_path(auth_path_));
        CCH_TRY(lease, co_await acquire_lock_async(auth_path_));
        CCH_TRY(current_data, read_current_data(auth_path_));

        current_data.erase(provider_id);
        CCH_TRY(serialized, serialize_auth_data(current_data));
        if (!lease->valid()) {
            co_return std::unexpected(storage_error("auth file lock was compromised", auth_path_));
        }
        CCH_TRY_VOID(write_file(auth_path_, serialized));
        if (!lease->valid()) {
            co_return std::unexpected(storage_error("auth file lock was compromised", auth_path_));
        }
        set_snapshot(std::move(current_data));
        co_return util::ExpectedVoid{};
    }

private:
    void set_snapshot(JsonObject snapshot) {
        std::scoped_lock lock(snapshot_mutex_);
        snapshot_ = std::move(snapshot);
    }

    std::filesystem::path auth_path_;
    mutable std::mutex snapshot_mutex_;
    JsonObject snapshot_;
};

AuthStorage::AuthStorage(std::filesystem::path auth_path)
    : impl_(std::make_unique<Impl>(std::move(auth_path))) {
    reload();
}

AuthStorage::~AuthStorage() = default;

void AuthStorage::reload() noexcept {
    impl_->reload();
}

boost::asio::awaitable<util::Expected<std::optional<ai::Credential>>> AuthStorage::read(
    std::string provider_id) {
    co_return impl_->read(provider_id);
}

boost::asio::awaitable<util::Expected<std::vector<ai::CredentialInfo>>> AuthStorage::list() {
    co_return impl_->list();
}

boost::asio::awaitable<util::Expected<std::optional<ai::Credential>>> AuthStorage::modify(
    std::string provider_id,
    ai::CredentialModifyHook modifier) {
    co_return co_await impl_->modify(std::move(provider_id), std::move(modifier));
}

boost::asio::awaitable<util::ExpectedVoid> AuthStorage::remove(std::string provider_id) {
    co_return co_await impl_->remove(std::move(provider_id));
}

} // namespace cch::coding_agent
