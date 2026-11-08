// Ticket #1098 (spec #1044, ADR 0075): Generic Remote exchange and Linux host.
// Acceptance criteria:
// 1. Real nested calls across multi-turn exchange; large console output batched
//    (not one exchange round per output item).
// 2. Abort sends stop and settles result/close without waiting for actual remote
//    worker exit; different transport/capability failures retain error taxonomy.
// 3. Verified through public caller: cch::codemode::RemoteCodemodeSandbox and
//    cch::codemode::serve_codemode_remote.

#include <cch/codemode/Remote.hpp>
#include <cch/codemode/Sandbox.hpp>

#include "support/AsyncResultBridge.hpp"
#include "support/Json.hpp"
#include "support/ReleaseGate.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace cch;

namespace {

template <typename T> [[nodiscard]] std::expected<T, support::Error> run_async(support::AsyncResult<T> operation) {
    boost::asio::io_context io;
    auto future = boost::asio::co_spawn(
            io,
            [operation = std::move(operation)]() mutable -> boost::asio::awaitable<std::expected<T, support::Error>> {
                co_return co_await support::detail::await_async_result(std::move(operation));
            },
            boost::asio::use_future);
    io.run();
    return future.get();
}

/// The registered #1179 remote-exchange observation: the remote cases below
/// compare their exchange shapes against the frozen scenario fields instead
/// of locally spelled expectations.
[[nodiscard]] support::JsonValue read_remote_exchange_artifact() {
    const auto path =
            std::filesystem::path{CCH_SOURCE_DIR} / "fixtures" / "pi-v1.3.0" / "audit-1053" / "remote-exchange.json";
    std::ifstream input(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto parsed = support::read_json(text);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

[[nodiscard]] const support::JsonValue& remote_scenario(const support::JsonValue& artifact, std::string_view name) {
    for (const auto& candidate : artifact.get_object().at("scenarios").get_array()) {
        if (candidate.get_object().at("name").get_string() == name) return candidate;
    }
    REQUIRE(false);
    return artifact;
}

/// Helper that wraps serve_codemode_remote and records message exchanges and replies.
struct RecordingRemote final : public codemode::CodemodeRemote {
    codemode::CodemodeServeOptions options;
    std::mutex mutex;
    std::mutex stop_reply_mutex;
    bool stop_reply_recorded{false};
    std::vector<std::vector<std::string>> exchange_message_kinds;
    std::vector<std::size_t> exchange_sizes;
    std::vector<std::string> reply_kinds;
    std::promise<void> stop_reply_received;
    std::vector<support::AsyncResult<void>> served;

    explicit RecordingRemote(codemode::CodemodeServeOptions opts = {}) : options(std::move(opts)) {}

    [[nodiscard]] support::AsyncResult<void> execute(
            codemode::CodemodeRemoteRequest request, codemode::CodemodeRemoteExchangeHook exchange) override {
        auto exchange_hook = std::make_shared<codemode::CodemodeRemoteExchangeHook>(std::move(exchange));
        auto wrapped_exchange = [this, exchange_hook](std::vector<codemode::CodemodeRemoteMessageVariant> messages)
                -> support::AsyncResult<codemode::CodemodeRemoteReplyVariant> {
            std::vector<std::string> kinds;
            kinds.reserve(messages.size());
            for (const auto& msg : messages) {
                std::visit(
                        [&](const auto& m) {
                            using T = std::decay_t<decltype(m)>;
                            if constexpr (std::is_same_v<T, codemode::CodemodeRemoteCallMessage>) {
                                kinds.push_back("call:" + m.name);
                            } else if constexpr (std::is_same_v<T, codemode::CodemodeRemoteOutputMessage>) {
                                kinds.push_back("output");
                            } else if constexpr (std::is_same_v<T, codemode::CodemodeRemoteDoneMessage>) {
                                kinds.push_back("done");
                            } else if constexpr (std::is_same_v<T, codemode::CodemodeRemoteCrashMessage>) {
                                kinds.push_back("crash");
                            } else if constexpr (std::is_same_v<T, codemode::CodemodeRemoteStartedMessage>) {
                                kinds.push_back("started");
                            } else if constexpr (std::is_same_v<T, codemode::CodemodeRemoteIdleMessage>) {
                                kinds.push_back("idle");
                            } else if constexpr (std::is_same_v<T, codemode::CodemodeRemoteFailureMessage>) {
                                kinds.push_back("failure");
                            }
                        },
                        msg);
            }
            {
                std::lock_guard lock(mutex);
                exchange_sizes.push_back(messages.size());
                exchange_message_kinds.push_back(std::move(kinds));
            }

            return support::detail::make_async_result(
                    [this, exchange_hook, messages = std::move(messages)]() mutable
                            -> boost::asio::awaitable<support::Expected<codemode::CodemodeRemoteReplyVariant>> {
                        auto reply =
                                co_await support::detail::await_async_result((*exchange_hook)(std::move(messages)));
                        if (reply) {
                            std::string kind = "null";
                            const bool is_stop = std::holds_alternative<codemode::CodemodeRemoteStopReply>(*reply);
                            if (std::holds_alternative<codemode::CodemodeRemoteResultReply>(*reply)) {
                                const auto& res = std::get<codemode::CodemodeRemoteResultReply>(*reply);
                                kind = std::format("result:{}", res.ok ? "true" : "false");
                            } else if (is_stop) {
                                kind = "stop";
                            }
                            {
                                std::lock_guard lock(mutex);
                                reply_kinds.push_back(std::move(kind));
                            }
                            if (is_stop) {
                                std::lock_guard lock(stop_reply_mutex);
                                if (!stop_reply_recorded) {
                                    stop_reply_recorded = true;
                                    stop_reply_received.set_value();
                                }
                            }
                        }
                        co_return reply;
                    });
        };

        return codemode::serve_codemode_remote(std::move(request), std::move(wrapped_exchange), options);
    }
};

} // namespace

namespace {

/// A successful answer with every field initialized.
[[nodiscard]] codemode::CodemodeOutcome ok_outcome(std::string value_json) {
    codemode::CodemodeOutcome outcome;
    outcome.ok = true;
    outcome.value_json = std::move(value_json);
    return outcome;
}

} // namespace

namespace {

/// The caller-side records one deterministic Remote run reports. Heap-owned and
/// shared by value with every callback, so a failed assertion that abandons the
/// test frame leaves the still-running remote writing into live storage
/// (CODING_STANDARDS.md §11.10).
struct DeterministicRecorder {
    std::mutex mutex;
    std::vector<codemode::CodemodeHostCall> calls;
    std::vector<std::pair<std::int64_t, std::int64_t>> idles;
    std::string fingerprint;

    /// Waits for `count` calls to reach the caller, or returns false at the bound.
    [[nodiscard]] bool wait_for_calls(std::size_t count) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard lock(mutex);
                if (calls.size() >= count) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return false;
    }

    [[nodiscard]] codemode::CodemodeHostCall call(std::size_t index) {
        std::lock_guard lock(mutex);
        return calls.at(index);
    }

    [[nodiscard]] std::vector<std::pair<std::int64_t, std::int64_t>> idle_list() {
        std::lock_guard lock(mutex);
        return idles;
    }
};

/// One Remote sandbox over a recording host.
[[nodiscard]] std::unique_ptr<codemode::RemoteCodemodeSandbox> make_remote_sandbox() {
    auto remote = std::make_shared<RecordingRemote>();
    codemode::RemoteCodemodeSandboxOptions options;
    options.remote = [remote]() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> { return remote; };
    auto created = codemode::RemoteCodemodeSandbox::create(std::move(options));
    REQUIRE(created.has_value());
    return std::move(*created);
}

/// The deterministic options every case shares: the frozen start, seed, tool
/// surface and deadline, plus the caller's recording callbacks.
[[nodiscard]] codemode::CodemodeRunOptions recording_options(std::shared_ptr<DeterministicRecorder> recorder) {
    codemode::CodemodeRunOptions options;
    options.started_at = 1768471200000;
    options.seed = 1;
    options.tools = {
            {.name = "wait", .js_name = "wait", .description = "Wait for the host."},
            {.name = "add", .js_name = "add", .description = "Add one."},
    };
    options.timeout = std::chrono::seconds{20};
    options.callbacks.on_started = [recorder](std::string_view value) {
        std::lock_guard lock(recorder->mutex);
        recorder->fingerprint = std::string{value};
    };
    options.callbacks.on_call = [recorder](const codemode::CodemodeHostCall& call) {
        std::lock_guard lock(recorder->mutex);
        recorder->calls.push_back(call);
    };
    options.callbacks.on_idle = [recorder](std::int64_t seq, std::int64_t issued) {
        std::lock_guard lock(recorder->mutex);
        recorder->idles.emplace_back(seq, issued);
    };
    return options;
}

} // namespace

TEST_CASE("Remote deterministic execution runs the script remotely and answers through deliver",
        "[codemode][remote][deterministic][issue1099]") {
    // The frozen generic-exchange observation (audit-1053 remote-exchange.json)
    // pins this script's value, output and call record; the deterministic
    // transport below must reproduce them through `deliver`.
    const auto artifact = read_remote_exchange_artifact();
    const auto& frozen = remote_scenario(
            artifact, "a generic remote runs the script and calls the caller's tools across the exchange");
    const auto& frozen_inputs = frozen.get_object().at("inputs").get_object();
    const auto& frozen_expected = frozen.get_object().at("expected").get_object();
    const auto& frozen_call = frozen_expected.at("calls").get_array().at(0).get_object();
    auto sandbox = make_remote_sandbox();
    auto recorder = std::make_shared<DeterministicRecorder>();
    auto started = sandbox->start(frozen_inputs.at("script").get_string(), recording_options(recorder));
    REQUIRE(started.has_value());
    auto run = std::move(*started);

    REQUIRE(recorder->wait_for_calls(1));
    const auto call = recorder->call(0);
    CHECK(call.name == frozen_call.at("name").get_string());
    CHECK_FALSE(run.deliver(1, 1768471201000.0, call.id, ok_outcome("2")).has_value());

    auto result = run_async(run.result());
    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    CHECK(result->value_json == std::to_string(static_cast<std::int64_t>(frozen_expected.at("value").get_number())));
    REQUIRE(result->output.size() == 1);
    CHECK(result->output.front().data ==
            frozen_expected.at("output").get_array().at(0).get_object().at("text").get_string());
    REQUIRE(result->calls.size() == 1);
    CHECK(result->calls.front().name == frozen_call.at("name").get_string());
    CHECK(result->calls.front().status == codemode::CodemodeCallRecord::Status::Ok);
    CHECK_FALSE(recorder->fingerprint.empty());
    CHECK(run.finished());
}

TEST_CASE("Remote deterministic execution applies reversed answers by result number",
        "[codemode][remote][deterministic][issue1099]") {
    // Frozen pi v1.3.0 (`remote-exchange.json`, "a deterministic start over a
    // transport that reorders replies still applies results in seq order"):
    // seq 2 arrives before seq 1 and the script still observes the frozen
    // value, with the frozen idle pairs recorded.
    const auto artifact = read_remote_exchange_artifact();
    const auto& frozen = remote_scenario(artifact,
            "a deterministic start over a transport that reorders replies still applies results in seq order");
    const auto& frozen_expected = frozen.get_object().at("expected").get_object();
    const auto& frozen_value = frozen_expected.at("value").get_array();
    auto sandbox = make_remote_sandbox();
    auto recorder = std::make_shared<DeterministicRecorder>();
    const std::string script = "const order = []; "
                               "await Promise.all([tools.wait(\"a\").then(() => order.push(\"a\")), "
                               "tools.wait(\"b\").then(() => order.push(\"b\"))]); return order;";
    auto started = sandbox->start(script, recording_options(recorder));
    REQUIRE(started.has_value());
    auto run = std::move(*started);

    // Both calls must be outstanding at once, or the transport could not
    // reorder anything.
    REQUIRE(recorder->wait_for_calls(2));
    const auto a = recorder->call(0);
    const auto b = recorder->call(1);
    CHECK(a.id != b.id);

    // Result number 2 arrives first and waits for number 1.
    CHECK_FALSE(run.deliver(2, 1768471202000.0, b.id, ok_outcome("null")).has_value());
    CHECK_FALSE(run.deliver(1, 1768471201000.0, a.id, ok_outcome("null")).has_value());

    auto result = run_async(run.result());
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->error.has_value());
    REQUIRE(result->value_json.has_value());
    auto parsed_value = support::read_json(*result->value_json);
    REQUIRE(parsed_value.has_value());
    REQUIRE(parsed_value->get_array().size() == frozen_value.size());
    for (std::size_t index = 0; index < frozen_value.size(); ++index) {
        CHECK(parsed_value->get_array().at(index).get_string() == frozen_value.at(index).get_string());
    }
    const auto idles = recorder->idle_list();
    REQUIRE(idles.size() == frozen_expected.at("idles").get_array().size());
    for (std::size_t index = 0; index < idles.size(); ++index) {
        const auto& frozen_idle = frozen_expected.at("idles").get_array().at(index).get_array();
        CHECK(idles[index] ==
                std::pair<std::int64_t, std::int64_t>{static_cast<std::int64_t>(frozen_idle.at(0).get_number()),
                        static_cast<std::int64_t>(frozen_idle.at(1).get_number())});
    }
}

