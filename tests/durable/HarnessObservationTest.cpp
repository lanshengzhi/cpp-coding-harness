// Discriminating cases for the committed-state observation surface (ticket
// #1088): a reopened Harness derives the conversation view, the task graph,
// and the ready/running/waiting/blocked inspection from committed records
// without running task code and without a second display scheduler.
//
// Every case drives the durable public Harness caller. A counting task
// definition proves the negative: its phase and migrate hooks increment
// shared counters the moment task code executes, so a counter reading of zero
// after observation is a real absence of execution, not a missing hook. The
// initial hook runs at task admission and is the positive control for the
// counter mechanism itself.

#include <cch/ai/Content.hpp>
#include <cch/ai/Models.hpp>
#include <cch/durable/Harness.hpp>
#include <cch/durable/SqliteStorage.hpp>
#include <cch/durable/Task.hpp>

#include "support/TempWorkspace.hpp"
#include "durable/FrozenExpectation.hpp"
#include "durable/GatedStorage.hpp"
#include "durable/Outcome.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace cch;
using namespace cch::durable;
using namespace cch::tests::durable;

namespace {

/// Task-code execution counters shared across Harness reopenings.
struct ExecutionCounters {
    std::atomic_int initial{0};
    std::atomic_int phase{0};
    std::atomic_int migrate{0};
};

/// One task whose hooks count their own executions: the initial hook is the
/// counter positive control (it runs at admission), the phase and migrate
/// hooks are the negative controls for "observation runs no task code".
[[nodiscard]] TaskDefinition counting_task(std::shared_ptr<ExecutionCounters> counters, std::string kind) {
    auto builder = TaskDefinitionBuilder(std::move(kind), 1);
    builder.initial([counters](const chord::Json&) {
        ++counters->initial;
        return chord::Json(chord::Json::Object{{"phase", chord::Json("run")}});
    });
    builder.phase("run", [counters](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        ++counters->phase;
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "completed", .result = chord::Json("ran")});
        });
    });
    builder.migrate([counters](const chord::Json& input, const chord::Json& checkpoint, std::int64_t) {
        ++counters->migrate;
        return MigratedTaskState{.input = input, .checkpoint = checkpoint};
    });
    return std::move(*builder.build());
}

/// A parent task whose run phase creates a child and an owned conversation in
/// one commit, then waits for the child: deterministic waiting states,
/// owner edges, and owned-conversation edges without a model stream. The
/// checkpoint records which run this is; the second run completes.
[[nodiscard]] TaskDefinition waiting_parent_definition(
        std::shared_ptr<const TaskDefinition> child_definition, std::shared_ptr<ExecutionCounters> counters) {
    auto builder = TaskDefinitionBuilder("audit1088.parent", 1);
    builder.initial([counters](const chord::Json&) {
        ++counters->initial;
        return chord::Json(chord::Json::Object{{"phase", chord::Json("run")}});
    });
    builder.phase("run",
            [child_definition](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
                return runtime.commit(
                        [child_definition](Transaction& tx,
                                const TaskRecord& current) -> support::Expected<std::optional<NextTaskState>> {
                            const auto* checkpoint = current.state.checkpoint->as_object();
                            const bool first_run = checkpoint == nullptr
                                    || !checkpoint->contains("waited");
                            chord::Json::Object next_checkpoint;
                            next_checkpoint.emplace("phase", chord::Json("run"));
                            if (!first_run) {
                                return NextTaskState::terminal(
                                        TaskOutcome{.status = "completed", .result = chord::Json("parent")});
                            }
                            next_checkpoint.emplace("waited", chord::Json(true));
                            auto child = tx.create_task(*child_definition,
                                    chord::Json(chord::Json::Object{{"n", chord::Json(1.0)}}),
                                    TaskOptions{.owner = current.id});
                            if (!child) {
                                return std::unexpected(std::move(child.error()));
                            }
                            auto conversation = tx.create_conversation(current.id);
                            if (!conversation) {
                                return std::unexpected(std::move(conversation.error()));
                            }
                            return NextTaskState::waiting(chord::Json(std::move(next_checkpoint)),
                                    std::vector<TaskId>{*child}, JoinPolicy::AllSettled);
                        });
            });
    builder.abort([](const TaskRecord&, TaskRuntime& runtime) {
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "aborted"});
        });
    });
    return std::move(*builder.build());
}

/// Records the frames one ViewWatch delivers.
struct FrameRecorder {
    mutable std::mutex mutex;
    std::vector<ViewFrame> frames{};

    [[nodiscard]] ViewFrameListener listener() {
        return [this](ViewFrame frame) {
            std::lock_guard<std::mutex> lock(mutex);
            frames.push_back(std::move(frame));
        };
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex);
        return frames.size();
    }

    [[nodiscard]] std::vector<ViewFrame> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex);
        return frames;
    }
};

/// Bound every poll: a wait that exceeds the deadline fails the test instead
/// of hanging it.
template <typename Predicate> void wait_until(Predicate predicate, std::string_view what) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) {
            FAIL("timed out waiting for " << what);
        }
        std::this_thread::yield();
    }
}

/// One diagnostic snapshot of the committed inspection: every task with its
/// state, wait edges and owner edge, plus the scheduling status. Bound
/// failures print this so a stuck run shows WHERE it stuck instead of
/// freezing the session (orchestrator rule: every wait is bounded and its
/// timeout names the live state).
[[nodiscard]] std::string inspection_snapshot(Harness* harness) {
    std::ostringstream out;
    auto inspected = consume(harness->inspect());
    if (!inspected.has_value()) {
        out << "inspect failed: " << inspected.error().message;
        return out.str();
    }
    out << "scheduling=" << inspected.value().scheduling;
    for (const TaskInspection& row : inspected.value().tasks) {
        out << "\n  task " << row.record.id.value << " kind=" << row.record.kind
            << " state=" << row.state.kind << " record.status=" << row.record.state.status;
        if (row.record.owner.has_value()) {
            out << " owner=" << row.record.owner->value;
        }
        if (!row.state.on.empty()) {
            out << " on=[";
            for (std::size_t index = 0; index < row.state.on.size(); ++index) {
                if (index != 0) out << ",";
                out << row.state.on[index].value;
            }
            out << "]";
        }
    }
    out << "\n  submissions=" << inspected.value().submissions.size();
    return out.str();
}

/// Bounded wait for one committed task record to reach `"terminal"`. The
/// bound is a test-side liveness guard only: on success the public
/// `wait_for_task` is still exercised afterwards (it returns immediately).
void wait_task_terminal(Harness* harness, TaskId id, std::string_view what) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (;;) {
        auto state = consume(harness->session().read_committed([id](Storage& storage) -> support::Expected<chord::Json> {
            auto task = storage.task(id);
            if (!task) return std::unexpected(task.error());
            return chord::Json(task->has_value() && (*task)->state.status == "terminal");
        }));
        REQUIRE(state.has_value());
        if (const bool* terminal = state.value().as_boolean(); terminal != nullptr && *terminal) return;
        if (std::chrono::steady_clock::now() > deadline) {
            FAIL("timed out waiting for task " << id.value << " to settle: " << what << "\n"
                                               << inspection_snapshot(harness));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

/// Bounded wait until the inspection matches `predicate`; the timeout fails
/// with the diagnostic snapshot instead of hanging.
template <typename Predicate>
void wait_inspection(Harness* harness, Predicate predicate, std::string_view what) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (;;) {
        auto inspected = consume(harness->inspect());
        REQUIRE(inspected.has_value());
        if (predicate(inspected.value())) return;
        if (std::chrono::steady_clock::now() > deadline) {
            FAIL("timed out waiting for " << what << "\n" << inspection_snapshot(harness));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

/// Every frame carries the exact operations that produce it: replaying each
/// frame's ops onto the previous revision reproduces the frame's value. The
/// check runs per frame, not only for the last one, so a frame carrying
/// corrupt middle operations fails here.
void check_exact_frames(const chord::Json& acquisition, const std::vector<ViewFrame>& frames) {
    chord::Json previous = acquisition;
    for (std::size_t index = 0; index < frames.size(); ++index) {
        auto applied = chord::apply_immutable(previous, frames[index].ops);
        INFO("frame " << index);
        REQUIRE(applied.has_value());
        CHECK(*applied == frames[index].value);
        previous = frames[index].value;
    }
}

/// The negative control for `check_exact_frames`: replacing one set operation
/// with a different value must break the replay chain, proving the per-frame
/// assertion discriminates corrupted operations instead of passing whatever
/// it is shown.
void check_tampered_frame_fails(const chord::Json& acquisition, std::vector<ViewFrame> frames) {
    bool tampered = false;
    for (auto& frame : frames) {
        for (auto& op : frame.ops) {
            if (auto* set = std::get_if<chord::SetOp>(&op); set != nullptr) {
                set->value = chord::Json("tampered");
                tampered = true;
                break;
            }
        }
        if (tampered) {
            break;
        }
    }
    REQUIRE(tampered);
    chord::Json previous = acquisition;
    bool mismatch = false;
    for (const ViewFrame& frame : frames) {
        auto applied = chord::apply_immutable(previous, frame.ops);
        if (!applied.has_value() || *applied != frame.value) {
            mismatch = true;
            break;
        }
        previous = frame.value;
    }
    CHECK(mismatch);
}

/// The committed non-terminal task records in committed order (oldest first),
/// the same order inspect derives its live task list in.
[[nodiscard]] std::vector<TaskRecord> committed_live_tasks(Storage& storage) {
    auto scanned = storage.scan_tasks(SIZE_MAX);
    REQUIRE(scanned.has_value());
    std::vector<TaskRecord> live;
    for (const TaskRecord& task : *scanned) {
        if (task.state.status != "terminal") {
            live.push_back(task);
        }
    }
    return live;
}

[[nodiscard]] chord::Json empty_graph_value() {
    return chord::Json(chord::Json::Object{{"tasks", chord::Json(chord::Json::Object{})}});
}

/// The frozen Chord delta wire verb of one operation (frozen pi
/// `assertValidOp`: r/s/d/a/t/p/m).
[[nodiscard]] char op_verb(const chord::Op& op) {
    return std::visit(
            [](const auto& value) -> char {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, chord::ReplaceOp>) return 'r';
                else if constexpr (std::is_same_v<T, chord::SetOp>) return 's';
                else if constexpr (std::is_same_v<T, chord::DeleteOp>) return 'd';
                else if constexpr (std::is_same_v<T, chord::AppendOp>) return 'a';
                else if constexpr (std::is_same_v<T, chord::TruncateOp>) return 't';
                else if constexpr (std::is_same_v<T, chord::SpliceOp>) return 'p';
                else return 'm';
            },
            op);
}

constexpr std::string_view kObservabilityArtifact = "fixtures/pi-v1.3.0/audit-1056/observability.json";

/// One model stream that releases after the test has made its mid-run
/// observations; the spin is deadline-bounded so a failed assertion can never
/// hang a worker forever.
struct LatchedReply {
    std::atomic_bool started{false};
    std::atomic_bool release{false};
    std::string reply{};
};

[[nodiscard]] ai::ModelStream latched_stream(std::shared_ptr<LatchedReply> latch) {
    return ai::ModelStream{ai::ModelStreamProducer{
            [latch](ai::AssistantEventSink sink, ai::ModelStreamCompletion completion) noexcept {
                latch->started.store(true);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                while (!latch->release.load() && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::yield();
                }
                auto message = ai::assistant_text_message(latch->reply);
                auto delivered = sink(ai::AssistantDoneEvent{.reason = message.stop_reason, .message = message});
                if (!delivered)
                    completion(std::unexpected(delivered.error()));
                else
                    completion(std::move(message));
            }}};
}

/// A gate task whose run phase stays running until the test releases it; the
/// dependent task waits for the gate from its first run.
[[nodiscard]] TaskDefinition gate_definition(std::shared_ptr<std::atomic_bool> release,
        std::string kind = "audit.inspect-gate") {
    auto builder = TaskDefinitionBuilder(std::move(kind), 1);
    builder.initial([](const chord::Json&) {
        return chord::Json(chord::Json::Object{{"phase", chord::Json("run")}});
    });
    builder.phase("run", [release](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!release->load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "completed", .result = chord::Json("gate")});
        });
    });
    return std::move(*builder.build());
}

