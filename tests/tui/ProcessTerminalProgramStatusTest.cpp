// OSC 7501 detection, override, and exit clear on a real pseudo-terminal
// (#1123 TStatus). Replays the registered frozen pi-v1.3.0 observations in
// fixtures/pi-v1.3.0/audit-1050/program-status-detection.json (revision
// 71023aec1546): the startup query order, the reply-before-DA answer, the DA
// sentinel that leaves the terminal unsupported, both PI_PROGRAM_STATUS
// overrides, and the stop() clear.
//
// The pseudo-terminal stands in for the terminal emulator: replies are written
// on the master exactly as a supporting emulator's protocol reply would be.
// This proves the protocol bytes and the detection state machine; it does not
// claim a real emulator supports OSC 7501.

#include "support/EnvVarGuard.hpp"
#include "support/IoContextRunner.hpp"
#include "support/PiTuiEvidence.hpp"
#include "support/PseudoTerminal.hpp"

#include <cch/tui/ProcessTerminal.hpp>
#include <cch/tui/ProgramStatus.hpp>

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <map>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

cch::tests::IoContextRunner& test_io() {
    static cch::tests::IoContextRunner runner;
    return runner;
}

[[nodiscard]] bool write_all(int descriptor, std::string_view bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto written = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

/// Accumulate terminal output until `needle` appears or the deadline expires.
/// The returned string is everything read, so later assertions can also pin
/// sequence order inside the same read window.
[[nodiscard]] bool wait_for_output(int master, std::string_view needle, std::string& accumulated) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        accumulated += cch::tests::read_available(master, std::chrono::milliseconds{10});
        if (accumulated.find(needle) != std::string::npos) return true;
    }
    return false;
}

/// One frozen detection scenario. Every expected byte string is read from the registered
/// audit-1050 artifact, so an edited observation fails the comparison instead of only
/// invalidating the receipt digest that pins the artifact.
struct DetectionScenario {
    std::string query_write;
    std::vector<std::string> status_writes;
    std::vector<std::string> stop_writes;
};

[[nodiscard]] const DetectionScenario& detection_scenario(std::string_view name) {
    static const auto bundle = [] {
        auto read = cch::tests::read_pi_v130_audit_evidence("audit-1050", "program-status-detection.json");
        REQUIRE(read);
        std::map<std::string, DetectionScenario> scenarios;
        for (const auto& scenario : read->at("scenarios").get_array()) {
            const auto& expected = scenario.at("expected");
            DetectionScenario parsed{
                    .query_write = expected.at("queryWrite").get_string(),
                    .status_writes = {},
                    .stop_writes = {},
            };
            for (const auto& write : expected.at("programStatusWrites").get_array())
                parsed.status_writes.push_back(write.get_string());
            for (const auto& write : expected.at("stopWrites").get_array())
                parsed.stop_writes.push_back(write.get_string());
            scenarios.emplace(scenario.at("name").get_string(), std::move(parsed));
        }
        return scenarios;
    }();
    const auto found = bundle.find(std::string{name});
    REQUIRE(found != bundle.end());
    return found->second;
}

[[nodiscard]] std::string_view startup_query() {
    return detection_scenario("default-reply-before-device-attributes").query_write;
}

[[nodiscard]] std::string_view override_query() { return detection_scenario("override-enabled").query_write; }

[[nodiscard]] std::string_view clear_status_write() {
    return detection_scenario("default-reply-before-device-attributes").stop_writes.at(0);
}

[[nodiscard]] std::string_view working_status_write() {
    return detection_scenario("default-reply-before-device-attributes").status_writes.at(0);
}

[[nodiscard]] cch::tui::ProgramStatus working_status() {
    return cch::tui::ProgramStatus{
            .state = cch::tui::ProgramStatus::State::Working,
            .app = "pi",
    };
}

/// One ProcessTerminal on its own pseudo-terminal, carrying every input byte it
/// forwarded so a reply can be proven consumed.
struct TerminalProbe {
    std::optional<cch::tests::PseudoTerminal> pty;
    std::optional<cch::tui::ProcessTerminal> terminal;
    std::vector<std::string> forwarded_input;

    [[nodiscard]] bool open() {
        pty = cch::tests::open_pseudo_terminal();
        if (!pty) return false;
        terminal.emplace(cch::tui::ProcessTerminalOptions{
                .input_fd = pty->slave.get(), .output_fd = pty->slave.get(), .executor = test_io().io.get_executor()});
        return true;
    }

