#include "coding_agent/tui/EditorAutocomplete.hpp"

#include <cch/tui/Autocomplete.hpp>
#include <cch/support/Error.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

/// Provider double that delivers its result from a detached worker thread —
/// the CombinedAutocompleteProvider fd-completion shape whose delivery must
/// reach the serialized editor domain (#609).
class WorkerThreadAutocompleteProvider final : public cch::tui::AutocompleteProvider {
public:
    void get_suggestions(
            const cch::tui::AutocompleteRequest& request,
            cch::tui::AutocompleteResultSink sink) override {
        requests.push_back(request);
        std::thread worker([sink = std::move(sink)]() mutable {
            (void)sink(cch::tui::AutocompleteSuggestions{
                    .items = {{.value = "src", .label = "src", .description = {}}},
                    .prefix = "@",
            });
        });
        worker.detach();
    }

    [[nodiscard]] cch::tui::AutocompleteApplyResult apply_completion(
            const std::vector<std::string>& lines,
            std::size_t cursor_line,
            std::size_t cursor_column,
            const cch::tui::AutocompleteItem&,
            std::string_view) override {
        return {.lines = lines, .cursor_line = cursor_line, .cursor_column = cursor_column};
    }

    [[nodiscard]] bool should_trigger_file_completion(
            const std::vector<std::string>&, std::size_t, std::size_t) const override {
        return true;
    }

    [[nodiscard]] std::vector<std::string> trigger_characters() const override { return {}; }

    std::vector<cch::tui::AutocompleteRequest> requests;
};

} // namespace

TEST_CASE("Executor-composed autocomplete delivery lands provider results on the serialized executor",
        "[coding_agent][tui][autocomplete][issue609]") {
    boost::asio::io_context io;
    auto work = boost::asio::make_work_guard(io);
    std::optional<std::thread::id> executor_thread;
    std::thread runner([&] {
        executor_thread = std::this_thread::get_id();
        io.run();
    });

    auto inner = std::make_unique<WorkerThreadAutocompleteProvider>();
    auto* inner_pointer = inner.get();
    cch::coding_agent::tui::ExecutorAutocompleteProvider provider(io.get_executor(), std::move(inner));

    const cch::tui::AutocompleteRequest request{
            .lines = {"@src"},
            .cursor_line = 0,
            .cursor_column = 4,
    };
    std::mutex mutex;
    std::vector<std::thread::id> delivery_threads;
    std::optional<cch::tui::AutocompleteSuggestions> delivered;
    provider.get_suggestions(request,
            [&](std::optional<cch::tui::AutocompleteSuggestions> result) -> cch::support::ExpectedVoid {
                std::lock_guard lock(mutex);
                delivery_threads.push_back(std::this_thread::get_id());
                delivered = std::move(result);
                return {};
            });

    // The worker-thread result must reach the sink on the composed executor,
    // not on the worker thread that produced it.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    auto delivered_on_executor = false;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard lock(mutex);
            if (!delivery_threads.empty() && executor_thread && delivery_threads.front() == *executor_thread) {
                delivered_on_executor = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    {
        std::lock_guard lock(mutex);
        REQUIRE(delivered_on_executor);
        REQUIRE(delivered.has_value());
        REQUIRE(delivered->items.size() == 1);
        CHECK(delivered->items.front().value == "src");
        CHECK(delivered->prefix == "@");
    }
    // The inner provider still sees the request unchanged.
    REQUIRE(inner_pointer->requests.size() == 1);
    CHECK(inner_pointer->requests.front().lines == std::vector<std::string>{"@src"});
    CHECK(inner_pointer->requests.front().cursor_column == 4);

    work.reset();
    io.stop();
    runner.join();
}