TEST_CASE("Remote deterministic execution rejects a reused result number and an unknown call",
        "[codemode][remote][deterministic][issue1099]") {
    // Frozen pi v1.3.0 (`audit-1099`): `deliver` refuses these synchronously, and
    // the run stays open so a valid answer can still complete it.
    auto sandbox = make_remote_sandbox();
    auto recorder = std::make_shared<DeterministicRecorder>();
    auto started = sandbox->start("return await tools.wait(1)", recording_options(recorder));
    REQUIRE(started.has_value());
    auto run = std::move(*started);

    REQUIRE(recorder->wait_for_calls(1));
    const auto call = recorder->call(0);
    const auto unknown = run.deliver(1, 1768471200000.0, 99, ok_outcome("null"));
    REQUIRE(unknown.has_value());
    CHECK(unknown->reason == codemode::CodemodeDeliveryRejection::Reason::UnknownCall);
    CHECK(unknown->message == "Call 99 is not waiting for a result");
    CHECK_FALSE(run.finished());

    CHECK_FALSE(run.deliver(1, 1768471200000.0, call.id, ok_outcome("null")).has_value());
    const auto reused = run.deliver(1, 1768471200000.0, call.id, ok_outcome("null"));
    REQUIRE(reused.has_value());
    CHECK(reused->reason == codemode::CodemodeDeliveryRejection::Reason::ResultNumber);
    CHECK(reused->message == "Result number 1 is not a positive integer used once");

    auto result = run_async(run.result());
    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
}