    [[nodiscard]] cch::support::ExpectedVoid start() {
        return terminal->start(
                [this](std::string sequence) -> cch::support::ExpectedVoid {
                    forwarded_input.push_back(std::move(sequence));
                    return {};
                },
                [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; });
    }

    [[nodiscard]] std::string read() const { return cch::tests::read_available(pty->master.get()); }

    void write(std::string_view bytes) const { REQUIRE(write_all(pty->master.get(), bytes)); }

    [[nodiscard]] bool wait_for(std::string_view needle, std::string& accumulated) const {
        return wait_for_output(pty->master.get(), needle, accumulated);
    }
};

[[nodiscard]] std::string forwarded_bytes(const TerminalProbe& probe) {
    std::string forwarded;
    for (const auto& fragment : probe.forwarded_input)
        forwarded += fragment;
    return forwarded;
}

} // namespace

TEST_CASE("Process Terminal reports the latest status once the terminal answers before DA",
        "[tui][terminal][program-status][issue1123][issue1182][spec]") {
    cch::tests::EnvVarGuard override_guard{"PI_PROGRAM_STATUS"};
    override_guard.unset();
    TerminalProbe probe;
    REQUIRE(probe.open());
    REQUIRE(probe.start());

    // Frozen query order: Kitty query, OSC 7501 query, DA sentinel.
    std::string output = probe.read();
    REQUIRE(probe.wait_for(startup_query(), output));

    // Before support is confirmed nothing is reported.
    REQUIRE(probe.terminal->set_program_status(working_status()));
    CHECK(probe.read().find("7501;state=working") == std::string::npos);

    // The reply answers the query before DA: it is consumed (never forwarded as
    // typed input) and the latest status is reported once.
    probe.write("\x1b]7501;?\x1b\\");
    output.clear();
    REQUIRE(probe.wait_for(working_status_write(), output));
    CHECK(probe.forwarded_input.empty());

    // Later reports are written immediately, including the opened blocked kind.
    probe.write("\x1b[?62;4;52c");
    const auto blocked = cch::tui::ProgramStatus{
            .state = cch::tui::ProgramStatus::State::Blocked,
            .app = "pi",
            .kind = cch::tui::ProgramStatus::Kind::Permission,
            .message = "Allow?",
    };
    REQUIRE(probe.terminal->set_program_status(blocked));
    output.clear();
    REQUIRE(probe.wait_for("msg=QWxsb3c/", output));
    CHECK(output.find("\x1b]7501;state=blocked:app=pi:kind=permission:msg=QWxsb3c/\x1b\\") != std::string::npos);

    // stop() removes the reported status before the ordinary mode restores, so
    // an exited TUI leaves no stale dashboard state. (#1182 records the one
    // frozen stop-write divergence: C++ omits `ESC[>4;0m` when modifyOtherKeys
    // was never active, so the full frozen stop sequence is not yet claimed.)
    REQUIRE(probe.terminal->stop());
    const auto stop_output = probe.read();
    const auto clear_position = stop_output.find(clear_status_write());
    REQUIRE(clear_position != std::string::npos);
    CHECK(stop_output.find("\x1b[?2004l") > clear_position);
}

TEST_CASE("Process Terminal leaves the status unsupported when DA answers before the reply",
        "[tui][terminal][program-status][issue1123][issue1182][spec]") {
    cch::tests::EnvVarGuard override_guard{"PI_PROGRAM_STATUS"};
    override_guard.unset();
    TerminalProbe probe;
    REQUIRE(probe.open());
    REQUIRE(probe.start());
    std::string startup;
    REQUIRE(probe.wait_for(startup_query(), startup));

    REQUIRE(probe.terminal->set_program_status(working_status()));
    // The DA sentinel answers the query without a program-status reply: the
    // terminal is unsupported and the keyboard fallback is enabled.
    probe.write("\x1b[?62;4;52c");
    REQUIRE(cch::tests::wait_until([&] {
        return probe.terminal->capabilities().keyboard_protocol == cch::tui::KeyboardProtocol::ModifyOtherKeys;
    }));

    // A late reply is swallowed and reports nothing, and it never reaches the
    // typed-input stream. The DA sentinel is still forwarded to the Tui's own
    // negotiation stage (the existing decoder contract), so it stays the only
    // fragment; the reply must add no bytes at all.
    probe.write("\x1b]7501;?\x07");
    CHECK(probe.read().find("7501;state=") == std::string::npos);
    CHECK(forwarded_bytes(probe) == "\x1b[?62;4;52c");

    REQUIRE(probe.terminal->stop());
    CHECK(probe.read().find(clear_status_write()) == std::string::npos);
}

