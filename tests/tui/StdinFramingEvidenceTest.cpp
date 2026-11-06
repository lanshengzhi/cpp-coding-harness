#include <cch/tui/Keys.hpp>
#include <cch/tui/StdinBuffer.hpp>

#include "support/PiTuiEvidence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using cch::support::JsonValue;
using cch::tui::StdinBuffer;
using cch::tui::StdinBufferOptions;

constexpr auto kNoDeadline = std::chrono::steady_clock::time_point::max();

/// The two literals stay in one statement so the receipt-boundary matcher can
/// anchor this source to the registered artifact without reading helper code.
[[nodiscard]] cch::support::Expected<JsonValue> read_framing_evidence() {
    return cch::tests::read_pi_v130_audit_evidence("audit-1221-stdin-framing", "stdin-framing.json");
}

[[nodiscard]] std::vector<std::string> strings(const JsonValue& value) {
    std::vector<std::string> result;
    for (const auto& item : value.get_array())
        result.push_back(item.get_string());
    return result;
}

[[nodiscard]] std::chrono::milliseconds::rep checked_milliseconds(const JsonValue& value) {
    const auto number = value.get_number();
    REQUIRE(std::isfinite(number));
    REQUIRE(number >= 0.0);
    REQUIRE(number <= static_cast<double>(std::numeric_limits<std::chrono::milliseconds::rep>::max()));
    return static_cast<std::chrono::milliseconds::rep>(number); // §9.14 checked narrowing
}

[[nodiscard]] std::chrono::milliseconds millis(const JsonValue& value) {
    return std::chrono::milliseconds{checked_milliseconds(value)};
}

struct Event {
    std::string type;
    std::string value;
};

struct Observation {
    std::vector<std::string> data;
    std::vector<std::string> paste;
    std::vector<Event> events;
    std::string buffer;
    bool deadline_armed{false};
    std::optional<std::vector<std::string>> flush_return;
};

class Recorder final {
public:
    explicit Recorder(StdinBufferOptions options) : buffer(options) {
        buffer.set_data_handler([this](std::string sequence) {
            data.push_back(std::move(sequence));
            events.push_back(Event{.type = "data", .value = data.back()});
        });
        buffer.set_paste_handler([this](std::string content) {
            paste.push_back(std::move(content));
            events.push_back(Event{.type = "paste", .value = paste.back()});
        });
    }

    [[nodiscard]] Observation observe() const {
        return Observation{.data = data,
                .paste = paste,
                .events = events,
                .buffer = std::string{buffer.get_buffer()},
                .deadline_armed = buffer.deadline() != kNoDeadline,
                .flush_return = std::nullopt};
    }

    StdinBuffer buffer;
    std::vector<std::string> data;
    std::vector<std::string> paste;
    std::vector<Event> events;
};

void check_observation(const Observation& actual, const JsonValue& expected) {
    CHECK(actual.data == strings(expected.at("data")));
    CHECK(actual.paste == strings(expected.at("paste")));
    const auto& expected_events = expected.at("events").get_array();
    REQUIRE(actual.events.size() == expected_events.size());
    for (std::size_t index = 0; index < expected_events.size(); ++index) {
        INFO("event " << index);
        CHECK(actual.events[index].type == expected_events[index].at("type").get_string());
        CHECK(actual.events[index].value == expected_events[index].at("value").get_string());
    }
    CHECK(actual.buffer == expected.at("buffer").get_string());
    CHECK(actual.deadline_armed == expected.at("timeout").get_boolean());
    if (expected.get_object().contains("flushReturn")) {
        REQUIRE(actual.flush_return.has_value());
        CHECK(*actual.flush_return == strings(expected.at("flushReturn")));
    } else {
        CHECK_FALSE(actual.flush_return.has_value());
    }
}

