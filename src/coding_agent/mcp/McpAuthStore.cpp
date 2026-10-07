// pi's MCP OAuth credential store (`mcp-auth.json`), spec #882 ticket #884.
// pi source at `7c10bd43` (v1.0.4): `packages/coding-agent/src/extensions/mcp/
// oauth.ts` (`McpOAuthCredentialStore`, `storeKeys`) and
// `packages/mcp/src/oauth/provider.ts` (`McpOAuthState`). The file is
// `JSON.stringify(states, null, 2)` with a trailing newline. The legacy
// bare-URL key is taken over on the first load; the `#875` `auth.json`
// `mcp__<server>` record is migrated in. Private to `cch_coding_agent`.

#include "coding_agent/mcp/McpAuthStore.hpp"

#include "coding_agent/PrettyJson.hpp"
#include "coding_agent/mcp/McpNamespace.hpp"

#include "support/Json.hpp"

#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace cch::coding_agent::mcp {
namespace {

using JsonObject = support::JsonValue::object_t;

[[nodiscard]] support::Error store_error(std::string message, std::string detail = {}) {
    return support::make_error(support::ErrorCode::Validation, std::move(message), std::move(detail));
}

/// The file's text, or `std::nullopt` when it does not exist. A file that
/// exists but cannot be read is an error.
[[nodiscard]] support::Expected<std::optional<std::string>> read_text_file(const std::filesystem::path& path) {
    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error) || exists_error) {
        return std::optional<std::string>{};
    }
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return std::unexpected(store_error("could not read " + path.string()));
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        return std::unexpected(store_error("could not read " + path.string()));
    }
    return std::optional<std::string>{contents.str()};
}

/// The file's JSON object, or an empty object when the file is missing. A file
/// that does not hold a JSON object is an explicit error — a credential store
/// is never silently replaced.
[[nodiscard]] support::Expected<JsonObject> read_states(const std::filesystem::path& path) {
    auto text = read_text_file(path);
    if (!text) {
        return std::unexpected(std::move(text.error()));
    }
    if (!text->has_value() || text->value().find_first_not_of(" \t\r\n") == std::string::npos) {
        return JsonObject{};
    }
    auto parsed = support::read_json(text->value());
    if (!parsed) {
        return std::unexpected(store_error(path.string() + ": contains invalid JSON"));
    }
    const auto* object = parsed->get_if<JsonObject>();
    if (object == nullptr) {
        return std::unexpected(store_error(path.string() + ": expected a JSON object"));
    }
    return *object;
}

/// Write `states` as `JSON.stringify(states, null, 2)` plus a trailing newline,
/// through a temporary file and a rename so a reader never sees a torn file.
/// The file is owner-only, like `auth.json`.
[[nodiscard]] support::ExpectedVoid write_states(const std::filesystem::path& path, const JsonObject& states) {
    auto serialized =
            coding_agent::detail::serialize_pretty_json(support::JsonValue{states}, /* trailing_newline */ true);
    if (!serialized) {
        return std::unexpected(std::move(serialized.error()));
    }
    std::error_code directory_error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), directory_error);
        if (directory_error) {
            return std::unexpected(
                    store_error("could not create " + path.parent_path().string(), directory_error.message()));
        }
    }
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return std::unexpected(store_error("could not open " + temporary.string() + " for writing"));
        }
        output << *serialized;
        if (!output) {
            return std::unexpected(store_error("could not write " + temporary.string()));
        }
    }
    std::error_code permission_error;
    std::filesystem::permissions(temporary,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace,
            permission_error);
    std::error_code rename_error;
    std::filesystem::rename(temporary, path, rename_error);
    if (rename_error) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        return std::unexpected(store_error("could not write " + path.string(), rename_error.message()));
    }
    return {};
}

[[nodiscard]] std::optional<std::string_view> string_member(const JsonObject& object, std::string_view key) {
    const auto found = object.find(std::string{key});
    if (found == object.end() || !found->second.holds<std::string>()) {
        return std::nullopt;
    }
    return std::string_view{found->second.get_string()};
}

[[nodiscard]] std::optional<McpOAuthTokens> tokens_from(const JsonObject& object) {
    const auto access = string_member(object, "access_token");
    if (!access) {
        return std::nullopt;
    }
    McpOAuthTokens tokens;
    tokens.access_token = std::string{*access};
    tokens.token_type = std::string{string_member(object, "token_type").value_or("Bearer")};
    if (const auto refresh = string_member(object, "refresh_token"); refresh && !refresh->empty()) {
        tokens.refresh_token = std::string{*refresh};
    }
    if (const auto scope = string_member(object, "scope"); scope && !scope->empty()) {
        tokens.scope = std::string{*scope};
    }
    return tokens;
}