TEST_CASE("Remote deterministic execution leaves a missing result pending until the caller cancels",
        "[codemode][remote][deterministic][issue1099]") {
    // Frozen pi v1.3.0 (`audit-1099`): with result 1 absent, an accepted
    // result 2 does not finish the run and only the first idle is reported; the
    // caller must cancel, and that settles as aborted rather than as a
    // successful replay.
    auto sandbox = make_remote_sandbox();
    auto recorder = std::make_shared<DeterministicRecorder>();
    std::stop_source stop;
    auto run_options = recording_options(recorder);
    run_options.stop_token = stop.get_token();
    run_options.abort_message = "missing result";
    const std::string script = "const order = []; "
                               "await Promise.all([tools.wait(1), tools.wait(2)]); return order;";
    auto started = sandbox->start(script, std::move(run_options));
    REQUIRE(started.has_value());
    auto run = std::move(*started);

    REQUIRE(recorder->wait_for_calls(2));
    const auto b = recorder->call(1);
    CHECK_FALSE(run.deliver(2, 1768471202000.0, b.id, ok_outcome("null")).has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    CHECK_FALSE(run.finished());
    const auto idles = recorder->idle_list();
    REQUIRE(idles.size() == 1);
    CHECK(idles[0] == std::pair<std::int64_t, std::int64_t>{0, 2});

    stop.request_stop();
    auto result = run_async(run.result());
    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Aborted);
    CHECK(result->error->message == "missing result");
}

TEST_CASE("Remote deterministic execution refuses a replay with a different seed or fingerprint",
        "[codemode][remote][deterministic][issue1099]") {
    // Frozen pi v1.3.0 (`deterministic-replay.json`): a seed mismatch is refused
    // before the VM exists, and a fingerprint mismatch settles as a sandbox
    // error after the run reports the fingerprint it actually produced.
    auto sandbox = make_remote_sandbox();
    auto recorder = std::make_shared<DeterministicRecorder>();

    auto seed_options = recording_options(recorder);
    seed_options.seed = 2;
    seed_options.expected_seed = 1;
    auto seed_mismatch = sandbox->start("return 1", std::move(seed_options));
    REQUIRE_FALSE(seed_mismatch.has_value());
    CHECK(seed_mismatch.error().message == "Deterministic replay seed mismatch: expected 1, got 2");

    auto fingerprint_options = recording_options(recorder);
    fingerprint_options.expected_fingerprint = "not the VM's fingerprint";
    auto started = sandbox->start("return 1", std::move(fingerprint_options));
    REQUIRE(started.has_value());
    auto result = run_async(started->result());
    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Sandbox);
    CHECK(result->error->message.starts_with("Deterministic replay fingerprint mismatch: expected"));
    CHECK_FALSE(recorder->fingerprint.empty());
}

TEST_CASE("RemoteCodemodeSandbox runs tools in the caller and the script on the remote", "[codemode][remote]") {
    // Frozen pi v1.3.0 (audit-1053 remote-exchange.json, "a generic remote runs
    // the script and calls the caller's tools across the exchange"): the
    // script, the store seed and every compared field below are the
    // registered observation's own.
    const auto artifact = read_remote_exchange_artifact();
    const auto& frozen = remote_scenario(
            artifact, "a generic remote runs the script and calls the caller's tools across the exchange");
    const auto& frozen_inputs = frozen.get_object().at("inputs").get_object();
    const auto& frozen_expected = frozen.get_object().at("expected").get_object();
    const auto& frozen_call = frozen_expected.at("calls").get_array().at(0).get_object();
    auto remote = std::make_shared<RecordingRemote>();
    codemode::RemoteCodemodeSandboxOptions options;
    options.remote = [remote]() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> { return remote; };

    auto created = codemode::RemoteCodemodeSandbox::create(std::move(options));
    REQUIRE(created.has_value());
    auto sandbox = std::move(*created);

    std::vector<std::string> seen_args;
    codemode::CodemodeToolCallHandler handler =
            [&](std::string_view name, std::string_view args, std::stop_token) -> support::AsyncResult<std::string> {
        if (name == frozen_call.at("name").get_string()) {
            seen_args.emplace_back(args);
            auto parsed = support::read_json(args);
            if (parsed && parsed->holds<support::JsonValue::object_t>()) {
                const auto& obj = parsed->get_object();
                if (obj.contains("a") && obj.at("a").holds<double>()) {
                    auto sum = static_cast<int>(obj.at("a").get_number()) + 1;
                    return support::AsyncResult<std::string>{support::Expected<std::string>{std::to_string(sum)}};
                }
            }
            return support::AsyncResult<std::string>{support::Expected<std::string>{"2"}};
        }
        return support::AsyncResult<std::string>{support::Expected<std::string>{
                std::unexpected(support::make_error(support::ErrorCode::Validation, "unknown tool"))}};
    };

    std::vector<codemode::CodemodeToolDescriptor> tools{{.name = frozen_call.at("name").get_string(),
            .js_name = frozen_call.at("name").get_string(),
            .description = "Add 1 to number"}};

    std::map<std::string, support::JsonValue> initial_store;
    for (const auto& [key, value] : frozen_inputs.at("store").get_object()) {
        initial_store.emplace(key, value);
    }

    auto result = run_async(sandbox->run(frozen_inputs.at("script").get_string(),
            std::move(tools),
            {},
            {},
            std::move(handler),
            std::move(initial_store)));

    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    CHECK(result->value_json.value_or("") ==
            std::to_string(static_cast<std::int64_t>(frozen_expected.at("value").get_number())));
    REQUIRE(result->output.size() == 1);
    CHECK(result->output[0].data ==
            frozen_expected.at("output").get_array().at(0).get_object().at("text").get_string());
    REQUIRE(result->calls.size() == 1);
    CHECK(result->calls[0].name == frozen_call.at("name").get_string());
    CHECK(result->calls[0].status == codemode::CodemodeCallRecord::Status::Ok);
    CHECK(result->calls[0].duration_ms.has_value() == frozen_call.at("durationPresent").get_boolean());
    // The frozen exchange reports an empty store write set for a successful
    // run; a remote that drops the terminal `writes` payload fails here.
    REQUIRE(result->store_writes.has_value());
    CHECK(result->store_writes->set.empty());
    CHECK(result->store_writes->deleted.empty());

    REQUIRE(seen_args.size() == 1);
    auto parsed_seen = support::read_json(seen_args[0]);
    REQUIRE(parsed_seen.has_value());
    const auto& frozen_seen = frozen_expected.at("seen").get_array().at(0).get_object();
    CHECK(parsed_seen->get_object().at("a").get_number() == frozen_seen.at("a").get_number());

    // The frozen exchange sequence and replies, read from the artifact:
    // exchange 0 carries ["output", "call:add"], exchange 1 carries ["done"],
    // and the replies are ["result:true", "null"].
    REQUIRE(remote->exchange_message_kinds.size() == frozen_expected.at("exchanges").get_array().size());
    for (std::size_t index = 0; index < remote->exchange_message_kinds.size(); ++index) {
        std::vector<std::string> kinds;
        for (const auto& kind : frozen_expected.at("exchanges").get_array().at(index).get_array()) {
            kinds.push_back(kind.get_string());
        }
        CHECK(remote->exchange_message_kinds[index] == kinds);
    }
    REQUIRE(remote->reply_kinds.size() == frozen_expected.at("replies").get_array().size());
    for (std::size_t index = 0; index < remote->reply_kinds.size(); ++index) {
        CHECK(remote->reply_kinds[index] == frozen_expected.at("replies").get_array().at(index).get_string());
    }
}