[[nodiscard]] TaskDefinition dependent_definition() {
    auto builder = TaskDefinitionBuilder("audit.inspect-dependent", 1);
    builder.initial([](const chord::Json& input) {
        double gate = 0.0;
        if (const auto* object = input.as_object()) {
            if (const auto found = object->find("gate"); found != object->end() && found->second.as_number()) {
                gate = *found->second.as_number();
            }
        }
        return chord::Json(chord::Json::Object{{"phase", chord::Json("wait")}, {"gate", chord::Json(gate)}});
    });
    builder.phase("wait", [](const TaskRecord& task, TaskRuntime& runtime) -> support::AsyncResult<void> {
        const auto* checkpoint = task.state.checkpoint->as_object();
        const double* gate_number = checkpoint->at("gate").as_number();
        REQUIRE(gate_number != nullptr);
        const TaskId gate{static_cast<std::int64_t>(*gate_number)};
        return runtime.commit([gate](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            // The wait parks on the gate; when the gate settles the scheduler
            // resumes at the checkpoint phase recorded here.
            return NextTaskState::waiting(
                    chord::Json(chord::Json::Object{
                            {"phase", chord::Json("resume")}, {"gate", chord::Json(static_cast<double>(gate.value))}}),
                    std::vector<TaskId>{gate}, JoinPolicy::AllSettled);
        });
    });
    builder.phase("resume", [](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "completed", .result = chord::Json("dependent")});
        });
    });
    return std::move(*builder.build());
}

/// The text of one assistant entry's first text block, or nullopt when the
/// entry carries no text content.
[[nodiscard]] std::optional<std::string> entry_text(const chord::Json& entry_json) {
    const auto* object = entry_json.as_object();
    if (object == nullptr) return std::nullopt;
    const auto* data = object->at("data").as_object();
    if (data == nullptr) return std::nullopt;
    const auto* model = data->at("model").as_array();
    if (model == nullptr || model->empty()) return std::nullopt;
    const auto* message = (*model)[0].as_object();
    if (message == nullptr) return std::nullopt;
    const auto* content = message->at("content").as_array();
    if (content == nullptr) return std::nullopt;
    for (const auto& block : *content) {
        if (const auto* block_object = block.as_object(); block_object != nullptr) {
            if (const auto* type = block_object->at("type").as_string(); type != nullptr && *type == "text") {
                if (const auto* text = block_object->at("text").as_string()) return *text;
            }
        }
    }
    return std::nullopt;
}

} // namespace