/// One stored state read from JSON. `nullopt` when the value is not an object.
[[nodiscard]] std::optional<McpOAuthState> state_from(const support::JsonValue& value) {
    const auto* object = value.get_if<JsonObject>();
    if (object == nullptr) {
        return std::nullopt;
    }
    McpOAuthState state;
    state.server_url = std::string{string_member(*object, "serverUrl").value_or("")};
    if (const auto tokens = object->find("tokens");
            tokens != object->end() && tokens->second.get_if<JsonObject>() != nullptr) {
        state.tokens = tokens_from(*tokens->second.get_if<JsonObject>());
    }
    if (const auto client = object->find("clientInformation");
            client != object->end() && client->second.get_if<JsonObject>() != nullptr) {
        const auto& client_object = *client->second.get_if<JsonObject>();
        if (const auto id = string_member(client_object, "client_id")) {
            McpOAuthClientInformation information;
            information.client_id = std::string{*id};
            if (const auto secret = string_member(client_object, "client_secret"); secret && !secret->empty()) {
                information.client_secret = std::string{*secret};
            }
            if (const auto uris = client_object.find("redirect_uris");
                    uris != client_object.end() && uris->second.get_if<support::JsonValue::array_t>() != nullptr) {
                for (const auto& uri : *uris->second.get_if<support::JsonValue::array_t>()) {
                    if (const auto* text = uri.get_if<std::string>(); text != nullptr && !text->empty()) {
                        information.redirect_uris.push_back(*text);
                    }
                }
            }
            state.client_information = std::move(information);
        }
    }
    if (const auto expires = object->find("tokensExpireAt");
            expires != object->end() && expires->second.get_if<double>() != nullptr) {
        state.tokens_expire_at = static_cast<std::int64_t>(*expires->second.get_if<double>());
    }
    if (const auto verifier = string_member(*object, "codeVerifier"); verifier && !verifier->empty()) {
        state.code_verifier = std::string{*verifier};
    }
    if (const auto oauth_state = string_member(*object, "oauthState"); oauth_state && !oauth_state->empty()) {
        state.oauth_state = std::string{*oauth_state};
    }
    return state;
}

[[nodiscard]] support::JsonValue state_to_json(const McpOAuthState& state) {
    JsonObject object;
    object.emplace("serverUrl", support::JsonValue{state.server_url});
    if (state.client_information) {
        JsonObject information;
        information.emplace("client_id", support::JsonValue{state.client_information->client_id});
        if (state.client_information->client_secret) {
            information.emplace("client_secret", support::JsonValue{*state.client_information->client_secret});
        }
        if (!state.client_information->redirect_uris.empty()) {
            support::JsonValue::array_t uris;
            uris.reserve(state.client_information->redirect_uris.size());
            for (const auto& uri : state.client_information->redirect_uris) {
                uris.emplace_back(uri);
            }
            information.emplace("redirect_uris", support::JsonValue{std::move(uris)});
        }
        object.emplace("clientInformation", support::JsonValue{std::move(information)});
    }
    if (state.tokens) {
        JsonObject tokens;
        tokens.emplace("access_token", support::JsonValue{state.tokens->access_token});
        tokens.emplace("token_type", support::JsonValue{state.tokens->token_type});
        if (state.tokens->refresh_token) {
            tokens.emplace("refresh_token", support::JsonValue{*state.tokens->refresh_token});
        }
        if (state.tokens->scope) {
            tokens.emplace("scope", support::JsonValue{*state.tokens->scope});
        }
        object.emplace("tokens", support::JsonValue{std::move(tokens)});
    }
    if (state.tokens_expire_at) {
        object.emplace("tokensExpireAt", support::JsonValue{static_cast<double>(*state.tokens_expire_at)});
    }
    if (state.code_verifier) {
        object.emplace("codeVerifier", support::JsonValue{*state.code_verifier});
    }
    if (state.oauth_state) {
        object.emplace("oauthState", support::JsonValue{*state.oauth_state});
    }
    return support::JsonValue{std::move(object)};
}

} // namespace

std::filesystem::path McpAuthStore::default_path(const std::filesystem::path& agent_dir) {
    return agent_dir / "mcp-auth.json";
}

std::string McpAuthStore::store_key(std::string_view name, std::string_view server_url) {
    return detail::mcp_namespace(name) + "|" + std::string{server_url};
}

McpAuthStore::McpAuthStore(std::filesystem::path file) : file_(std::move(file)) {}

support::Expected<std::optional<McpOAuthState>> McpAuthStore::load(std::string_view name, std::string_view server_url) {
    auto states = read_states(file_);
    if (!states) {
        return std::unexpected(std::move(states.error()));
    }
    const std::string key = store_key(name, server_url);
    const std::string legacy = std::string{server_url};
    if (const auto found = states->find(key); found != states->end()) {
        return state_from(found->second);
    }
    // "The first server to load legacy state takes it over; others with the
    // same URL sign in again." (pi `forServer().load`)
    if (const auto found = states->find(legacy); found != states->end()) {
        const auto state = state_from(found->second);
        states->erase(found);
        if (state) {
            states->emplace(key, state_to_json(*state));
        }
        if (auto written = write_states(file_, *states); !written) {
            return std::unexpected(std::move(written.error()));
        }
        return state;
    }
    return std::optional<McpOAuthState>{};
}