TEST_CASE("RemoteCodemodeSandbox batches output instead of making one exchange per line", "[codemode][remote]") {
    // Frozen pi v1.3.0 (audit-1053 remote-exchange.json, "remote output is
    // buffered instead of one exchange per output item"): the item count and
    // the bounded exchange shape compared below are the registered
    // observation's own fields.
    const auto artifact = read_remote_exchange_artifact();
    const auto& frozen = remote_scenario(artifact, "remote output is buffered instead of one exchange per output item");
    const auto& frozen_inputs = frozen.get_object().at("inputs").get_object();
    const auto& frozen_expected = frozen.get_object().at("expected").get_object();
    const auto item_count = static_cast<std::int64_t>(frozen_inputs.at("outputItems").get_number());
    auto remote = std::make_shared<RecordingRemote>();
    codemode::RemoteCodemodeSandboxOptions options;
    options.remote = [remote]() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> { return remote; };

    auto created = codemode::RemoteCodemodeSandbox::create(std::move(options));
    REQUIRE(created.has_value());
    auto sandbox = std::move(*created);

    auto result = run_async(
            sandbox->run("for (let i = 0; i < " + std::to_string(item_count) + "; i++) console.log(i); return 'ok'"));

    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    CHECK(result->value_json.value_or("") == "\"ok\"");
    REQUIRE(result->output.size() == static_cast<std::size_t>(frozen_expected.at("outputCount").get_number()));
    const auto& frozen_last = frozen_expected.at("last").get_object();
    CHECK(result->output[4999].data == frozen_last.at("text").get_string());
    CHECK(result->output[4999].console == !frozen_last.at("console").holds<std::nullptr_t>());

    // Bounded exchange count with the frozen batch sizes.
    CHECK(remote->exchange_sizes.size() == static_cast<std::size_t>(frozen_expected.at("exchanges").get_number()));
    std::vector<std::size_t> expected_sizes;
    for (const auto& size : frozen_expected.at("exchangeSizes").get_array()) {
        expected_sizes.push_back(static_cast<std::size_t>(size.get_number()));
    }
    CHECK(remote->exchange_sizes == expected_sizes);
}

TEST_CASE("serve_codemode_remote stops when an output batch receives a stop reply", "[codemode][remote]") {
    std::size_t output_count = 0;
    std::size_t terminal_count = 0;
    codemode::CodemodeRemoteExchangeHook exchange =
            [&output_count, &terminal_count](std::vector<codemode::CodemodeRemoteMessageVariant> messages)
            -> support::AsyncResult<codemode::CodemodeRemoteReplyVariant> {
        for (const auto& message : messages) {
            if (std::holds_alternative<codemode::CodemodeRemoteOutputMessage>(message)) ++output_count;
            if (std::holds_alternative<codemode::CodemodeRemoteDoneMessage>(message)) ++terminal_count;
        }
        return support::AsyncResult<codemode::CodemodeRemoteReplyVariant>{
                support::Expected<codemode::CodemodeRemoteReplyVariant>{codemode::CodemodeRemoteStopReply{}}};
    };

    codemode::CodemodeRemoteRequest request;
    request.data.code = "for (let i = 0; i < 1000; i++) console.log(i); return 'unreachable'";
    auto result = run_async(codemode::serve_codemode_remote(std::move(request), std::move(exchange)));

    REQUIRE(result.has_value());
    CHECK(output_count == 1000);
    CHECK(terminal_count == 0);
}

TEST_CASE("RemoteCodemodeSandbox caps the interrupt budget and memory at the remote's limits", "[codemode][remote]") {
    // Frozen pi v1.3.0 (audit-1053 remote-exchange.json, "the remote caps the
    // requested interrupt budget and memory limit"): the served caps, the
    // requested budget and both error records compared below are the
    // registered observation's own fields.
    const auto artifact = read_remote_exchange_artifact();
    const auto& frozen = remote_scenario(artifact, "the remote caps the requested interrupt budget and memory limit");
    const auto& frozen_inputs = frozen.get_object().at("inputs").get_object();
    const auto& frozen_expected = frozen.get_object().at("expected").get_object();
    const auto& frozen_timeout = frozen_expected.at("timeout").get_object();
    const auto& frozen_memory = frozen_expected.at("memory").get_object();
    codemode::CodemodeServeOptions serve_opts;
    serve_opts.max_interrupt_budget = static_cast<std::size_t>(frozen_inputs.at("maxInterruptBudget").get_number());
    serve_opts.max_memory_limit_bytes = static_cast<std::size_t>(frozen_inputs.at("maxMemoryLimitBytes").get_number());

    auto remote = std::make_shared<RecordingRemote>(serve_opts);
    codemode::RemoteCodemodeSandboxOptions options;
    options.remote = [remote]() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> { return remote; };
    options.interrupt_budget = static_cast<std::size_t>(frozen_inputs.at("requestedInterruptBudget").get_number());

    auto created = codemode::RemoteCodemodeSandbox::create(std::move(options));
    REQUIRE(created.has_value());
    auto sandbox = std::move(*created);

    // 1. Interrupt budget cap
    auto timeout_result = run_async(sandbox->run("while (true) {}"));
    REQUIRE(timeout_result.has_value());
    REQUIRE(timeout_result->error.has_value());
    CHECK(timeout_result->error->kind == codemode::CodemodeError::Kind::Timeout);
    CHECK(timeout_result->error->message == frozen_timeout.at("message").get_string());

    // 2. Memory limit cap
    auto mem_result =
            run_async(sandbox->run("const parts = []; for (;;) parts.push('x'.repeat(1 << 20) + parts.length);"));
    REQUIRE(mem_result.has_value());
    REQUIRE(mem_result->error.has_value());
    CHECK(mem_result->error->kind == codemode::CodemodeError::Kind::Script);
    CHECK(mem_result->error->name.value_or("") == frozen_memory.at("name").get_string());
    CHECK(mem_result->error->message == frozen_memory.at("message").get_string());
}

TEST_CASE("RemoteCodemodeSandbox tells the remote to stop when caller aborts and settles immediately",
        "[codemode][remote]") {
    // Frozen pi v1.3.0 (audit-1053 remote-exchange.json, "caller abort answers
    // the remote's exchange with stop and settles without waiting for remote
    // compute"): the aborted error and the cancelled hanging call compared
    // below are the registered observation's own fields; the stop reply is
    // the frozen reply kind.
    const auto artifact = read_remote_exchange_artifact();
    const auto& frozen = remote_scenario(artifact,
            "caller abort answers the remote's exchange with stop and settles without waiting for remote compute");
    const auto& frozen_expected = frozen.get_object().at("expected").get_object();
    const auto& frozen_call = frozen_expected.at("calls").get_array().at(0).get_object();
    auto remote = std::make_shared<RecordingRemote>();
    auto stop_reply_received = remote->stop_reply_received.get_future();
    codemode::RemoteCodemodeSandboxOptions options;
    options.remote = [remote]() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> { return remote; };

    auto created = codemode::RemoteCodemodeSandbox::create(std::move(options));
    REQUIRE(created.has_value());
    auto sandbox = std::move(*created);

    std::promise<void> tool_started;
    auto tool_started_future = tool_started.get_future();
    std::stop_source stop_src;

    codemode::CodemodeToolCallHandler handler =
            [&](std::string_view name, std::string_view, std::stop_token signal) -> support::AsyncResult<std::string> {
        if (name == "hang") {
            tool_started.set_value();
            // Hang until signal cancels
            return support::detail::make_async_result(
                    [signal]() -> boost::asio::awaitable<support::Expected<std::string>> {
                        auto executor = co_await boost::asio::this_coro::executor;
                        boost::asio::steady_timer timer(executor);
                        while (!signal.stop_requested()) {
                            timer.expires_after(std::chrono::milliseconds{10});
                            co_await timer.async_wait(boost::asio::use_awaitable);
                        }
                        co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "cancelled"));
                    });
        }
        return support::AsyncResult<std::string>{support::Expected<std::string>{""}};
    };

    std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "hang", .js_name = "hang", .description = "Hangs until aborted"}};

    // Spawn execution in thread
    std::stop_source caller_stop;
    auto run_future = std::async(std::launch::async, [&]() {
        return run_async(sandbox->run("await tools.hang(); return 'never'",
                std::move(tools),
                {},
                caller_stop.get_token(),
                std::move(handler)));
    });

    // Wait until tool starts
    tool_started_future.wait();

    const auto abort_time = std::chrono::steady_clock::now();
    caller_stop.request_stop();

    auto run_res = run_future.get();
    const auto settled_time = std::chrono::steady_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(settled_time - abort_time).count();

    // Must settle without waiting for remote compute or worker delay (discriminating check: < 200 ms).
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    CHECK(elapsed_ms < 200);
#else
    (void)elapsed_ms;