// Discriminating case (ticket #1088): after a durable reopen, calling only
// the observation APIs — inspect, task graph, conversation view, and the
// watch acquisitions — reproduces the committed records exactly while every
// task-code execution counter stays at zero. The initial-admission counter is
// the positive control: it proves the counters work and that task code ran
// before the close; the zero phase and migrate counters after reopening prove
// observation derived the views without executing task code.
TEST_CASE("a reopened Harness derives every observation from committed records without running task code",
        "[durable][observation][inspect][issue1088]") {
    tests::TempWorkspace workspace;
    const std::string file = (workspace.path() / "observation.sqlite").string();
    auto counters = std::make_shared<ExecutionCounters>();

    TaskId parent_id{};
    TaskId child_id{};
    EntryId first_entry_id{};
    {
        auto opened_storage = SqliteStorage::open(file);
        REQUIRE(opened_storage.has_value());
        std::unique_ptr<Storage> storage = std::move(opened_storage.value());
        std::vector<TaskDefinition> tasks;
        tasks.push_back(counting_task(counters, "audit1088.witness"));
        auto opened = consume(Harness::open(*storage, HarnessOptions{.tasks = std::move(tasks)}));
        REQUIRE(opened.has_value());
        auto harness = std::move(opened.value());
        const auto root = consume(harness->root());
        REQUIRE(root.has_value());
        REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
            const TaskDefinition* definition = harness->task_definition("audit1088.witness");
            if (definition == nullptr) {
                return std::unexpected(support::make_error(support::ErrorCode::Validation, "missing definition"));
            }
            auto parent = tx.create_task(*definition,
                    chord::Json(chord::Json::Object{{"n", chord::Json(1.0)}}), TaskOptions{.conversation_id = root.value()});
            if (!parent) return std::unexpected(parent.error());
            parent_id = *parent;
            auto child = tx.create_task(*definition,
                    chord::Json(chord::Json::Object{{"n", chord::Json(2.0)}}), TaskOptions{.owner = parent_id});
            if (!child) return std::unexpected(child.error());
            child_id = *child;
            auto entry = tx.append_entry(
                    root.value(), "pi.user", chord::Json(chord::Json::Object{{"text", chord::Json("hello")}}));
            if (!entry) return std::unexpected(entry.error());
            first_entry_id = entry->id;
            return {};
        })).has_value());
        // A committed document change the reopened view must reflect.
        auto agent_doc = detail::builtin_document("pi.agent:v1", DocumentHistory::Rewindable, DocumentFork::AsOf,
                [] { return chord::Json(); }, [](const chord::Json&, const chord::OpBatch&, std::int64_t) {
                    return true;
                });
        auto doc_commit = consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
            auto draft = tx.doc(agent_doc, DocumentAddress{.conversation_id = root.value()});
            if (!draft) return std::unexpected(std::move(draft.error()));
            if (auto set = draft->set(chord::Path{chord::path_key("observed")}, chord::Json(true)); !set) {
                return std::unexpected(std::move(set.error()));
            }
            return {};
        }));
        {
            INFO(std::string{"doc commit error: "} + (doc_commit.has_value() ? std::string{} : doc_commit.error().message));
            REQUIRE(doc_commit.has_value());
        }
        // Positive control: task admission ran the initial hook twice...
        REQUIRE(counters->initial.load() == 2);
        // ...and no phase or migrate hook has run yet.
        REQUIRE(counters->phase.load() == 0);
        REQUIRE(counters->migrate.load() == 0);
        REQUIRE(consume(harness->close()).has_value());
        REQUIRE(storage->close().has_value());
    }

    // With the definitions installed the committed records inspect as their
    // ready states; without them the same records inspect as blocked.
    // Observation must not care either way: it never consults the definitions.
    std::vector<TaskDefinition> reopened_tasks;
    const auto definitions_installed = GENERATE(true, false);
    if (definitions_installed) {
        reopened_tasks.push_back(counting_task(counters, "audit1088.witness"));
    }

    auto reopened_storage = SqliteStorage::open(file);
    REQUIRE(reopened_storage.has_value());
    std::unique_ptr<Storage> storage = std::move(reopened_storage.value());
    auto reopened = consume(Harness::open(*storage, HarnessOptions{.tasks = std::move(reopened_tasks)}));
    REQUIRE(reopened.has_value());
    auto harness = std::move(reopened.value());
    REQUIRE(consume(harness->open_tasks()).has_value());
    const auto root = consume(harness->root());
    REQUIRE(root.has_value());

    // The reopened Harness called only observation APIs so far: no task code
    // has run. The counters are unchanged since before the close.
    CHECK(counters->phase.load() == 0);
    CHECK(counters->migrate.load() == 0);

    // inspect() derives the live task list and unsettled submissions from the
    // committed records alone, line by line.
    const auto expected_tasks = committed_live_tasks(*storage);
    REQUIRE(expected_tasks.size() == 2);
    const auto inspection = consume(harness->inspect());
    REQUIRE(inspection.has_value());
    CHECK(inspection.value().scheduling == "paused");
    REQUIRE(inspection.value().tasks.size() == expected_tasks.size());
    CHECK(inspection.value().submissions.empty());
    for (std::size_t index = 0; index < expected_tasks.size(); ++index) {
        INFO("task " << index);
        CHECK(inspection.value().tasks[index].record == expected_tasks[index]);
        // The derived state is the committed one: ready while pending, blocked
        // when the definition is missing. Observation consults no definition.
        CHECK(inspection.value().tasks[index].state.kind
                == (definitions_installed ? std::string("ready") : std::string("blocked")));
        if (!definitions_installed) {
            CHECK(inspection.value().tasks[index].state.reason == "missing_task");
        }
    }
    // The owner edge is the committed one.
    CHECK(inspection.value().tasks[1].record.owner == parent_id);

    // Observation is side-effect free: reading it again yields the same
    // inspection and still runs no task code. (`HarnessInspection` carries an
    // `Error`, so it compares field by field.)
    const auto reread = consume(harness->inspect());
    REQUIRE(reread.has_value());
    CHECK(reread.value().scheduling == inspection.value().scheduling);
    CHECK(reread.value().submissions == inspection.value().submissions);
    REQUIRE(reread.value().tasks.size() == inspection.value().tasks.size());
    for (std::size_t index = 0; index < reread.value().tasks.size(); ++index) {
        CHECK(reread.value().tasks[index].record == inspection.value().tasks[index].record);
        CHECK(reread.value().tasks[index].state.kind == inspection.value().tasks[index].state.kind);
    }
    CHECK(counters->phase.load() == 0);
    CHECK(counters->migrate.load() == 0);

    // task_graph() derives the same records as structured nodes.
    const auto graph = consume(harness->task_graph());
    REQUIRE(graph.has_value());
    REQUIRE(graph.value().tasks.size() == 2);
    CHECK(graph.value().tasks[0].id == parent_id);
    CHECK(graph.value().tasks[1].id == child_id);
    CHECK(graph.value().tasks[1].owner == parent_id);
    CHECK(graph.value().tasks[0].state.status == "pending");
    CHECK(graph.value().tasks[1].state.status == "pending");
    CHECK(graph.value().tasks[0].state.phase == "run");
    CHECK(graph.value().tasks[1].state.phase == "run");
    CHECK(graph.value().tasks[0].conversation_id == root.value());
    CHECK(graph.value().tasks[1].conversation_id == root.value());

    // conversation_view() derives the conversation record, the visible
    // entries, and the five built-in documents from committed state.
    const auto view = consume(harness->conversation_view(root.value()));
    REQUIRE(view.has_value());
    const auto* view_object = view.value().as_object();
    REQUIRE(view_object != nullptr);
    CHECK(*view_object->at("conversation").as_object()->at("id").as_number()
            == static_cast<double>(root.value().value));
    const auto* entries = view_object->at("entries").as_array();
    REQUIRE(entries != nullptr);
    REQUIRE(entries->size() == 1);
    CHECK(*(*entries)[0].as_object()->at("id").as_number() == static_cast<double>(first_entry_id.value));
    const auto* docs = view_object->at("docs").as_object();
    REQUIRE(docs != nullptr);
    for (const char* kind : {"pi.agent", "pi.inbox", "pi.live", "pi.provider", "pi.usage"}) {
        INFO(kind);
        CHECK(docs->contains(kind));
    }
    CHECK(*docs->at("pi.agent").as_object()->at("observed").as_boolean());

    // Watch acquisitions observe the same committed state and, with no later
    // commit, deliver no frames: nothing fabricates activity.
    auto graph_recorder = std::make_shared<FrameRecorder>();
    auto graph_watch = consume(harness->watch_task_graph(graph_recorder->listener()));
    REQUIRE(graph_watch.has_value());
    CHECK(graph_recorder->size() == 0);
    const chord::Json graph_acquisition = graph_watch.value().value();
    CHECK(graph_acquisition != empty_graph_value());
    auto view_recorder = std::make_shared<FrameRecorder>();
    auto view_watch = consume(harness->watch_conversation(root.value(), view_recorder->listener()));
    REQUIRE(view_watch.has_value());
    CHECK(view_recorder->size() == 0);
    CHECK(view_watch.value().value() == view.value());
    CHECK(counters->phase.load() == 0);
    CHECK(counters->migrate.load() == 0);

    if (definitions_installed) {
        // The positive control for the negative claim: once scheduling
        // resumes, task code runs and the tasks settle — proving the earlier
        // zero reading meant "not executed", not "not wired". The child is
        // owned by the parent, so it settles by running or by orphan
        // reconciliation; either way exactly its terminal commit disappears
        // from the graph.
        REQUIRE(harness->resume().has_value());
        wait_task_terminal(harness.get(), parent_id, "reopened parent settle");
        const auto receipt = consume(harness->wait_for_task(parent_id));
        REQUIRE(receipt.has_value());
        CHECK(receipt.value().state.status == "terminal");
        REQUIRE(receipt.value().state.outcome.has_value());
        wait_task_terminal(harness.get(), child_id, "reopened child settle");
        const auto child_receipt = consume(harness->wait_for_task(child_id));
        REQUIRE(child_receipt.has_value());
        CHECK(child_receipt.value().state.status == "terminal");
        CHECK(counters->phase.load() >= 1);

        // After the runs settle, the same observations show an idle harness.
        const auto settled = consume(harness->inspect());
        REQUIRE(settled.has_value());
        CHECK(settled.value().tasks.empty());
        const auto settled_graph = consume(harness->task_graph());
        REQUIRE(settled_graph.has_value());
        CHECK(settled_graph.value().tasks.empty());

        // The task-graph watcher saw the run commits; every frame replays
        // exactly and the delivered value ends equal to a fresh committed
        // read. The conversation watcher saw none of it: task commits do not
        // touch the root conversation.
        wait_until([&] { return graph_recorder->size() >= 3; }, "task-graph frames");
        const auto frames = graph_recorder->snapshot();
        check_exact_frames(graph_acquisition, frames);
        check_tampered_frame_fails(graph_acquisition, frames);
        CHECK(frames.back().value == empty_graph_value());
        CHECK(view_recorder->size() == 0);
        CHECK(view_watch.value().value() == view.value());
    }

    // Closing the Session ends the live watches as SessionClosed.
    REQUIRE(consume(harness->close()).has_value());
    CHECK(graph_watch.value().stop() == WatchEndReason::SessionClosed);
    CHECK(view_watch.value().stop() == WatchEndReason::SessionClosed);
    REQUIRE(storage->close().has_value());
}