/// Replays a captured step list. `emitDataSequence` is private in C++; the
/// public `process` path is the only caller of that method, so the direct
/// captured call is replayed through the same public seam.
void replay_cases(const JsonValue& scenario) {
    const auto& input_cases = scenario.at("inputs").at("cases").get_array();
    const auto& expected_cases = scenario.at("expected").at("cases").get_array();
    REQUIRE(input_cases.size() == expected_cases.size());
    for (std::size_t case_index = 0; case_index < input_cases.size(); ++case_index) {
        INFO(expected_cases[case_index].at("name").get_string());
        Recorder recorder({.timeout = std::chrono::milliseconds{50}, .escape_timeout = std::chrono::milliseconds{10}});
        const auto& steps = input_cases[case_index].at("steps").get_array();
        const auto& observations = expected_cases[case_index].at("observations").get_array();
        REQUIRE(steps.size() == observations.size());
        for (std::size_t step_index = 0; step_index < steps.size(); ++step_index) {
            const auto& step = steps[step_index];
            std::optional<std::vector<std::string>> flush_return;
            if (step.get_object().contains("process")) recorder.buffer.process(step.at("process").get_string());
            if (step.get_object().contains("emitDataSequence"))
                recorder.buffer.process(step.at("emitDataSequence").get_string());
            if (step.get_object().contains("flush")) flush_return = recorder.buffer.flush();
            if (step.get_object().contains("clear")) recorder.buffer.clear();
            if (step.get_object().contains("destroy")) recorder.buffer.destroy();
            if (step.get_object().contains("waitMs")) std::this_thread::sleep_for(millis(step.at("waitMs")));
            auto actual = recorder.observe();
            actual.flush_return = std::move(flush_return);
            check_observation(actual, observations[step_index]);
        }
    }
}

} // namespace

TEST_CASE("pi-v1.3.0 stdin framing options and events replay", "[tui][issue1221][compat-pi]") {
    const auto bundle = read_framing_evidence();
    REQUIRE(bundle);
    const auto& scenario = cch::tests::find_scenario(*bundle, "stdin-framing-options-and-events");
    const auto& inputs = scenario.at("inputs");
    const auto& expected = scenario.at("expected");

    StdinBuffer defaults({});
    CHECK(defaults.selected_timeout() == millis(expected.at("defaultTimeoutMs")));
    defaults.process("\x1b");
    CHECK(defaults.selected_timeout() == millis(expected.at("defaultEscapeTimeoutMs")));
    defaults.clear();

    Recorder explicit_options({
            .timeout = millis(expected.at("explicitTimeoutMs")),
            .escape_timeout = millis(expected.at("explicitEscapeTimeoutMs")),
    });
    CHECK(explicit_options.buffer.selected_timeout() == millis(expected.at("explicitTimeoutMs")));
    explicit_options.buffer.process("\x1b");
    CHECK(explicit_options.buffer.selected_timeout() == millis(expected.at("explicitEscapeTimeoutMs")));
    explicit_options.buffer.clear();

    // The frozen event map names exactly the two payload streams the C++
    // handler slots represent.
    CHECK(strings(expected.at("eventNames")) == std::vector<std::string>{"data", "paste"});
    Recorder recorder({
            .timeout = millis(expected.at("explicitTimeoutMs")),
            .escape_timeout = millis(expected.at("explicitEscapeTimeoutMs")),
    });
    recorder.buffer.process(inputs.at("stream").get_string());
    CHECK(recorder.data == strings(expected.at("data")));
    CHECK(recorder.paste == strings(expected.at("paste")));
    const auto& expected_events = expected.at("events").get_array();
    REQUIRE(recorder.events.size() == expected_events.size());
    for (std::size_t index = 0; index < expected_events.size(); ++index) {
        CHECK(recorder.events[index].type == expected_events[index].at("type").get_string());
        CHECK(recorder.events[index].value == expected_events[index].at("value").get_string());
    }
}

TEST_CASE("pi-v1.3.0 stdin framing paste lifecycle replay", "[tui][issue1221][compat-pi]") {
    const auto bundle = read_framing_evidence();
    REQUIRE(bundle);
    replay_cases(cch::tests::find_scenario(*bundle, "stdin-framing-paste-lifecycle"));
}

TEST_CASE("pi-v1.3.0 stdin framing buffer lifecycle replay", "[tui][issue1221][compat-pi]") {
    const auto bundle = read_framing_evidence();
    REQUIRE(bundle);
    replay_cases(cch::tests::find_scenario(*bundle, "stdin-framing-buffer-lifecycle"));
}

TEST_CASE("pi-v1.3.0 stdin framing emitDataSequence and kitty dedup replay", "[tui][issue1221][compat-pi]") {
    const auto bundle = read_framing_evidence();
    REQUIRE(bundle);
    replay_cases(cch::tests::find_scenario(*bundle, "stdin-framing-emit-and-kitty-dedup"));
}