support::ExpectedVoid McpAuthStore::save(
        std::string_view name, std::string_view server_url, const McpOAuthState& state) {
    auto states = read_states(file_);
    if (!states) {
        return std::unexpected(std::move(states.error()));
    }
    states->insert_or_assign(store_key(name, server_url), state_to_json(state));
    return write_states(file_, *states);
}

support::Expected<std::optional<McpOAuthTokens>> McpAuthStore::tokens(
        std::string_view name, std::string_view server_url) {
    auto states = read_states(file_);
    if (!states) {
        return std::unexpected(std::move(states.error()));
    }
    // The namespaced key wins; the legacy key is read but not taken over.
    const auto lookup = [&states](const std::string& candidate) -> std::optional<McpOAuthTokens> {
        const auto found = states->find(candidate);
        if (found == states->end()) {
            return std::nullopt;
        }
        const auto* object = found->second.get_if<JsonObject>();
        if (object == nullptr) {
            return std::nullopt;
        }
        const auto tokens_entry = object->find("tokens");
        if (tokens_entry != object->end() && tokens_entry->second.get_if<JsonObject>() != nullptr) {
            return tokens_from(*tokens_entry->second.get_if<JsonObject>());
        }
        return std::nullopt;
    };
    if (auto found = lookup(store_key(name, server_url))) {
        return found;
    }
    if (auto found = lookup(std::string{server_url})) {
        return found;
    }
    return std::optional<McpOAuthTokens>{};
}

support::Expected<bool> McpAuthStore::remove(std::string_view name, std::string_view server_url) {
    auto states = read_states(file_);
    if (!states) {
        return std::unexpected(std::move(states.error()));
    }
    const std::string key = store_key(name, server_url);
    const std::string legacy = std::string{server_url};
    bool removed = false;
    if (states->erase(key) > 0) {
        removed = true;
    } else if (states->erase(legacy) > 0) {
        removed = true;
    }
    if (!removed) {
        return false;
    }
    if (auto written = write_states(file_, *states); !written) {
        return std::unexpected(std::move(written.error()));
    }
    return true;
}

support::Expected<bool> McpAuthStore::migrate_from_auth_json(
        const std::filesystem::path& auth_json, std::string_view name, std::string_view server_url) {
    auto records = read_states(auth_json);
    if (!records) {
        return std::unexpected(std::move(records.error()));
    }
    const std::string provider_id = detail::mcp_namespace(name);
    const auto found = records->find(provider_id);
    if (found == records->end()) {
        return false;
    }
    const auto* record = found->second.get_if<JsonObject>();
    if (record == nullptr) {
        return false;
    }
    if (string_member(*record, "type").value_or("") != "oauth") {
        return false;
    }
    const auto access = string_member(*record, "access");
    if (!access || access->empty()) {
        return false;
    }
    McpOAuthState state;
    state.server_url = std::string{server_url};
    McpOAuthTokens tokens;
    tokens.access_token = std::string{*access};
    tokens.token_type = "Bearer";
    if (const auto refresh = string_member(*record, "refresh"); refresh && !refresh->empty()) {
        tokens.refresh_token = std::string{*refresh};
    }
    state.tokens = std::move(tokens);
    if (const auto expires = record->find("expires");
            expires != record->end() && expires->second.get_if<double>() != nullptr) {
        state.tokens_expire_at = static_cast<std::int64_t>(*expires->second.get_if<double>());
    }
    // Do not overwrite a state this store already holds (the migration is
    // one-way and idempotent).
    const std::string key = store_key(name, server_url);
    {
        auto states = read_states(file_);
        if (!states) {
            return std::unexpected(std::move(states.error()));
        }
        if (states->contains(key) || states->contains(std::string{server_url})) {
            return false;
        }
        states->emplace(key, state_to_json(state));
        if (auto written = write_states(file_, *states); !written) {
            return std::unexpected(std::move(written.error()));
        }
    }
    // Remove the migrated record so the secret is not stored in two files.
    records->erase(found);
    auto serialized =
            coding_agent::detail::serialize_pretty_json(support::JsonValue{*records}, /* trailing_newline */ true);
    if (!serialized) {
        return std::unexpected(std::move(serialized.error()));
    }
    std::filesystem::path temporary = auth_json;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return std::unexpected(store_error("could not open " + temporary.string() + " for writing"));
        }
        output << *serialized;
        if (!output) {
            return std::unexpected(store_error("could not write " + temporary.string()));
        }
    }
    std::error_code rename_error;
    std::filesystem::rename(temporary, auth_json, rename_error);
    if (rename_error) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        return std::unexpected(store_error("could not write " + auth_json.string(), rename_error.message()));
    }
    return true;
}

} // namespace cch::coding_agent::mcp