// Discriminating case (ticket #1088): the task-graph watch delivers exactly
// one frame per touching commit — a waiting transition with its wait targets
// and join policy, an owner edge, an owned-conversation edge, and a node
// deletion on every terminal settle — and cancelling one subscription changes
// no runtime state while the other watch keeps receiving.
TEST_CASE("a task-graph watch delivers exact frames and a cancelled subscription changes no runtime state",
        "[durable][observation][task-graph][issue1088]") {
    auto counters = std::make_shared<ExecutionCounters>();
    MemoryStorage storage;

    std::vector<TaskDefinition> definitions;
    auto child_definition = std::make_shared<TaskDefinition>(counting_task(counters, "audit1088.child"));
    definitions.push_back(counting_task(counters, "audit1088.child"));
    definitions.push_back(waiting_parent_definition(child_definition, counters));

    auto opened = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(definitions)}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    const auto root = consume(harness->root());
    REQUIRE(root.has_value());

    // Both watches acquire the empty committed graph.
    auto first_recorder = std::make_shared<FrameRecorder>();
    auto first = consume(harness->watch_task_graph(first_recorder->listener()));
    REQUIRE(first.has_value());
    auto second_recorder = std::make_shared<FrameRecorder>();
    auto second = consume(harness->watch_task_graph(second_recorder->listener()));
    REQUIRE(second.has_value());
    const chord::Json acquisition = empty_graph_value();
    CHECK(first.value().value() == acquisition);
    CHECK(second.value().value() == acquisition);

    // One commit creates the parent: exactly one frame.
    TaskId parent_id{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
        const TaskDefinition* definition = harness->task_definition("audit1088.parent");
        if (definition == nullptr) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation, "missing parent definition"));
        }
        auto parent = tx.create_task(*definition, chord::Json(chord::Json::Object{{"n", chord::Json(0.0)}}),
                TaskOptions{.conversation_id = root.value()});
        if (!parent) return std::unexpected(parent.error());
        parent_id = *parent;
        return {};
    })).has_value());
    REQUIRE(first_recorder->size() == 1);
    REQUIRE(second_recorder->size() == 1);

    // Scheduling the parent produces the deterministic transition chain:
    // initial run, one phase commit that creates the child and the owned
    // conversation and enters the wait, the child's run and settle, the wait
    // resolution, and the parent's second run and settle.
    REQUIRE(harness->resume().has_value());
    wait_task_terminal(harness.get(), parent_id, "task-graph parent settle");
    const auto receipt = consume(harness->wait_for_task(parent_id));
    REQUIRE(receipt.has_value());
    CHECK(receipt.value().state.status == "terminal");
    REQUIRE(receipt.value().state.outcome.has_value());
    CHECK(receipt.value().state.outcome->status == "completed");

    // Every touching commit produces a frame on each watch in order.
    wait_until([&] { return first_recorder->size() >= 7; }, "task-graph frames");
    const auto frames = first_recorder->snapshot();
    REQUIRE(frames.size() >= 7);
    CHECK(second_recorder->size() == frames.size());
    check_exact_frames(acquisition, frames);
    check_tampered_frame_fails(acquisition, frames);

    // The wait frame carries the exact operations of the transition: the
    // parent's waiting state with its wait target and join policy, the new
    // child's owner edge, and the parent's owned-conversation edge.
    const auto* waited_tasks = frames[2].value.as_object()->at("tasks").as_object();
    const auto* parent_node = waited_tasks->at(std::to_string(parent_id.value)).as_object();
    const auto* parent_state = parent_node->at("state").as_object();
    CHECK(*parent_state->at("status").as_string() == "waiting");
    CHECK(*parent_state->at("policy").as_string() == "allSettled");
    const auto* on = parent_state->at("on").as_array();
    REQUIRE(on->size() == 1);
    const double* on_id = (*on)[0].as_number();
    REQUIRE(on_id != nullptr);
    const TaskId child_id{static_cast<std::int64_t>(*on_id)};
    const auto* child_node = waited_tasks->at(std::to_string(child_id.value)).as_object();
    CHECK(*child_node->at("owner").as_number() == static_cast<double>(parent_id.value));
    const auto* conversations = parent_node->at("conversations").as_array();
    REQUIRE(conversations->size() == 1);
    const double* owned_id = (*conversations)[0].as_number();
    REQUIRE(owned_id != nullptr);
    // The owned conversation is the committed record the same commit created.
    auto owned_conversation = storage.conversation(ConversationId{static_cast<std::int64_t>(*owned_id)});
    REQUIRE(owned_conversation.has_value());
    REQUIRE(owned_conversation.value().has_value());
    CHECK(owned_conversation.value().value().owner->task_id == parent_id);
    REQUIRE(frames[2].ops.size() == 3);
    const auto* deleted_tasks = frames.back().value.as_object()->at("tasks").as_object();
    CHECK(deleted_tasks->empty());

    // Cancel the second subscription: the run is unaffected and the first
    // watch keeps receiving exactly the later commits.
    CHECK(second.value().stop() == WatchEndReason::Stopped);
    TaskId later_id{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
        auto later = tx.create_task(*child_definition, chord::Json(chord::Json::Object{{"n", chord::Json(9.0)}}),
                TaskOptions{.conversation_id = root.value()});
        if (!later) return std::unexpected(later.error());
        later_id = *later;
        return {};
    })).has_value());
    REQUIRE(harness->resume().has_value());
    wait_task_terminal(harness.get(), later_id, "post-cancellation task settle");
    const auto later_receipt = consume(harness->wait_for_task(later_id));
    REQUIRE(later_receipt.has_value());
    CHECK(later_receipt.value().state.status == "terminal");

    wait_until([&] { return first_recorder->size() >= frames.size() + 3; }, "later task-graph frames");
    CHECK(second_recorder->size() == frames.size());
    const auto all = first_recorder->snapshot();
    REQUIRE(all.size() >= frames.size() + 3);
    check_exact_frames(acquisition, all);
    // The final delivered value equals a fresh committed read.
    const auto fresh = consume(harness->task_graph());
    REQUIRE(fresh.has_value());
    CHECK(fresh.value().tasks.empty());
    CHECK(all.back().value == empty_graph_value());
    // The cancelled watch kept its acquisition revision.
    CHECK(second.value().value() == frames.back().value);

    CHECK(counters->phase.load() == 2);
    REQUIRE(consume(harness->close()).has_value());
    CHECK(first.value().stop() == WatchEndReason::SessionClosed);
    CHECK(second.value().stop() == WatchEndReason::Stopped);
}

// Discriminating case (ticket #1088): the conversation view watch delivers
// exactly one frame per touching commit — a document-change operation batch
// prefixed into the docs entry, an entry splice, a head-marker splice that
// cuts the retained prefix — while a rolled-back transaction publishes
// nothing and a cancelled subscription changes no committed or runtime
// state.
TEST_CASE("a conversation view watch delivers exact frames, rolls back invisibly, and survives cancellation",
        "[durable][observation][conversation-view][issue1088]") {
    MemoryStorage storage;
    auto opened = consume(Harness::open(storage, HarnessOptions{}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    const auto root = consume(harness->root());
    REQUIRE(root.has_value());

    // Same semantics as the harness's built-in agent document so the draft
    // matches the stored incarnation (Rewindable/AsOf, always checkpoint).
    auto agent_doc = detail::builtin_document("pi.agent:v1", DocumentHistory::Rewindable, DocumentFork::AsOf,
            [] { return chord::Json(); }, [](const chord::Json&, const chord::OpBatch&, std::int64_t) {
                return true;
            });

    auto first_recorder = std::make_shared<FrameRecorder>();
    auto first = consume(harness->watch_conversation(root.value(), first_recorder->listener()));
    REQUIRE(first.has_value());
    auto second_recorder = std::make_shared<FrameRecorder>();
    auto second = consume(harness->watch_conversation(root.value(), second_recorder->listener()));
    REQUIRE(second.has_value());

    const auto hydrated = consume(harness->conversation_view(root.value()));
    REQUIRE(hydrated.has_value());
    const chord::Json acquisition = first.value().value();
    CHECK(acquisition == hydrated.value());
    CHECK(second.value().value() == hydrated.value());

    // Commit 1: append one entry.
    EntryId first_entry_id{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
        auto entry = tx.append_entry(
                root.value(), "pi.user", chord::Json(chord::Json::Object{{"text", chord::Json("one")}}));
        if (!entry) return std::unexpected(entry.error());
        first_entry_id = entry->id;
        return {};
    })).has_value());

    // Commit 2: change the built-in agent document.
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
        auto draft = tx.doc(agent_doc, DocumentAddress{.conversation_id = root.value()});
        if (!draft) return std::unexpected(std::move(draft.error()));
        if (auto set = draft->set(chord::Path{chord::path_key("model")}, chord::Json("witness")); !set) {
            return std::unexpected(std::move(set.error()));
        }
        return {};
    })).has_value());

    // Commit 3: append a head entry cutting the retained prefix.
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
        auto entry = tx.append_entry(root.value(), "pi.user",
                chord::Json(chord::Json::Object{{"text", chord::Json("two")}}), EntryHeadVariant{first_entry_id});
        if (!entry) return std::unexpected(entry.error());
        return {};
    })).has_value());

    // Commit 4: roll back after staging a write; the transaction rejects and
    // publishes nothing.
    const auto rolled_back = consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
        auto entry = tx.append_entry(
                root.value(), "pi.user", chord::Json(chord::Json::Object{{"text", chord::Json("ghost")}}));
        if (!entry) return std::unexpected(entry.error());
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "roll back on purpose"));
    }));
    REQUIRE_FALSE(rolled_back.has_value());

    // Three touching commits, three frames; the rollback is invisible.
    REQUIRE(first_recorder->size() == 3);
    REQUIRE(second_recorder->size() == 3);
    const auto frames = first_recorder->snapshot();
    check_exact_frames(acquisition, frames);
    check_tampered_frame_fails(acquisition, frames);

    // Frame 1 splices the entry in at the end.
    REQUIRE(frames[0].ops.size() == 1);
    const auto* splice = std::get_if<chord::SpliceOp>(&frames[0].ops.front());
    REQUIRE(splice != nullptr);
    CHECK(splice->index == 0);
    CHECK(splice->items.size() == 1);
    // Frame 2 prefixes the document operation into the pi.agent:v1 docs
    // entry.
    REQUIRE(frames[1].ops.size() == 1);
    const auto* doc_set = std::get_if<chord::SetOp>(&frames[1].ops.front());
    REQUIRE(doc_set != nullptr);
    REQUIRE(doc_set->path.size() == 3);
    CHECK(std::get<chord::PathKey>(doc_set->path[0]).key == "docs");
    CHECK(std::get<chord::PathKey>(doc_set->path[1]).key == "pi.agent");
    CHECK(std::get<chord::PathKey>(doc_set->path[2]).key == "model");
    // The head marker is inserted before the retained non-head suffix.
    REQUIRE(frames[2].ops.size() == 1);
    const auto* head_splice = std::get_if<chord::SpliceOp>(&frames[2].ops.front());
    REQUIRE(head_splice != nullptr);
    CHECK(head_splice->index == 0);
    const auto* delivered_entries = frames[2].value.as_object()->at("entries").as_array();
    REQUIRE(delivered_entries->size() == 2);
    CHECK(delivered_entries->front().as_object()->contains("head"));

    // The delivered value replays exactly and the document part equals a
    // fresh committed read; entries follow the live view's retained-prefix
    // rule (a fresh hydration cuts at the head marker, the advance view keeps
    // what was retained when the head entry arrived).
    const auto fresh = consume(harness->conversation_view(root.value()));
    REQUIRE(fresh.has_value());
    CHECK(fresh.value() == frames[2].value);

    // Cancel the second subscription: later commits still settle, the first
    // watch keeps receiving exactly one frame per touching commit, and the
    // cancelled watch keeps its last delivered revision.
    CHECK(second.value().stop() == WatchEndReason::Stopped);
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
        auto entry = tx.append_entry(
                root.value(), "pi.user", chord::Json(chord::Json::Object{{"text", chord::Json("three")}}));
        if (!entry) return std::unexpected(entry.error());
        return {};
    })).has_value());
    REQUIRE(first_recorder->size() == 4);
    CHECK(second_recorder->size() == 3);
    const auto all = first_recorder->snapshot();
    check_exact_frames(acquisition, all);
    const auto* final_entries = all[3].value.as_object()->at("entries").as_array();
    CHECK(final_entries->size() == 3);
    CHECK(second.value().value() == frames[2].value);

    // Missing conversations are rejected by the same committed read.
    const auto missing = consume(harness->conversation_view(ConversationId{.value = 9999}));
    REQUIRE_FALSE(missing.has_value());

    REQUIRE(consume(harness->close()).has_value());
    CHECK(first.value().stop() == WatchEndReason::SessionClosed);
    CHECK(second.value().stop() == WatchEndReason::Stopped);
}