TEST_CASE("pi-v1.3.0 stdin framing real deadline selection replay", "[tui][issue1221][compat-pi]") {
    const auto bundle = read_framing_evidence();
    REQUIRE(bundle);
    const auto& scenario = cch::tests::find_scenario(*bundle, "stdin-framing-timeout-selection");
    const auto& options = scenario.at("inputs").at("options");
    const auto timeout = millis(options.at("timeout"));
    const auto escape_timeout = millis(options.at("escapeTimeout"));
    const auto& input_cases = scenario.at("inputs").at("cases").get_array();
    const auto& expected_cases = scenario.at("expected").at("cases").get_array();
    REQUIRE(input_cases.size() == expected_cases.size());

    for (std::size_t case_index = 0; case_index < input_cases.size(); ++case_index) {
        INFO(expected_cases[case_index].at("name").get_string());
        Recorder recorder({.timeout = timeout, .escape_timeout = escape_timeout});
        const auto& steps = input_cases[case_index].at("steps").get_array();
        const auto& observations = expected_cases[case_index].at("observations").get_array();
        REQUIRE(steps.size() == observations.size());
        REQUIRE(steps.front().get_object().contains("process"));
        recorder.buffer.process(steps.front().at("process").get_string());
        check_observation(recorder.observe(), observations.front());

        for (std::size_t step_index = 1; step_index < steps.size(); ++step_index) {
            const auto& step = steps[step_index];
            if (step.get_object().contains("process")) {
                recorder.buffer.process(step.at("process").get_string());
            } else {
                REQUIRE(step.get_object().contains("waitMs"));
                const auto wait = millis(step.at("waitMs"));
                const auto deadline = recorder.buffer.deadline();
                if (deadline == kNoDeadline) {
                    // A recognized reply header waits for its terminator and
                    // arms no timeout at all.
                    std::this_thread::sleep_for(wait);
                } else {
                    const auto selected = recorder.buffer.selected_timeout();
                    const auto expected_selected = recorder.buffer.holds_lone_escape() ? escape_timeout : timeout;
                    CHECK(selected == expected_selected);
                    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - std::chrono::steady_clock::now());
                    CHECK(remaining <= selected);
                    if (std::chrono::steady_clock::now() + wait >= deadline) {
                        // A real deadline observation: wait for the armed
                        // instant, then end the fragment window through the
                        // same flush the terminal's readiness timer calls.
                        std::this_thread::sleep_until(deadline + std::chrono::milliseconds{5});
                        (void)recorder.buffer.flush();
                    } else {
                        std::this_thread::sleep_for(wait);
                    }
                }
            }
            check_observation(recorder.observe(), observations[step_index]);
        }
    }
}

TEST_CASE("pi-v1.3.0 stdin framing flush returns and timer callback replay", "[tui][issue1221][compat-pi]") {
    const auto bundle = read_framing_evidence();
    REQUIRE(bundle);
    const auto& scenario = cch::tests::find_scenario(*bundle, "stdin-framing-flush");
    const auto& input_cases = scenario.at("inputs").at("cases").get_array();
    const auto& expected_cases = scenario.at("expected").at("cases").get_array();
    REQUIRE(input_cases.size() == expected_cases.size());

    for (std::size_t case_index = 0; case_index < input_cases.size(); ++case_index) {
        INFO(expected_cases[case_index].at("name").get_string());
        Recorder recorder({.timeout = std::chrono::milliseconds{50}, .escape_timeout = std::chrono::milliseconds{10}});
        std::vector<std::string> flushed;
        for (const auto& step : input_cases[case_index].at("steps").get_array()) {
            if (step.get_object().contains("process")) recorder.buffer.process(step.at("process").get_string());
            if (step.get_object().contains("flush")) flushed = recorder.buffer.flush();
            if (step.get_object().contains("clear")) recorder.buffer.clear();
            if (step.get_object().contains("destroy")) recorder.buffer.destroy();
        }
        const auto& expected = expected_cases[case_index];
        CHECK(flushed == strings(expected.at("flushReturn")));
        // pi's flush returns the fragment without emitting; its timer callback
        // emits it. The C++ seam combines both, so its data history is the
        // frozen timer-callback observation.
        CHECK(recorder.data == strings(expected.at("timerCallbackData")));
        const auto& final_observation = expected.at("observations").get_array().back();
        CHECK(recorder.buffer.get_buffer() == final_observation.at("buffer").get_string());
        CHECK((recorder.buffer.deadline() != kNoDeadline) == final_observation.at("timeout").get_boolean());
    }
}