TEST_CASE("Process Terminal clears the reported status on the final alternate restore stop",
        "[tui][terminal][program-status][issue1123][spec][exit]") {
    cch::tests::EnvVarGuard override_guard{"PI_PROGRAM_STATUS", std::optional<std::string>{"1"}};
    TerminalProbe probe;
    REQUIRE(probe.open());
    REQUIRE(probe.start());
    std::string output = probe.read();
    REQUIRE(probe.wait_for(override_query(), output));
    REQUIRE(probe.terminal->set_program_status(working_status()));
    output.clear();
    REQUIRE(probe.wait_for(working_status_write(), output));

    // The atomic alternate-restore exit branch must remove the reported status
    // too; the ordinary stop case above cannot discriminate this path.
    REQUIRE(probe.terminal->set_alternate_screen(true));
    REQUIRE(probe.terminal->stop(cch::tui::TerminalStopOptions{.final_alternate_restore = true}));
    const auto stop_output = probe.read();
    CHECK(stop_output.find(clear_status_write()) != std::string::npos);
    CHECK(stop_output.find("\x1b[?1049l") != std::string::npos);
}

TEST_CASE("Process Terminal skips the query and reports immediately with PI_PROGRAM_STATUS=1",
        "[tui][terminal][program-status][issue1123][issue1182][spec][env]") {
    cch::tests::EnvVarGuard override_guard{"PI_PROGRAM_STATUS", std::optional<std::string>{"1"}};
    TerminalProbe probe;
    REQUIRE(probe.open());
    REQUIRE(probe.start());

    std::string output = probe.read();
    REQUIRE(probe.wait_for(override_query(), output));
    CHECK(output.find("7501;?") == std::string::npos);

    REQUIRE(probe.terminal->set_program_status(working_status()));
    output.clear();
    REQUIRE(probe.wait_for(working_status_write(), output));

    REQUIRE(probe.terminal->stop());
    CHECK(probe.read().find(clear_status_write()) != std::string::npos);
}

TEST_CASE("Process Terminal skips the query and reports nothing with PI_PROGRAM_STATUS=0",
        "[tui][terminal][program-status][issue1123][issue1182][spec][env]") {
    cch::tests::EnvVarGuard override_guard{"PI_PROGRAM_STATUS", std::optional<std::string>{"0"}};
    TerminalProbe probe;
    REQUIRE(probe.open());
    REQUIRE(probe.start());

    std::string output = probe.read();
    REQUIRE(probe.wait_for(override_query(), output));
    CHECK(output.find("7501;?") == std::string::npos);

    REQUIRE(probe.terminal->set_program_status(working_status()));
    probe.write("\x1b]7501;?\x1b\\");
    CHECK(probe.read().find("7501;state=") == std::string::npos);

    REQUIRE(probe.terminal->stop());
    CHECK(probe.read().find(clear_status_write()) == std::string::npos);
}

TEST_CASE("Process Terminal keeps the latest status across a restart and re-reports it",
        "[tui][terminal][program-status][issue1123][spec]") {
    cch::tests::EnvVarGuard override_guard{"PI_PROGRAM_STATUS"};
    override_guard.unset();
    TerminalProbe probe;
    REQUIRE(probe.open());
    REQUIRE(probe.start());
    std::string output;
    REQUIRE(probe.wait_for(startup_query(), output));
    REQUIRE(probe.terminal->set_program_status(working_status()));
    probe.write("\x1b]7501;?\x1b\\");
    output.clear();
    REQUIRE(probe.wait_for(working_status_write(), output));

    REQUIRE(probe.terminal->stop());
    CHECK(probe.read().find(clear_status_write()) != std::string::npos);

    REQUIRE(probe.start());
    output.clear();
    REQUIRE(probe.wait_for(startup_query(), output));
    // Stopped: a report is stored without bytes until the restarted terminal
    // confirms support again.
    REQUIRE(probe.terminal->set_program_status(cch::tui::ProgramStatus{
            .state = cch::tui::ProgramStatus::State::Done,
            .app = "pi",
    }));
    CHECK(probe.read().find("7501;state=done") == std::string::npos);

    // The first start's DA arrives late; it must not end the new query. The
    // query reply then confirms support and re-reports the latest status.
    probe.write("\x1b[?62;4;52c");
    probe.write("\x1b]7501;?\x07");
    output.clear();
    REQUIRE(probe.wait_for("\x1b]7501;state=done:app=pi\x1b\\", output));
    // The late DA is the only forwarded fragment; the reply that confirms the
    // restarted query adds no bytes to the typed-input stream.
    CHECK(forwarded_bytes(probe) == "\x1b[?62;4;52c");
}