TEST_CASE("stopping or replacing a watch prevents callbacks that have not started",
        "[durable][observation][issue1088]") {
    MemoryStorage storage;
    auto opened = consume(Harness::open(storage, HarnessOptions{}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    const auto root = consume(harness->root());
    REQUIRE(root.has_value());

    ViewWatch* second_watch = nullptr;
    int first_calls = 0;
    int second_calls = 0;
    auto first = consume(harness->watch_conversation(root.value(), [&](ViewFrame) {
        ++first_calls;
        if (second_watch != nullptr) static_cast<void>(second_watch->stop());
    }));
    REQUIRE(first.has_value());
    auto second = consume(harness->watch_conversation(root.value(), [&](ViewFrame) { ++second_calls; }));
    REQUIRE(second.has_value());
    second_watch = &second.value();
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
        auto entry = tx.append_entry(root.value(), "pi.user", chord::Json(chord::Json::Object{}));
        if (!entry) return std::unexpected(entry.error());
        return {};
    })).has_value());
    CHECK(first_calls == 1);
    CHECK(second_calls == 0);
    CHECK(second.value().stop() == WatchEndReason::Stopped);

    int replaced_calls = 0;
    int replacement_calls = 0;
    auto target = consume(harness->watch_conversation(root.value(), [&](ViewFrame) { ++replaced_calls; }));
    auto replacement = consume(harness->watch_conversation(root.value(), [&](ViewFrame) { ++replacement_calls; }));
    REQUIRE(target.has_value());
    REQUIRE(replacement.has_value());
    target.value() = std::move(replacement.value());
    second_watch = nullptr;
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
        auto entry = tx.append_entry(root.value(), "pi.user", chord::Json(chord::Json::Object{}));
        if (!entry) return std::unexpected(entry.error());
        return {};
    })).has_value());
    CHECK(replaced_calls == 0);
    CHECK(replacement_calls == 1);
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("committed reads wait for earlier admitted commits", "[durable][observation][issue1088][read-order]") {
    cch::tests::GatedStorage storage;
    auto session = Session::create(storage);
    storage.hold_next_commit();
    std::optional<support::Expected<void>> committed;
    auto pending = session->commit([](Transaction& tx) -> support::Expected<void> {
        auto conversation = tx.create_conversation();
        if (!conversation) return std::unexpected(conversation.error());
        return {};
    });
    std::move(pending).start([&](support::Expected<void> result) noexcept { committed = std::move(result); });
    REQUIRE(storage.holds_commit());
    std::mutex read_mutex;
    std::optional<support::Expected<chord::Json>> read;
    std::atomic_bool read_started{false};
    std::atomic_bool read_done{false};
    std::jthread reader([&] {
        read_started.store(true);
        auto operation = session->read_committed([](Storage& store) -> support::Expected<chord::Json> {
            auto conversations = store.scan_conversations(100);
            if (!conversations) return std::unexpected(conversations.error());
            return chord::Json(static_cast<double>(conversations->size()));
        });
        std::move(operation).start([&](support::Expected<chord::Json> result) noexcept {
            std::lock_guard lock(read_mutex);
            read = std::move(result);
            read_done.store(true);
        });
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!read_done.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    const bool started = read_started.load();
    const bool read_before_commit = read_done.load();
    double early_count = -1.0;
    {
        std::lock_guard lock(read_mutex);
        if (read && *read && (*read)->as_number()) early_count = *(*read)->as_number();
    }
    storage.release_held_commit();
    reader.join();
    const auto completion_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!read_done.load() && std::chrono::steady_clock::now() < completion_deadline) std::this_thread::yield();
    REQUIRE(started);
    REQUIRE(read_done.load());
    REQUIRE(committed.has_value());
    REQUIRE(committed->has_value());
    REQUIRE(read.has_value());
    REQUIRE(read->has_value());
    INFO("read completed before storage settlement: " << read_before_commit << ", observed count: " << early_count);
    CHECK_FALSE(read_before_commit);
    REQUIRE((*read)->as_number() != nullptr);
    CHECK(*(*read)->as_number() == 1.0);
    REQUIRE(consume(session->close()).has_value());
}

TEST_CASE("inspection does not combine an uncommitted reservation with a pending task",
        "[durable][observation][issue1088][reservation-probe]") {
    MemoryStorage storage;
    auto counters = std::make_shared<ExecutionCounters>();
    std::vector<TaskDefinition> definitions;
    definitions.push_back(counting_task(counters, "audit1088.reservation"));
    auto opened = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(definitions)}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    const auto root = consume(harness->root());
    REQUIRE(root.has_value());
    TaskId task_id{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
        const auto* definition = harness->task_definition("audit1088.reservation");
        if (!definition) return std::unexpected(support::make_error(support::ErrorCode::Validation, "missing definition"));
        auto task = tx.create_task(*definition, chord::Json(chord::Json::Object{}),
                TaskOptions{.conversation_id = root.value()});
        if (!task) return std::unexpected(task.error());
        task_id = *task;
        return {};
    })).has_value());

    struct Gate {
        std::atomic_bool entered{false};
        std::atomic_bool release{false};
        std::atomic_bool probe_done{false};
        std::atomic_bool inconsistent{false};
    };
    auto gate = std::make_shared<Gate>();
    std::jthread holder([&, gate] {
        static_cast<void>(consume(harness->session().read_committed([gate](Storage&) -> support::Expected<chord::Json> {
            gate->entered.store(true);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (!gate->release.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
            return chord::Json(nullptr);
        })));
    });
    const auto entered_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!gate->entered.load() && std::chrono::steady_clock::now() < entered_deadline) std::this_thread::yield();
    const bool entered = gate->entered.load();
    std::jthread probe([&, gate] {
        if (entered && harness->resume()) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!gate->release.load() && std::chrono::steady_clock::now() < deadline) {
                auto inspection = consume(harness->inspect());
                auto record = storage.task(task_id);
                if (inspection.has_value() && record && record->has_value()) {
                    for (const auto& row : inspection.value().tasks) {
                        if (row.record.id == task_id && row.record.state.status == "pending" &&
                                row.state.kind == "running" && (*record)->state.status == "pending") {
                            gate->inconsistent.store(true);
                        }
                    }
                }
                if (gate->inconsistent.load()) break;
                std::this_thread::yield();
            }
        }
        gate->probe_done.store(true);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(7);
    while (!gate->probe_done.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    const bool probe_completed_before_release = gate->probe_done.load();
    gate->release.store(true);
    holder.join();
    probe.join();
    REQUIRE(entered);
    INFO("probe completed before release: " << probe_completed_before_release);
    CHECK_FALSE(gate->inconsistent.load());
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("a watch callback already running may finish after stop", "[durable][observation][issue1088]") {
    MemoryStorage storage;
    auto opened = consume(Harness::open(storage, HarnessOptions{}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    const auto root = consume(harness->root());
    REQUIRE(root.has_value());
    std::atomic_bool started{false};
    std::atomic_bool release{false};
    std::atomic_bool finished{false};
    auto watch = consume(harness->watch_conversation(root.value(), [&](ViewFrame) {
        started.store(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!release.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        finished.store(true);
    }));
    REQUIRE(watch.has_value());
    std::jthread writer([&] {
        static_cast<void>(consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
            auto entry = tx.append_entry(root.value(), "pi.user", chord::Json(chord::Json::Object{}));
            if (!entry) return std::unexpected(entry.error());
            return {};
        })));
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!started.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    const bool began = started.load();
    if (began) CHECK(watch.value().stop() == WatchEndReason::Stopped);
    release.store(true);
    writer.join();
    REQUIRE(began);
    CHECK(finished.load());
    REQUIRE(consume(harness->close()).has_value());
}

// Exercise the observation seam with real committed mutations shaped like the
// frozen generation transitions. This does not claim the generation producer
// itself emits those transitions; that integration remains blocked by #1163.
TEST_CASE("conversation watch observes committed generation and usage changes",
        "[durable][observation][issue1088]") {
    MemoryStorage storage;
    auto opened = consume(Harness::open(storage, HarnessOptions{}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    const auto root = consume(harness->root());
    REQUIRE(root.has_value());
    auto live = detail::builtin_document("pi.live:v1", DocumentHistory::Latest, DocumentFork::Initial,
            [] { return chord::Json(chord::Json::Object{}); },
            [](const chord::Json&, const chord::OpBatch&, std::int64_t) { return true; });
    auto usage = detail::builtin_document("pi.usage:v1", DocumentHistory::Latest, DocumentFork::Initial,
            [] { return chord::Json(chord::Json::Object{}); },
            [](const chord::Json&, const chord::OpBatch&, std::int64_t) { return true; });
    auto recorder = std::make_shared<FrameRecorder>();
    auto watch = consume(harness->watch_conversation(root.value(), recorder->listener()));
    REQUIRE(watch.has_value());
    const auto initial = watch.value().value();
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
        auto doc = tx.doc(live, DocumentAddress{.conversation_id = root.value()});
        if (!doc) return std::unexpected(doc.error());
        auto set = doc->set({chord::path_key("generation")},
                chord::Json(chord::Json::Object{{"attempt", chord::Json(1.0)}}));
        if (!set) return std::unexpected(set.error());
        return {};
    })).has_value());
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
        auto doc = tx.doc(live, DocumentAddress{.conversation_id = root.value()});
        if (!doc) return std::unexpected(doc.error());
        auto removed = doc->erase({chord::path_key("generation")});
        if (!removed) return std::unexpected(removed.error());
        auto totals = tx.doc(usage, DocumentAddress{.conversation_id = root.value()});
        if (!totals) return std::unexpected(totals.error());
        auto set = totals->set({chord::path_key("models"), chord::path_key("faux/faux-1")},
                chord::Json(chord::Json::Object{{"input", chord::Json(2.0)}}));
        if (!set) return std::unexpected(set.error());
        return {};
    })).has_value());
    const auto frames = recorder->snapshot();
    REQUIRE(frames.size() == 2);
    check_exact_frames(initial, frames);
    check_tampered_frame_fails(initial, frames);
    const auto fresh = consume(harness->conversation_view(root.value()));
    REQUIRE(fresh.has_value());
    CHECK(frames.back().value == fresh.value());
    const auto* docs = fresh.value().as_object()->at("docs").as_object();
    CHECK(!docs->at("pi.live").as_object()->contains("generation"));
    CHECK(docs->at("pi.usage").as_object()->at("models").as_object()->contains("faux/faux-1"));
    REQUIRE(consume(harness->close()).has_value());
}