#endif

    REQUIRE(run_res.has_value());
    REQUIRE(run_res->error.has_value());
    CHECK(run_res->error->kind == codemode::CodemodeError::Kind::Aborted);
    CHECK(run_res->error->message == frozen_expected.at("error").get_object().at("message").get_string());
    REQUIRE(run_res->calls.size() == 1);
    CHECK(run_res->calls[0].name == frozen_call.at("name").get_string());
    CHECK(run_res->calls[0].status == codemode::CodemodeCallRecord::Status::Cancelled);
    CHECK(run_res->calls[0].duration_ms.has_value() == frozen_call.at("durationPresent").get_boolean());

    // Reply sent across exchange must be stop.
    CHECK(stop_reply_received.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
}

TEST_CASE(
        "RemoteCodemodeSandbox discriminating abort check: does not block on slow remote exit", "[codemode][remote]") {
    // Discriminating test: A slow remote that delays 600 ms after receiving stop before returning execute().
    // The caller's run() must return immediately (< 100 ms) instead of waiting for the remote exit!
    struct SlowExitRemote final : public codemode::CodemodeRemote {
        [[nodiscard]] support::AsyncResult<void> execute(
                codemode::CodemodeRemoteRequest, codemode::CodemodeRemoteExchangeHook exchange) override {
            return support::detail::make_async_result(
                    [exchange = std::move(exchange)]() mutable -> boost::asio::awaitable<support::Expected<void>> {
                        auto executor = co_await boost::asio::this_coro::executor;
                        std::vector<codemode::CodemodeRemoteMessageVariant> msgs;
                        msgs.push_back(codemode::CodemodeRemoteCallMessage{
                                .id = 1, .target = "tool", .name = "slow_call", .args = std::nullopt});
                        auto reply = co_await support::detail::await_async_result(exchange(std::move(msgs)));
                        if (reply && std::holds_alternative<codemode::CodemodeRemoteStopReply>(*reply)) {
                            // Simulate slow remote worker cleanup
                            boost::asio::steady_timer delay(executor);
                            delay.expires_after(std::chrono::milliseconds{600});
                            co_await delay.async_wait(boost::asio::use_awaitable);
                        }
                        co_return support::ExpectedVoid{};
                    });
        }
    };

    auto slow_remote = std::make_shared<SlowExitRemote>();
    codemode::RemoteCodemodeSandboxOptions options;
    options.remote = [slow_remote]() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> {
        return slow_remote;
    };

    auto created = codemode::RemoteCodemodeSandbox::create(std::move(options));
    REQUIRE(created.has_value());
    auto sandbox = std::move(*created);

    std::promise<void> call_started;
    auto call_started_future = call_started.get_future();
    std::stop_source stop_src;

    codemode::CodemodeToolCallHandler handler =
            [&](std::string_view, std::string_view, std::stop_token stop) -> support::AsyncResult<std::string> {
        call_started.set_value();
        return support::detail::make_async_result([stop]() -> boost::asio::awaitable<support::Expected<std::string>> {
            auto executor = co_await boost::asio::this_coro::executor;
            boost::asio::steady_timer timer(executor);
            while (!stop.stop_requested()) {
                timer.expires_after(std::chrono::milliseconds{10});
                co_await timer.async_wait(boost::asio::use_awaitable);
            }
            co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "cancelled"));
        });
    };

    std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "slow_call", .js_name = "slow_call", .description = "call"}};

    auto run_future = std::async(std::launch::async, [&]() {
        return run_async(sandbox->run("dummy", std::move(tools), {}, stop_src.get_token(), std::move(handler)));
    });

    call_started_future.wait();
    const auto t0 = std::chrono::steady_clock::now();
    stop_src.request_stop();
    auto result = run_future.get();
    const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    // If caller blocked on remote execute() exit, elapsed would be >= 600 ms.
    // It must settle without waiting for remote compute: elapsed < 150 ms.
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    CHECK(elapsed < 150);
#else
    (void)elapsed;
#endif
    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Aborted);
}

TEST_CASE(
        "RemoteCodemodeSandbox rejects runs after close and closes active executions promptly", "[codemode][remote]") {
    struct DelayedExitRemote final : public codemode::CodemodeRemote {
        std::promise<void> stopped;
        std::atomic<bool> returned{false};

        [[nodiscard]] support::AsyncResult<void> execute(
                codemode::CodemodeRemoteRequest, codemode::CodemodeRemoteExchangeHook exchange) override {
            return support::detail::make_async_result([this, exchange = std::move(exchange)]() mutable
                                                              -> boost::asio::awaitable<support::Expected<void>> {
                auto reply = co_await support::detail::await_async_result(exchange({codemode::CodemodeRemoteCallMessage{
                        .id = 1, .target = "tool", .name = "hang", .args = "null"}}));
                if (reply && std::holds_alternative<codemode::CodemodeRemoteStopReply>(*reply)) {
                    stopped.set_value();
                    auto executor = co_await boost::asio::this_coro::executor;
                    boost::asio::steady_timer delay(executor);
                    delay.expires_after(std::chrono::milliseconds{300});
                    co_await delay.async_wait(boost::asio::use_awaitable);
                }
                returned.store(true);
                co_return support::ExpectedVoid{};
            });
        }
    };

    auto remote = std::make_shared<DelayedExitRemote>();
    codemode::RemoteCodemodeSandboxOptions options;
    options.remote = [remote]() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> { return remote; };
    auto created = codemode::RemoteCodemodeSandbox::create(std::move(options));
    REQUIRE(created.has_value());
    auto sandbox = std::move(*created);

    auto stopped = remote->stopped.get_future();
    std::promise<void> handler_started;
    auto handler_started_future = handler_started.get_future();
    codemode::CodemodeToolCallHandler handler = [&handler_started](std::string_view,
                                                        std::string_view,
                                                        std::stop_token signal) -> support::AsyncResult<std::string> {
        handler_started.set_value();
        return support::detail::make_async_result([signal]() -> boost::asio::awaitable<support::Expected<std::string>> {
            auto executor = co_await boost::asio::this_coro::executor;
            boost::asio::steady_timer timer(executor);
            while (!signal.stop_requested()) {
                timer.expires_after(std::chrono::milliseconds{10});
                co_await timer.async_wait(boost::asio::use_awaitable);
            }
            co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "closed"));
        });
    };
    auto run_future = std::async(std::launch::async, [&] {
        return run_async(sandbox->run("await tools.hang()",
                {{.name = "hang", .js_name = "hang", .description = "waits"}},
                {},
                {},
                std::move(handler)));
    });
    handler_started_future.wait();
    const auto close_time = std::chrono::steady_clock::now();
    sandbox->close();
    auto result = run_future.get();
    const auto close_elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - close_time)
                    .count();

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    // Sanitizer overhead invalidates this performance bound; functional assertions still run.
    (void)close_elapsed;