TEST_CASE("pi-v1.3.0 fragmented color reply header survives the real deadline", "[tui][issue1221][compat-pi]") {
    const auto bundle = read_framing_evidence();
    REQUIRE(bundle);
    const auto& scenario = cch::tests::find_scenario(*bundle, "fragmented-color-reply-header-survives-deadline");
    const auto& inputs = scenario.at("inputs");
    const auto& expected = scenario.at("expected");
    const auto& reply = inputs.at("reply").get_string();
    const auto& header = expected.at("bufferedAfterHeaderTimeout").get_string();
    REQUIRE(reply.starts_with(header));

    Recorder recorder({
            .timeout = millis(inputs.at("timeoutMs")),
            .escape_timeout = std::chrono::milliseconds{5},
    });
    recorder.buffer.process(header);
    CHECK(recorder.buffer.deadline() == kNoDeadline);
    std::this_thread::sleep_for(millis(inputs.at("tailWaitMs")));
    CHECK(recorder.data == strings(expected.at("afterHeaderTimeout")));
    CHECK(recorder.buffer.get_buffer() == header);

    recorder.buffer.process(reply.substr(header.size()));
    CHECK(recorder.data == strings(expected.at("afterTail")));
    REQUIRE_FALSE(recorder.data.empty());
    const auto completed = cch::tui::parse_key(recorder.data.back(), /*kitty_protocol_active=*/true);
    CHECK_FALSE(completed.has_value());
    if (completed) CHECK_FALSE(cch::tui::matches_key(*completed, "ctrl+g"));

    recorder.buffer.process("\x07");
    CHECK(recorder.data == strings(expected.at("afterRealCtrlG")));
    REQUIRE(recorder.data.size() > 1);
    const auto real_ctrl_g = cch::tui::parse_key(recorder.data.back(), /*kitty_protocol_active=*/true);
    REQUIRE(real_ctrl_g);
    CHECK(cch::tui::key_id(*real_ctrl_g) == "ctrl+g");
}

TEST_CASE("pi-v1.3.0 fragmented color reply never leaks or opens the editor", "[tui][issue1221][compat-pi]") {
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1050", "keys-and-terminal-replies.json");
    REQUIRE(bundle);
    const auto& scenario = cch::tests::find_scenario(*bundle, "fragmented-color-reply-never-leaks-or-opens-editor");
    const auto& inputs = scenario.at("inputs");
    const auto& expected = scenario.at("expected");
    const auto& reply = inputs.at("reply").get_string();
    const auto& header = expected.at("bufferedAfterHeaderTimeout").get_string();
    REQUIRE(reply.starts_with(header));

    Recorder recorder({
            .timeout = millis(inputs.at("timeoutMs")),
            .escape_timeout = std::chrono::milliseconds{5},
    });
    recorder.buffer.process(header);
    CHECK(recorder.buffer.deadline() == kNoDeadline);
    std::this_thread::sleep_for(millis(inputs.at("tailWaitMs")));
    CHECK(recorder.data == strings(expected.at("afterHeaderTimeout")));
    CHECK(recorder.buffer.get_buffer() == header);

    recorder.buffer.process(reply.substr(header.size()));
    CHECK(recorder.data == strings(expected.at("afterTail")));
    REQUIRE_FALSE(recorder.data.empty());
    const auto completed = cch::tui::parse_key(recorder.data.back(), /*kitty_protocol_active=*/true);
    CHECK_FALSE(completed.has_value());
    if (completed) CHECK_FALSE(cch::tui::matches_key(*completed, "ctrl+g"));

    recorder.buffer.process("\x07");
    CHECK(recorder.data == strings(expected.at("afterRealCtrlG")));
    REQUIRE(recorder.data.size() > 1);
    const auto real_ctrl_g = cch::tui::parse_key(recorder.data.back(), /*kitty_protocol_active=*/true);
    REQUIRE(real_ctrl_g);
    CHECK(cch::tui::key_id(*real_ctrl_g) == "ctrl+g");
}

TEST_CASE("pi-v1.3.0 fragmented stdin replies replay with a real deadline", "[tui][issue1221][compat-pi]") {
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1221", "input.json");
    REQUIRE(bundle);
    for (const auto name : {"fragmented-osc-reply", "fragmented-apc-reply", "ambiguous-alt-input"}) {
        INFO(name);
        const auto& scenario = cch::tests::find_scenario(*bundle, name);
        const auto& input = scenario.at("inputs");
        const auto& expected = scenario.at("expected");
        const auto timeout = millis(input.at("timeoutMs"));
        Recorder recorder({.timeout = timeout, .escape_timeout = timeout});
        const auto& chunks = input.at("chunks").get_array();
        REQUIRE_FALSE(chunks.empty());
        recorder.buffer.process(chunks.front().get_string());
        std::this_thread::sleep_for(millis(input.at("waitMs")));
        // The captured wait is longer than the sequence window: an ambiguous
        // Alt prefix has armed a deadline and is delivered by the flush; a
        // recognized reply header has no deadline and stays buffered.
        if (recorder.buffer.deadline() != kNoDeadline) (void)recorder.buffer.flush();
        CHECK(recorder.data == strings(expected.at("afterWait")));
        for (std::size_t index = 1; index < chunks.size(); ++index)
            recorder.buffer.process(chunks[index].get_string());
        CHECK(recorder.data == strings(expected.at("afterCompletion")));
    }
}