// Discriminating case (ticket #1088): a storage failure ends every
// structural watch with SessionFailed and changes no committed state; the
// failed transaction appears in no view.
TEST_CASE("a failed session ends structural watches without changing committed state",
        "[durable][observation][failure][issue1088]") {
    cch::tests::GatedStorage gated{};
    cch::tests::GatedStorage* storage = &gated;
    auto opened = consume(Harness::open(*storage, HarnessOptions{}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    const auto root = consume(harness->root());
    REQUIRE(root.has_value());

    auto graph_recorder = std::make_shared<FrameRecorder>();
    auto graph_watch = consume(harness->watch_task_graph(graph_recorder->listener()));
    REQUIRE(graph_watch.has_value());
    auto view_recorder = std::make_shared<FrameRecorder>();
    auto view_watch = consume(harness->watch_conversation(root.value(), view_recorder->listener()));
    REQUIRE(view_watch.has_value());

    storage->fail_next_commit("disk vanished");
    const auto failed = consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
        auto entry = tx.append_entry(
                root.value(), "pi.user", chord::Json(chord::Json::Object{{"text", chord::Json("doomed")}}));
        if (!entry) return std::unexpected(entry.error());
        return {};
    }));
    REQUIRE_FALSE(failed.has_value());
    CHECK(harness->session().failed());

    // Both watches end with the Session failure, the failed transaction
    // produced no frame, and the Session-gated observation APIs now report
    // the failed Session instead of inventing state. inspect reads committed
    // records directly: it still derives the unchanged committed world.
    CHECK(graph_watch.value().stop() == WatchEndReason::SessionFailed);
    CHECK(view_watch.value().stop() == WatchEndReason::SessionFailed);
    CHECK(graph_recorder->size() == 0);
    CHECK(view_recorder->size() == 0);
    const auto inspection = consume(harness->inspect());
    REQUIRE_FALSE(inspection.has_value());
    const auto graph = consume(harness->task_graph());
    REQUIRE_FALSE(graph.has_value());
    const auto view = consume(harness->conversation_view(root.value()));
    REQUIRE_FALSE(view.has_value());

    // The failed session closes its storage; no failed publication reached either watch.
}

namespace {

[[nodiscard]] ai::ModelStream text_reply() {
    return ai::ModelStream{ai::ModelStreamProducer{
            [](ai::AssistantEventSink sink, ai::ModelStreamCompletion completion) noexcept {
                auto message = ai::assistant_text_message("finished");
                auto delivered = sink(ai::AssistantDoneEvent{.reason = message.stop_reason, .message = message});
                if (!delivered)
                    completion(std::unexpected(delivered.error()));
                else
                    completion(std::move(message));
            }}};
}

[[nodiscard]] SubmissionDraft text_input(std::string text) {
    return SubmissionDraft{
            .body = InputSubmissionDraft{.content = chord::Json{std::move(text)}, .when_busy = std::nullopt}};
}

} // namespace