#else
    CHECK(close_elapsed < 150);
#endif
    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Aborted);
    CHECK(result->error->message == "Sandbox closed");
    CHECK(result->calls.size() == 1);
    CHECK(result->calls.front().status == codemode::CodemodeCallRecord::Status::Cancelled);
    CHECK(stopped.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
    CHECK_FALSE(remote->returned.load());

    auto after_close = run_async(sandbox->run("return 2"));
    REQUIRE(after_close.has_value());
    REQUIRE(after_close->error.has_value());
    CHECK(after_close->error->kind == codemode::CodemodeError::Kind::Aborted);
    CHECK(after_close->error->message == "Sandbox closed");
}

TEST_CASE("RemoteCodemodeSandbox keeps a settled result when remote cleanup fails later", "[codemode][remote]") {
    struct LateFailureRemote final : public codemode::CodemodeRemote {
        std::shared_ptr<tests::ReleaseGate> release_gate = std::make_shared<tests::ReleaseGate>();
        std::mutex executor_mutex;
        std::optional<boost::asio::any_io_executor> executor;
        std::promise<void> reply_received;
        std::promise<void> failure_returned;

        void release() {
            std::optional<boost::asio::any_io_executor> target;
            {
                std::lock_guard lock(executor_mutex);
                target = executor;
            }
            if (target) {
                boost::asio::post(*target, [release_gate = release_gate] { release_gate->release(); });
            }
        }

        [[nodiscard]] support::AsyncResult<void> execute(
                codemode::CodemodeRemoteRequest, codemode::CodemodeRemoteExchangeHook exchange) override {
            return support::detail::make_async_result(
                    [this, release_gate = release_gate, exchange = std::move(exchange)]() mutable
                            -> boost::asio::awaitable<support::Expected<void>> {
                        auto reply = co_await support::detail::await_async_result(
                                exchange({codemode::CodemodeRemoteDoneMessage{.ok = true,
                                        .value = "\\\"settled\\\"",
                                        .writes = "[]",
                                        .error = std::nullopt,
                                        .during = std::nullopt}}));
                        if (!reply) co_return support::ExpectedVoid{};
                        auto current_executor = co_await boost::asio::this_coro::executor;
                        {
                            std::lock_guard lock(executor_mutex);
                            executor = current_executor;
                        }
                        reply_received.set_value();
                        co_await release_gate->wait();
                        failure_returned.set_value();
                        co_return std::unexpected(support::make_error(support::ErrorCode::Process, "late reset"));
                    });
        }
    };

    auto remote = std::make_shared<LateFailureRemote>();
    codemode::RemoteCodemodeSandboxOptions options;
    options.remote = [remote]() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> { return remote; };
    auto created = codemode::RemoteCodemodeSandbox::create(std::move(options));
    REQUIRE(created.has_value());
    auto sandbox = std::move(*created);
    auto reply_received = remote->reply_received.get_future();
    auto failure_returned = remote->failure_returned.get_future();

    auto result = run_async(sandbox->run("return 1"));
    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    CHECK(result->value_json == "\\\"settled\\\"");
    CHECK(reply_received.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
    remote->release();
    CHECK(failure_returned.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
    CHECK_FALSE(result->error.has_value());
}

TEST_CASE("RemoteCodemodeSandbox reports failing, silent or unreachable remote as sandbox error with transport reason",
        "[codemode][remote]") {
    // Frozen pi v1.3.0 (audit-1053 remote-exchange.json, "a failing, silent or
    // unreachable remote is a sandbox error with the transport reason"): the
    // three error records compared below are the registered observation's own
    // fields, in the frozen order.
    const auto artifact = read_remote_exchange_artifact();
    const auto& frozen_expected = remote_scenario(
            artifact, "a failing, silent or unreachable remote is a sandbox error with the transport reason")
                                          .get_object()
                                          .at("expected")
                                          .get_array();
    // 1. Failing / rejecting remote
    {
        struct FailingRemote final : public codemode::CodemodeRemote {
            [[nodiscard]] support::AsyncResult<void> execute(
                    codemode::CodemodeRemoteRequest, codemode::CodemodeRemoteExchangeHook) override {
                return support::AsyncResult<void>{
                        std::unexpected(support::make_error(support::ErrorCode::Validation, "object reset"))};
            }
        };

        codemode::RemoteCodemodeSandboxOptions options;
        options.remote = []() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> {
            return std::make_shared<FailingRemote>();
        };
        auto sandbox = *codemode::RemoteCodemodeSandbox::create(std::move(options));

        auto res = run_async(sandbox->run("return 1"));
        REQUIRE(res.has_value());
        REQUIRE(res->error.has_value());
        CHECK(res->error->kind == codemode::CodemodeError::Kind::Sandbox);
        CHECK(res->error->message == frozen_expected.at(0).get_object().at("message").get_string());
    }

    // 2. Silent remote (returns without settling script)
    {
        struct SilentRemote final : public codemode::CodemodeRemote {
            [[nodiscard]] support::AsyncResult<void> execute(
                    codemode::CodemodeRemoteRequest, codemode::CodemodeRemoteExchangeHook) override {
                return support::AsyncResult<void>{support::ExpectedVoid{}};
            }
        };

        codemode::RemoteCodemodeSandboxOptions options;
        options.remote = []() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> {
            return std::make_shared<SilentRemote>();
        };
        auto sandbox = *codemode::RemoteCodemodeSandbox::create(std::move(options));

        auto res = run_async(sandbox->run("return 1"));
        REQUIRE(res.has_value());
        REQUIRE(res->error.has_value());
        CHECK(res->error->kind == codemode::CodemodeError::Kind::Sandbox);
        CHECK(res->error->message == frozen_expected.at(1).get_object().at("message").get_string());
    }

    // 3. Unreachable remote
    {
        codemode::RemoteCodemodeSandboxOptions options;
        options.remote = []() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> {
            return std::unexpected(support::make_error(support::ErrorCode::Validation, "no binding"));
        };
        auto sandbox = *codemode::RemoteCodemodeSandbox::create(std::move(options));

        auto res = run_async(sandbox->run("return 1"));
        REQUIRE(res.has_value());
        REQUIRE(res->error.has_value());
        CHECK(res->error->kind == codemode::CodemodeError::Kind::Sandbox);
        CHECK(res->error->message == frozen_expected.at(2).get_object().at("message").get_string());
    }
}

// ---------------------------------------------------------------------------
// #1193: the generic Remote row's independent frozen capture (audit-1193).
// The scenarios below replay audit-1193/remote-generic.json through the
// public Remote surface: the request that crosses to the remote, the exchange
// message sequence, the terminal writes set, and the three transport errors.
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] std::optional<double> parse_remote_number(std::string_view input) {
    const auto whitespace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!input.empty() && whitespace(input.front()))
        input.remove_prefix(1);
    while (!input.empty() && whitespace(input.back()))
        input.remove_suffix(1);
    auto rest = input;
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    if (!rest.empty() && rest.front() == '-') rest.remove_prefix(1);
    if (rest.empty()) return std::nullopt;
    if (rest.front() == '0') {
        rest.remove_prefix(1);
    } else {
        if (rest.front() < '1' || rest.front() > '9') return std::nullopt;
        while (!rest.empty() && digit(rest.front()))
            rest.remove_prefix(1);
    }
    if (!rest.empty() && rest.front() == '.') {
        rest.remove_prefix(1);
        if (rest.empty() || !digit(rest.front())) return std::nullopt;
        while (!rest.empty() && digit(rest.front()))
            rest.remove_prefix(1);
    }
    if (!rest.empty() && (rest.front() == 'e' || rest.front() == 'E')) {
        rest.remove_prefix(1);
        if (!rest.empty() && (rest.front() == '+' || rest.front() == '-')) rest.remove_prefix(1);
        if (rest.empty() || !digit(rest.front())) return std::nullopt;
        while (!rest.empty() && digit(rest.front()))
            rest.remove_prefix(1);
    }
    if (!rest.empty()) return std::nullopt;
    double value = 0;
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

[[nodiscard]] support::JsonValue read_1193_artifact(std::string_view name) {
    const auto path = std::filesystem::path{CCH_SOURCE_DIR} / "fixtures" / "pi-v1.3.0" / "audit-1193" / name;
    std::ifstream input(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto parsed = support::read_json(text);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

[[nodiscard]] const support::JsonValue* scenario_named(const support::JsonValue& artifact, std::string_view name) {
    for (const auto& candidate : artifact.get_object().at("scenarios").get_array()) {
        if (candidate.get_object().at("name").get_string() == name) return &candidate;
    }
    return nullptr;
}

/// A remote that serves the request in-process and records the request and
/// the exchange message kinds, exactly as the frozen capture's recording
/// remote does.
struct GenericRecordingRemote final : public codemode::CodemodeRemote {
    struct CallRecord {
        std::int64_t id;
        std::string target;
        std::string name;
        std::optional<std::string> args;
    };

    std::mutex mutex;
    std::optional<codemode::CodemodeRemoteRequest> request;
    std::vector<std::string> exchange_kinds;
    std::vector<CallRecord> calls;

    [[nodiscard]] support::AsyncResult<void> execute(
            codemode::CodemodeRemoteRequest incoming, codemode::CodemodeRemoteExchangeHook exchange) override {
        {
            std::lock_guard lock(mutex);
            request = incoming;
        }
        auto exchange_hook = std::make_shared<codemode::CodemodeRemoteExchangeHook>(std::move(exchange));
        auto wrapped = [this, exchange_hook](std::vector<codemode::CodemodeRemoteMessageVariant> messages)
                -> support::AsyncResult<codemode::CodemodeRemoteReplyVariant> {
            {
                std::lock_guard lock(mutex);
                for (const auto& message : messages) {
                    std::visit(
                            [&](const auto& m) {
                                using T = std::decay_t<decltype(m)>;
                                if constexpr (std::is_same_v<T, codemode::CodemodeRemoteCallMessage>) {
                                    exchange_kinds.push_back("call");
                                    calls.push_back(
                                            CallRecord{.id = m.id, .target = m.target, .name = m.name, .args = m.args});
                                } else if constexpr (std::is_same_v<T, codemode::CodemodeRemoteOutputMessage>) {
                                    exchange_kinds.push_back("output");
                                } else if constexpr (std::is_same_v<T, codemode::CodemodeRemoteDoneMessage>) {
                                    exchange_kinds.push_back("done");
                                } else if constexpr (std::is_same_v<T, codemode::CodemodeRemoteCrashMessage>) {
                                    exchange_kinds.push_back("crash");
                                } else if constexpr (std::is_same_v<T, codemode::CodemodeRemoteStartedMessage>) {
                                    exchange_kinds.push_back("started");
                                } else if constexpr (std::is_same_v<T, codemode::CodemodeRemoteIdleMessage>) {
                                    exchange_kinds.push_back("idle");
                                } else if constexpr (std::is_same_v<T, codemode::CodemodeRemoteFailureMessage>) {
                                    exchange_kinds.push_back("failure");
                                }
                            },
                            message);
                }
            }
            return (*exchange_hook)(std::move(messages));
        };
        auto done = codemode::serve_codemode_remote(std::move(incoming), std::move(wrapped));
        return done;
    }
};

} // namespace

TEST_CASE("the generic remote exchange runs the script remotely and calls a caller tool with the frozen contract",
        "[codemode][remote][issue1193][spec]") {
    // A remote that only returns the script value without exchange messages
    // fails this comparison: the recorded exchange sequence and the caller-side
    // tool execution are the compared contract.
    const auto artifact = read_1193_artifact("remote-generic.json");
    const auto* exchange =
            scenario_named(artifact, "a generic remote exchange runs the script remotely and calls a caller tool");
    REQUIRE(exchange != nullptr);
    const auto& expected = exchange->get_object().at("expected").get_object();

    auto remote = std::make_shared<GenericRecordingRemote>();
    codemode::RemoteCodemodeSandboxOptions options;
    options.remote = [remote]() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> { return remote; };
    auto created = codemode::RemoteCodemodeSandbox::create(std::move(options));
    REQUIRE(created.has_value());
    auto& sandbox = *created;

    std::vector<std::string> seen_args;
    std::map<std::string, support::JsonValue> store;
    store.emplace("k", support::JsonValue{1.0});
    auto result = run_async(sandbox->run(
            "text('hi'); return await tools.add({ a: 1 })",
            std::vector<codemode::CodemodeToolDescriptor>{{.name = "add", .js_name = "add", .description = "Add one."}},
            codemode::CodemodeLimits{},
            {},
            [&seen_args](std::string_view, std::string_view arguments_json, std::stop_token)
                    -> support::AsyncResult<std::string> {
                seen_args.emplace_back(arguments_json);
                return support::AsyncResult<std::string>{support::Expected<std::string>{std::string{"2"}}};
            },
            std::move(store)));
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->error.has_value());
    REQUIRE(result->value_json.has_value());
    CHECK(*result->value_json == "2");
    REQUIRE(result->output.size() == 1);
    CHECK(result->output[0].data == "hi");
    REQUIRE(result->calls.size() == 1);
    CHECK(result->calls[0].name == "add");
    CHECK(result->calls[0].status == codemode::CodemodeCallRecord::Status::Ok);
    REQUIRE(result->store_writes.has_value());
    CHECK(result->store_writes->set.empty());
    CHECK(result->store_writes->deleted.empty());
    REQUIRE(seen_args.size() == 1);
    CHECK(seen_args[0] == R"({"a":1})");

    // The request that crossed to the remote matches the frozen request:
    // the code, the tool table with its script name, and the store snapshot.
    std::lock_guard lock(remote->mutex);
    REQUIRE(remote->request.has_value());
    const auto& frozen_request = expected.at("request").get_object();
    CHECK(remote->request->data.code == frozen_request.at("code").get_string());
    REQUIRE(remote->request->data.tools.size() == 1);
    CHECK(remote->request->data.tools[0].name == "add");
    CHECK(remote->request->data.tools[0].js_name == "add");
    CHECK(remote->request->data.store.at("k") == "1");
    CHECK_FALSE(remote->request->interrupt_budget.has_value());

    // The exchange carried the caller's tool call and ended with the terminal
    // done message, in the frozen order.
    const auto& frozen_calls = expected.at("callMessages").get_array();
    REQUIRE(remote->calls.size() == frozen_calls.size());
    for (std::size_t i = 0; i < remote->calls.size(); ++i) {
        const auto& frozen_call = frozen_calls[i].get_object();
        CHECK(remote->calls[i].id == static_cast<std::int64_t>(frozen_call.at("id").get_number()));
        CHECK(remote->calls[i].target == frozen_call.at("target").get_string());
        CHECK(remote->calls[i].name == frozen_call.at("name").get_string());
        REQUIRE(remote->calls[i].args.has_value());
        CHECK(*remote->calls[i].args == frozen_call.at("args").get_string());
    }
    REQUIRE_FALSE(remote->exchange_kinds.empty());
    CHECK(remote->exchange_kinds.back() == "done");
}

TEST_CASE("remote numeric results require a complete in-range number", "[codemode][remote][issue1232]") {
    struct ValueRemote final : codemode::CodemodeRemote {
        std::string value;
        [[nodiscard]] support::AsyncResult<void> execute(
                codemode::CodemodeRemoteRequest, codemode::CodemodeRemoteExchangeHook exchange) override {
            return support::detail::make_async_result([value = value, exchange = std::move(exchange)]() mutable
                                                              -> boost::asio::awaitable<support::Expected<void>> {
                auto reply = co_await support::detail::await_async_result(exchange({codemode::CodemodeRemoteDoneMessage{
                        .ok = true, .value = value, .writes = "[]", .error = std::nullopt, .during = std::nullopt}}));
                if (!reply) co_return std::unexpected(reply.error());
                co_return support::ExpectedVoid{};
            });
        }
    };
    auto remote = std::make_shared<ValueRemote>();
    codemode::RemoteCodemodeSandboxOptions options;
    options.remote = [remote]() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> { return remote; };
    auto sandbox = codemode::RemoteCodemodeSandbox::create(std::move(options));
    REQUIRE(sandbox.has_value());
    for (const auto& [input, expected] : std::vector<std::pair<std::string, std::optional<double>>>{{"-12.5e1", -125.0},
                 {" \t0.25E+2\r\n", 25.0},
                 {"-0", 0.0},
                 {"1.7976931348623157e308", 0x1.fffffffffffffp1023},
                 {"4.9406564584124654e-324", 0x0.0000000000001p-1022},
                 {"1.25e-2", 0.0125},
                 {"", std::nullopt},
                 {" ", std::nullopt},
                 {"abc", std::nullopt},
                 {"nan", std::nullopt},
                 {"inf", std::nullopt},
                 {"1e9999", std::nullopt},
                 {"1e-9999", std::nullopt},
                 {"\v1", std::nullopt},
                 {"1\f", std::nullopt},
                 {"1junk", std::nullopt},
                 {"01", std::nullopt},
                 {"-01", std::nullopt},
                 {"1.", std::nullopt},
                 {".1", std::nullopt},
                 {"+1", std::nullopt},
                 {"1e+", std::nullopt},
                 {"1 2", std::nullopt}}) {
        remote->value = input;
        auto result = run_async((*sandbox)->run("return 1"));
        REQUIRE(result.has_value());
        REQUIRE_FALSE(result->error.has_value());
        REQUIRE(result->value_json.has_value());
        REQUIRE(result->store_writes.has_value());
        CHECK(result->store_writes->set.empty());
        CHECK(result->store_writes->deleted.empty());
        INFO("remote value: " << input);
        CHECK(parse_remote_number(*result->value_json) == expected);
    }
}

TEST_CASE("the generic remote terminal writes set is empty on success and carries the script write set",
        "[codemode][remote][issue1193][spec]") {
    const auto artifact = read_1193_artifact("remote-generic.json");
    const auto* writes = scenario_named(artifact,
            "the terminal writes set is empty on success and carries a real write set when the script writes");
    REQUIRE(writes != nullptr);
    const auto& expected = writes->get_object().at("expected").get_object();

    auto remote = std::make_shared<GenericRecordingRemote>();
    codemode::RemoteCodemodeSandboxOptions options;
    options.remote = [remote]() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> { return remote; };
    auto created = codemode::RemoteCodemodeSandbox::create(std::move(options));
    REQUIRE(created.has_value());
    auto& sandbox = *created;

    auto plain = run_async(sandbox->run("return 1"));
    auto writing = run_async(sandbox->run("store('a', 1); store('b', { c: 2 }); return load('a');"));

    REQUIRE(plain.has_value());
    REQUIRE_FALSE(plain->error.has_value());
    REQUIRE(plain->value_json.has_value());
    const auto plain_number = parse_remote_number(*plain->value_json);
    REQUIRE(plain_number.has_value());
    CHECK(*plain_number == expected.at("plain").get_object().at("value").get_number());
    REQUIRE(plain->store_writes.has_value());
    CHECK(plain->store_writes->set.empty());
    CHECK(plain->store_writes->deleted.empty());

    REQUIRE(writing.has_value());
    REQUIRE_FALSE(writing->error.has_value());
    REQUIRE(writing->value_json.has_value());
    const auto writing_number = parse_remote_number(*writing->value_json);
    REQUIRE(writing_number.has_value());
    CHECK(*writing_number == expected.at("writing").get_object().at("value").get_number());
    REQUIRE(writing->store_writes.has_value());
    const auto& frozen_set = expected.at("writing").get_object().at("writes").get_object().at("set").get_object();
    REQUIRE(writing->store_writes->set.size() == frozen_set.size());
    auto written_b = support::write_json(writing->store_writes->set.at("b"));
    REQUIRE(written_b.has_value());
    CHECK(*written_b == R"({"c":2})");
    CHECK(writing->store_writes->set.at("a").get_number() == 1);
    CHECK(writing->store_writes->deleted.empty());
}

TEST_CASE("the generic remote keeps three distinct sandbox errors for rejected silent and unreachable remotes",
        "[codemode][remote][issue1193][spec]") {
    const auto artifact = read_1193_artifact("remote-generic.json");
    const auto* failures =
            scenario_named(artifact, "a rejected silent or unreachable remote keeps three distinct sandbox errors");
    REQUIRE(failures != nullptr);
    const auto& expected = failures->get_object().at("expected").get_object();

    struct FailingRemote final : public codemode::CodemodeRemote {
        [[nodiscard]] support::AsyncResult<void> execute(
                codemode::CodemodeRemoteRequest, codemode::CodemodeRemoteExchangeHook) override {
            return support::AsyncResult<void>{
                    std::unexpected(support::make_error(support::ErrorCode::Validation, "object reset"))};
        }
    };
    struct SilentRemote final : public codemode::CodemodeRemote {
        [[nodiscard]] support::AsyncResult<void> execute(
                codemode::CodemodeRemoteRequest, codemode::CodemodeRemoteExchangeHook) override {
            return support::AsyncResult<void>{support::ExpectedVoid{}};
        }
    };

    for (std::size_t index = 0; index < 3; ++index) {
        const char* mode = index == 0 ? "rejected" : index == 1 ? "silent" : "unreachable";
        codemode::RemoteCodemodeSandboxOptions options;
        if (index == 0) {
            options.remote = []() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> {
                return std::make_shared<FailingRemote>();
            };
        } else if (index == 1) {
            options.remote = []() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> {
                return std::make_shared<SilentRemote>();
            };
        } else {
            options.remote = []() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> {
                return std::unexpected(support::make_error(support::ErrorCode::Validation, "no binding"));
            };
        }
        auto created = codemode::RemoteCodemodeSandbox::create(std::move(options));
        REQUIRE(created.has_value());
        auto result = run_async((*created)->run("return 1"));
        REQUIRE(result.has_value());
        REQUIRE(result->error.has_value());
        CHECK(result->error->kind == codemode::CodemodeError::Kind::Sandbox);
        const auto& frozen_error = expected.at(mode).get_object().at("error").get_object();
        CHECK(result->error->message == frozen_error.at("message").get_string());
    }
}