TEST_CASE("pi-v1.3.0 a partial OSC tail is not typed while the reply header waits", "[tui][issue1221][compat-pi]") {
    // A header that matches the frozen reply pattern must stay buffered across
    // the sequence timeout; a header that flushes at the deadline would type
    // the tail and turn the BEL into Ctrl+G.
    Recorder recorder({.timeout = std::chrono::milliseconds{20}, .escape_timeout = std::chrono::milliseconds{5}});
    recorder.buffer.process("\x1b]4;1;rgb:eaea/6c");
    CHECK(recorder.buffer.deadline() == kNoDeadline);
    std::this_thread::sleep_for(std::chrono::milliseconds{80});
    CHECK(recorder.data.empty());
    CHECK(recorder.buffer.get_buffer() == "\x1b]4;1;rgb:eaea/6c");
    recorder.buffer.process("6c/6d6d\x07");
    REQUIRE(recorder.data.size() == 1);
    CHECK(recorder.data.front() == "\x1b]4;1;rgb:eaea/6c6c/6d6d\x07");
    CHECK_FALSE(cch::tui::parse_key(recorder.data.front(), true).has_value());
}

TEST_CASE("pi-v1.3.0 a collapsed paste tail is data after the paste event", "[tui][issue1221][compat-pi]") {
    // The end marker completes the paste and the remaining bytes re-enter the
    // normal framing path; swallowing the tail would lose the next keystroke.
    Recorder recorder({});
    recorder.buffer.process("\x1b[200~hello\x1b[201~x");
    REQUIRE(recorder.paste.size() == 1);
    CHECK(recorder.paste.front() == "hello");
    REQUIRE(recorder.data.size() == 1);
    CHECK(recorder.data.front() == "x");
}

TEST_CASE("pi-v1.3.0 a lone ESC uses the escape deadline not the sequence deadline", "[tui][issue1221][compat-pi]") {
    Recorder recorder({.timeout = std::chrono::milliseconds{200}, .escape_timeout = std::chrono::milliseconds{20}});
    recorder.buffer.process("\x1b");
    CHECK(recorder.buffer.selected_timeout() == std::chrono::milliseconds{20});
    const auto deadline = recorder.buffer.deadline();
    REQUIRE(deadline != kNoDeadline);
    const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    CHECK(remaining <= std::chrono::milliseconds{20});
    CHECK(remaining >= std::chrono::milliseconds{0});
    std::this_thread::sleep_until(deadline + std::chrono::milliseconds{5});
    (void)recorder.buffer.flush();
    CHECK(recorder.data == std::vector<std::string>{"\x1b"});
}

TEST_CASE("pi-v1.3.0 clear drops the buffered fragment instead of flushing it", "[tui][issue1221][compat-pi]") {
    Recorder recorder({});
    recorder.buffer.process("\x1b[<35");
    CHECK(recorder.buffer.get_buffer() == "\x1b[<35");
    recorder.buffer.clear();
    CHECK(recorder.buffer.get_buffer().empty());
    CHECK(recorder.buffer.deadline() == kNoDeadline);
    recorder.buffer.process("x");
    CHECK(recorder.data == std::vector<std::string>{"x"});
}

TEST_CASE("pi-v1.3.0 an explicit-modifier kitty report does not suppress the next raw character",
        "[tui][issue1221][compat-pi]") {
    // Only a bare unmodified CSI-u printable report arms the duplicate
    // suppression; a dedup that ignores the modifier fields drops the "a".
    Recorder recorder({});
    recorder.buffer.process("\x1b[97;1u");
    recorder.buffer.process("a");
    CHECK(recorder.data == (std::vector<std::string>{"\x1b[97;1u", "a"}));
}

TEST_CASE("pi-v1.3.0 an empty flush returns no sequences and arms no deadline", "[tui][issue1221][compat-pi]") {
    Recorder recorder({});
    CHECK(recorder.buffer.flush().empty());
    CHECK(recorder.data.empty());
    CHECK(recorder.buffer.deadline() == kNoDeadline);
}