// Frozen comparison (ticket #1088): every expectation below is the audit-1056
// observability artifact's own `expected` object for the named scenario, so a
// change in the frozen behavior changes the expectations the C++ side must
// meet. Each section drives the public Harness caller and compares against
// the registered observation.
TEST_CASE("frozen observation: conversation watch publishes delta frames whose final view equals a fresh hydration",
        "[durable][observation][frozen][issue1088]") {
        const auto frozen = FrozenScenario::load(
                kObservabilityArtifact, "conversation watch publishes delta frames whose final view equals a fresh hydration");
        REQUIRE(frozen.has_value());
        const auto* initial = frozen->object_member("initial");
        REQUIRE(initial != nullptr);
        const auto* final_expected = frozen->object_member("final");
        REQUIRE(final_expected != nullptr);
        const auto frames_expected = frozen->number_member("frames");
        REQUIRE(frames_expected.has_value());
        const auto* first_frame_ops = frozen->array_member("firstFrameOps");
        REQUIRE(first_frame_ops != nullptr);

        auto latch = std::make_shared<LatchedReply>();
        latch->reply = "hello";
        MemoryStorage storage;
        HarnessOptions options{};
        options.stream = [latch](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
            return latched_stream(latch);
        };
        auto opened = consume(Harness::open(storage, std::move(options)));
        REQUIRE(opened.has_value());
        auto harness = std::move(opened.value());
        const auto root = consume(harness->root());
        REQUIRE(root.has_value());

        auto recorder = std::make_shared<FrameRecorder>();
        auto publications = std::make_shared<std::vector<std::string>>();
        auto publication_mutex = std::make_shared<std::mutex>();
        REQUIRE(harness->session().subscribe_commits([publications, publication_mutex](const CommitPublication& publication) {
            std::string description = std::to_string(publication.seq) + ":";
            for (const auto& change : publication.changes) {
                if (const auto* doc = std::get_if<DocumentCommitChange>(&change)) {
                    description += " doc=" + doc->record.kind + "(";
                    for (const auto& op : doc->ops) description.push_back(op_verb(op));
                    description += ")";
                } else if (const auto* table = std::get_if<TableCommitChange>(&change)) {
                    description += " " + table->type;
                } else {
                    description += " submission";
                }
            }
            std::lock_guard lock(*publication_mutex);
            publications->push_back(std::move(description));
        }).has_value());
        auto watch = consume(harness->watch_conversation(root.value(), recorder->listener()));
        REQUIRE(watch.has_value());

        // The acquisition value is the frozen initial view: root id 1, no
        // entries, the five built-in documents keyed by bare kind, an empty
        // live block and an empty inbox.
        const auto initial_view = consume(harness->conversation_view(root.value()));
        REQUIRE(initial_view.has_value());
        const auto* initial_object = initial_view.value().as_object();
        REQUIRE(initial_object != nullptr);
        CHECK(*initial_object->at("conversation").as_object()->at("id").as_number()
                == initial->at("conversation").get_object().at("id").get_number());
        CHECK(initial_object->at("entries").as_array()->size()
                == static_cast<std::size_t>(initial->at("entries").get_number()));
        const auto* initial_docs = initial_object->at("docs").as_object();
        for (const auto& kind : initial->at("docs").get_array()) {
            INFO(kind.get_string());
            CHECK(initial_docs->contains(kind.get_string()));
        }
        CHECK(json_equals(chord::Json(chord::Json::Object{}), initial->at("live")));
        CHECK(initial_object->at("docs").as_object()->at("pi.inbox").as_object()->at("items").as_array()->empty()
                == initial->at("inbox").at("items").get_array().empty());
        CHECK(watch.value().value() == initial_view.value());

        // One admission, one reply, one settlement: exactly the frozen frame
        // count, each frame replayable onto the previous revision.
        const auto submitted = consume(harness->submit(root.value(), text_input("hi")));
        REQUIRE(submitted.has_value());
        latch->release.store(true);
        wait_inspection(harness.get(),
                [](const HarnessInspection& inspected) { return inspected.submissions.empty(); },
                "conversation-watch submission settle");
        const auto settled = consume(submitted.value().wait());
        REQUIRE(settled.has_value());
        const auto frame_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (recorder->size() < static_cast<std::size_t>(*frames_expected)
                && std::chrono::steady_clock::now() < frame_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const auto observed_frames = recorder->snapshot();
        if (observed_frames.size() != static_cast<std::size_t>(*frames_expected)) {
            std::lock_guard publication_lock(*publication_mutex);
            for (const auto& publication : *publications) WARN("publication " << publication);
            for (const auto& frame : observed_frames) {
                std::string verbs;
                for (const auto& op : frame.ops) verbs.push_back(op_verb(op));
                WARN("observed frame ops: " << verbs);
            }
        }
        CHECK(observed_frames.size() == static_cast<std::size_t>(*frames_expected));
        if (observed_frames.size() < static_cast<std::size_t>(*frames_expected)) return;

        const auto frames = recorder->snapshot();
        REQUIRE(frames.size() == static_cast<std::size_t>(*frames_expected));
        REQUIRE(frames.front().ops.size() == first_frame_ops->size());
        for (std::size_t index = 0; index < first_frame_ops->size(); ++index) {
            CHECK(op_verb(frames.front().ops[index]) == first_frame_ops->at(index).get_string()[0]);
        }
        check_exact_frames(initial_view.value(), frames);

        // The final delivered value equals a fresh hydration and carries the
        // frozen entries: the user text and the assistant's text block.
        const auto fresh = consume(harness->conversation_view(root.value()));
        REQUIRE(fresh.has_value());
        CHECK(frames.back().value == fresh.value());
        const auto* entries = fresh.value().as_object()->at("entries").as_array();
        REQUIRE(entries->size() == final_expected->at("entries").get_array().size());
        for (std::size_t index = 0; index < entries->size(); ++index) {
            const auto& expected_entry = final_expected->at("entries").get_array().at(index);
            CHECK(*(*entries)[index].as_object()->at("kind").as_string() == expected_entry.get_array().at(0).get_string());
            const auto& expected_content = expected_entry.get_array().at(1);
            if (const auto* expected_text = expected_content.get_if<std::string>()) {
                // The frozen expectation records the plain user text.
                const auto text = entry_text((*entries)[index]);
                REQUIRE(text.has_value());
                CHECK(*text == *expected_text);
            } else {
                // The assistant entry records its content blocks; compare
                // the message content against the frozen blocks.
                const auto* data = (*entries)[index].as_object()->at("data").as_object();
                REQUIRE(data != nullptr);
                const auto* model = data->at("model").as_array();
                REQUIRE(model != nullptr);
                REQUIRE(!model->empty());
                CHECK(json_equals((*model)[0].as_object()->at("content"), expected_content));
            }
        }
        const auto* final_docs = fresh.value().as_object()->at("docs").as_object();
        for (const auto& kind : final_expected->at("docs").get_array()) {
            CHECK(final_docs->contains(kind.get_string()));
        }
        REQUIRE(consume(harness->close()).has_value());
        CHECK(watch.value().stop() == WatchEndReason::SessionClosed);
}

TEST_CASE("frozen observation: the task graph follows owner edges, owned conversations, waits and completion to empty",
        "[durable][observation][frozen][issue1088]") {
        const auto frozen = FrozenScenario::load(
                kObservabilityArtifact, "the task graph follows owner edges, owned conversations, waits and completion to empty");
        REQUIRE(frozen.has_value());
        const auto* before = frozen->object_member("before");
        REQUIRE(before != nullptr);
        const auto* running_expected = frozen->object_member("running");
        REQUIRE(running_expected != nullptr);
        const auto* after = frozen->object_member("after");
        REQUIRE(after != nullptr);

        auto counters = std::make_shared<ExecutionCounters>();
        auto release_child = std::make_shared<std::atomic_bool>(false);
        MemoryStorage storage;
        std::vector<TaskDefinition> definitions;
        auto child_definition = std::make_shared<TaskDefinition>(gate_definition(release_child, "audit1088.child"));
        definitions.push_back(gate_definition(release_child, "audit1088.child"));
        definitions.push_back(waiting_parent_definition(child_definition, counters));
        auto opened = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(definitions)}));
        REQUIRE(opened.has_value());
        auto harness = std::move(opened.value());
        const auto root = consume(harness->root());
        REQUIRE(root.has_value());

        auto recorder = std::make_shared<FrameRecorder>();
        auto watch = consume(harness->watch_task_graph(recorder->listener()));
        REQUIRE(watch.has_value());
        CHECK(before->empty());
        CHECK(watch.value().value() == empty_graph_value());

        TaskId parent_id{};
        REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
            const TaskDefinition* definition = harness->task_definition("audit1088.parent");
            if (definition == nullptr) {
                return std::unexpected(support::make_error(support::ErrorCode::Validation, "missing parent definition"));
            }
            auto parent = tx.create_task(*definition,
                    chord::Json(chord::Json::Object{{"n", chord::Json(0.0)}}), TaskOptions{.conversation_id = root.value()});
            if (!parent) return std::unexpected(parent.error());
            parent_id = *parent;
            return {};
        })).has_value());
        REQUIRE(harness->resume().has_value());

        // The frozen running graph: the parent waits on its own child, owns
        // one conversation, and the child runs under its owner edge.
        wait_until([&] {
            for (const auto& frame : recorder->snapshot()) {
                const auto* tasks = frame.value.as_object()->at("tasks").as_object();
                for (const auto& [id, task] : *tasks) {
                    (void)id;
                    const auto* row = task.as_object();
                    const auto owner = row->find("owner");
                    if (owner != row->end() && owner->second.as_number() &&
                            *row->at("state").as_object()->at("status").as_string() == "running") return true;
                }
            }
            return false;
        }, "running child graph frame");
        const auto frames = recorder->snapshot();
        const chord::Json::Object* tasks = nullptr;
        for (const auto& frame : frames) {
            const auto* candidate = frame.value.as_object()->at("tasks").as_object();
            for (const auto& [id, task] : *candidate) {
                (void)id;
                const auto* row = task.as_object();
                const auto owner = row->find("owner");
                if (owner != row->end() && owner->second.as_number() &&
                        *row->at("state").as_object()->at("status").as_string() == "running") tasks = candidate;
            }
            if (tasks) break;
        }
        REQUIRE(tasks != nullptr);
        const auto& parent_expected = running_expected->at("parent");
        const auto& child_expected = running_expected->at("child");
        const auto* parent = tasks->at(std::to_string(parent_id.value)).as_object();
        CHECK(*parent->at("state").as_object()->at("status").as_string()
                == parent_expected.at("status").get_string());
        CHECK(parent->at("conversations").as_array()->size()
                == static_cast<std::size_t>(parent_expected.at("conversations").get_number()));
        const auto* on = parent->at("state").as_object()->at("on").as_array();
        REQUIRE(on->size() == 1);
        const TaskId child_id{static_cast<std::int64_t>(*(*on)[0].as_number())};
        const auto* child = tasks->at(std::to_string(child_id.value)).as_object();
        CHECK(*child->at("state").as_object()->at("status").as_string() == child_expected.at("status").get_string());
        REQUIRE(child->at("owner").as_number() != nullptr);
        CHECK(static_cast<std::int64_t>(*child->at("owner").as_number()) == parent_id.value);
        // The waited child is the parent's owned child (onOwnsChild edge).
        CHECK(child->at("conversations").as_array()->empty());

        // Completion empties the graph, exactly the frozen after state.
        release_child->store(true);
        wait_task_terminal(harness.get(), parent_id, "frozen graph parent settle");
        const auto receipt = consume(harness->wait_for_task(parent_id));
        REQUIRE(receipt.has_value());
        wait_until([&] { return !recorder->snapshot().empty() && recorder->snapshot().back().value == empty_graph_value(); },
                "empty graph");
        CHECK(recorder->snapshot().back().value.as_object()->at("tasks").as_object()->empty() == after->empty());
        const auto fresh = consume(harness->task_graph());
        REQUIRE(fresh.has_value());
        CHECK(fresh.value().tasks.empty());
        REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("frozen observation: inspect derives ready, running, waiting and blocked task states without running task code",
        "[durable][observation][frozen][issue1088]") {
        const auto frozen = FrozenScenario::load(kObservabilityArtifact,
                "inspect derives ready, running, waiting and blocked task states without running task code");
        REQUIRE(frozen.has_value());
        const auto* paused_expected = frozen->object_member("paused");
        REQUIRE(paused_expected != nullptr);
        const auto* running_expected = frozen->object_member("running");
        REQUIRE(running_expected != nullptr);
        const auto* settled_expected = frozen->object_member("settled");
        REQUIRE(settled_expected != nullptr);
        REQUIRE(paused_expected->contains("scheduling"));
        REQUIRE(paused_expected->contains("gate"));
        REQUIRE(paused_expected->contains("dependent"));
        REQUIRE(paused_expected->contains("missing"));
        REQUIRE(running_expected->contains("scheduling"));
        REQUIRE(running_expected->contains("gate"));
        REQUIRE(running_expected->contains("dependent"));
        REQUIRE(settled_expected->contains("liveTaskKinds"));
        REQUIRE(settled_expected->contains("missing"));

        auto release = std::make_shared<std::atomic_bool>(false);
        cch::tests::TempWorkspace workspace;
        const auto file = workspace.path() / "inspect.sqlite";
        auto initial_storage = SqliteStorage::open(file);
        REQUIRE(initial_storage.has_value());
        TaskId gate_id{};
        TaskId dependent_id{};
        TaskId missing_id{};
        {
            std::vector<TaskDefinition> tasks;
            tasks.push_back(gate_definition(release));
            tasks.push_back(dependent_definition());
            tasks.push_back(counting_task(std::make_shared<ExecutionCounters>(), "audit.inspect-missing"));
            auto opened = consume(Harness::open(**initial_storage, HarnessOptions{.tasks = std::move(tasks)}));
            REQUIRE(opened.has_value());
            auto harness = std::move(opened.value());
            const auto root = consume(harness->root());
            REQUIRE(root.has_value());
            REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
                const TaskDefinition* gate = harness->task_definition("audit.inspect-gate");
                const TaskDefinition* dependent = harness->task_definition("audit.inspect-dependent");
                const TaskDefinition* missing = harness->task_definition("audit.inspect-missing");
                if (gate == nullptr || dependent == nullptr || missing == nullptr) {
                    return std::unexpected(support::make_error(support::ErrorCode::Validation, "missing definition"));
                }
                auto created_gate = tx.create_task(*gate, chord::Json(chord::Json::Object{{"n", chord::Json(1.0)}}),
                        TaskOptions{.conversation_id = root.value()});
                if (!created_gate) return std::unexpected(created_gate.error());
                gate_id = *created_gate;
                auto created_dependent = tx.create_task(*dependent,
                        chord::Json(chord::Json::Object{{"gate", chord::Json(static_cast<double>(gate_id.value))}}),
                        TaskOptions{.conversation_id = root.value()});
                if (!created_dependent) return std::unexpected(created_dependent.error());
                dependent_id = *created_dependent;
                auto created_missing = tx.create_task(*missing,
                        chord::Json(chord::Json::Object{{"n", chord::Json(3.0)}}), TaskOptions{.conversation_id = root.value()});
                if (!created_missing) return std::unexpected(created_missing.error());
                missing_id = *created_missing;
                return {};
            })).has_value());
            REQUIRE(consume(harness->close()).has_value());
        }

        // Reopen without the missing kind's definition: the same committed
        // records now inspect as ready/ready/blocked, exactly the frozen
        // paused view, and inspection never ran any task code.
        std::vector<TaskDefinition> reopened_tasks;
        reopened_tasks.push_back(gate_definition(release));
        reopened_tasks.push_back(dependent_definition());
        initial_storage->reset();
        auto reopened_storage = SqliteStorage::open(file);
        REQUIRE(reopened_storage.has_value());
        auto opened = consume(Harness::open(**reopened_storage, HarnessOptions{.tasks = std::move(reopened_tasks)}));
        REQUIRE(opened.has_value());
        auto harness = std::move(opened.value());

        auto inspect = consume(harness->inspect());
        REQUIRE(inspect.has_value());
        CHECK(inspect.value().scheduling == paused_expected->at("scheduling").get_string());
        REQUIRE(inspect.value().tasks.size() == 3);
        const auto& gate_expected = paused_expected->at("gate");
        const auto& dependent_expected = paused_expected->at("dependent");
        const auto& missing_expected = paused_expected->at("missing");
        const TaskInspection& gate_row = inspect.value().tasks[0];
        const TaskInspection& dependent_row = inspect.value().tasks[1];
        const TaskInspection& missing_row = inspect.value().tasks[2];
        CHECK(gate_row.record.id == gate_id);
        CHECK(dependent_row.record.id == dependent_id);
        CHECK(missing_row.record.id == missing_id);
        CHECK(gate_row.record.kind == "audit.inspect-gate");
        CHECK(dependent_row.record.kind == "audit.inspect-dependent");
        CHECK(missing_row.record.kind == "audit.inspect-missing");
        CHECK(gate_row.state.kind == gate_expected.at("kind").get_string());
        CHECK(gate_row.state.migrates == gate_expected.at("migrates").get_boolean());
        CHECK(dependent_row.state.kind == dependent_expected.at("kind").get_string());
        CHECK(dependent_row.state.migrates == dependent_expected.at("migrates").get_boolean());
        CHECK(missing_row.state.kind == missing_expected.at("kind").get_string());
        REQUIRE(missing_row.state.reason.has_value());
        CHECK(missing_row.state.reason.value() == missing_expected.at("reason").get_string());

        // Resume: the gate runs (phase holds) and the dependent waits on it.
        REQUIRE(harness->resume().has_value());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        for (;;) {
            const auto probe = consume(harness->inspect());
            REQUIRE(probe.has_value());
            const TaskInspection* gate_now = nullptr;
            const TaskInspection* dependent_now = nullptr;
            for (const auto& row : probe.value().tasks) {
                if (row.record.id == gate_id) gate_now = &row;
                if (row.record.id == dependent_id) dependent_now = &row;
            }
            if (gate_now != nullptr && gate_now->state.kind == "running" && dependent_now != nullptr
                    && dependent_now->state.kind == "waiting") {
                CHECK(probe.value().scheduling == running_expected->at("scheduling").get_string());
                CHECK(gate_now->state.kind == running_expected->at("gate").at("kind").get_string());
                CHECK(dependent_now->state.kind == running_expected->at("dependent").at("kind").get_string());
                REQUIRE(dependent_now->state.on.size() == static_cast<std::size_t>(running_expected->at("dependent").at("onLength").get_number()));
                CHECK((dependent_now->state.on.front() == gate_id) == running_expected->at("dependent").at("onIsGate").get_boolean());
                break;
            }
            if (std::chrono::steady_clock::now() > deadline) FAIL("gate did not run or dependent did not wait");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        // Releasing the gate settles gate and dependent; only the missing
        // kind's task stays live and blocked, exactly the frozen settled view.
        // (`wait_for_idle` cannot be used here: the blocked task is live by
        // design and idle waits on all live work.)
        release->store(true);
        wait_inspection(harness.get(),
                [&](const HarnessInspection& inspected) {
                    bool gate_live = false;
                    bool dependent_live = false;
                    for (const TaskInspection& row : inspected.tasks) {
                        if (row.record.id == gate_id) gate_live = true;
                        if (row.record.id == dependent_id) dependent_live = true;
                    }
                    return !gate_live && !dependent_live;
                },
                "gate and dependent settle");
        const auto settled = consume(harness->inspect());
        REQUIRE(settled.has_value());
        const auto& live_kinds = settled_expected->at("liveTaskKinds").get_array();
        REQUIRE(settled.value().tasks.size() == live_kinds.size());
        CHECK(settled.value().tasks.front().record.kind == live_kinds.front().get_string());
        CHECK(settled.value().tasks.front().record.id == missing_id);
        CHECK(settled.value().tasks.front().state.kind == settled_expected->at("missing").at("kind").get_string());
        CHECK(settled.value().tasks.front().state.reason == settled_expected->at("missing").at("reason").get_string());
        REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("frozen observation: inspect lists the placed submission and the generation task that owns it",
        "[durable][observation][frozen][issue1088]") {
        const auto frozen = FrozenScenario::load(
                kObservabilityArtifact, "inspect lists the placed submission and the generation task that owns it");
        REQUIRE(frozen.has_value());
        const auto* submissions_expected = frozen->array_member("submissions");
        REQUIRE(submissions_expected != nullptr);
        const auto* tasks_expected = frozen->array_member("tasks");
        REQUIRE(tasks_expected != nullptr);

        auto latch = std::make_shared<LatchedReply>();
        latch->reply = "hello";
        MemoryStorage storage;
        HarnessOptions options{};
        options.stream = [latch](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
            return latched_stream(latch);
        };
        auto opened = consume(Harness::open(storage, std::move(options)));
        REQUIRE(opened.has_value());
        auto harness = std::move(opened.value());
        const auto root = consume(harness->root());
        REQUIRE(root.has_value());

        const auto submitted = consume(harness->submit(root.value(), text_input("hi")));
        REQUIRE(submitted.has_value());

        // While the model call holds, the placed submission and its running
        // generation task are both committed records, exactly the frozen
        // inspection.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        bool matched = false;
        for (;;) {
            const auto inspection = consume(harness->inspect());
            REQUIRE(inspection.has_value());
            const bool submissions_match = inspection.value().submissions.size() == 1
                    && inspection.value().submissions.front().id == submitted.value().id
                    && inspection.value().submissions.front().conversation_id == root.value();
            bool generation_running = false;
            for (const auto& row : inspection.value().tasks) {
                if (row.record.kind == "pi.generation" && row.state.kind == "running") generation_running = true;
            }
            if (inspection.value().submissions.size() == 1 && generation_running) {
                const auto& row = inspection.value().submissions.front();
                const auto& expected_row = submissions_expected->front();
                CHECK((row.id == submitted.value().id) == expected_row.at("idMatches").get_boolean());
                CHECK((row.conversation_id == root.value()) == expected_row.at("conversationIsRoot").get_boolean());
                CHECK(row.type == expected_row.at("type").get_string());
                CHECK(row.status == expected_row.at("status").get_string());
                const auto& expected_task = tasks_expected->front();
                CHECK(expected_task.at("kind").get_string() == std::string("pi.generation"));
                CHECK(generation_running);
                matched = true;
                break;
            }
            if (!submissions_match && std::chrono::steady_clock::now() > deadline)
                FAIL("submission never inspected as placed");
            if (std::chrono::steady_clock::now() > deadline) FAIL("generation task never inspected as running");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(matched);

        // Settlement empties the unsettled list; the committed record stays.
        latch->release.store(true);
        wait_inspection(harness.get(),
                [](const HarnessInspection& inspected) { return inspected.submissions.empty(); },
                "placed submission settle");
        const auto settled = consume(submitted.value().wait());
        REQUIRE(settled.has_value());
        const auto after = consume(harness->inspect());
        REQUIRE(after.has_value());
        CHECK(after.value().submissions.empty());
        REQUIRE(consume(harness->close()).has_value());
}
