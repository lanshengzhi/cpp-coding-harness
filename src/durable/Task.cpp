#include <cch/durable/Task.hpp>

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace cch::durable {

struct TaskDefinition::Impl {
    std::string kind;
    std::int64_t version{1};
    std::move_only_function<chord::Json(const chord::Json&)> initial;
    std::map<std::string, PhaseHandler> phases;
    AbortHandler abort_handler;
};

TaskDefinition::TaskDefinition(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TaskDefinition::TaskDefinition(TaskDefinition&&) noexcept = default;
TaskDefinition& TaskDefinition::operator=(TaskDefinition&&) noexcept = default;
TaskDefinition::~TaskDefinition() = default;

const std::string& TaskDefinition::kind() const noexcept { return impl_->kind; }

std::int64_t TaskDefinition::version() const noexcept { return impl_->version; }

chord::Json TaskDefinition::initial(const chord::Json& input) const {
    if (impl_->initial) {
        return impl_->initial(input);
    }
    return chord::Json(chord::Json::Object{});
}

bool TaskDefinition::has_phase(std::string_view phase) const { return impl_->phases.contains(std::string{phase}); }

support::AsyncResult<void> TaskDefinition::run_phase(
        std::string_view phase, const TaskRecord& task, TaskRuntime& runtime) const {
    const auto it = impl_->phases.find(std::string{phase});
    if (it == impl_->phases.end()) {
        return support::AsyncResult<void>{std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Unknown phase: " + std::string{phase}))};
    }
    return it->second(task, runtime);
}

support::AsyncResult<void> TaskDefinition::run_abort(const TaskRecord& task, TaskRuntime& runtime) const {
    if (!impl_->abort_handler) {
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    }
    return impl_->abort_handler(task, runtime);
}

TaskDefinitionBuilder::TaskDefinitionBuilder(std::string kind, std::int64_t version)
    : impl_(std::make_unique<TaskDefinition::Impl>(TaskDefinition::Impl{
              .kind = std::move(kind),
              .version = version,
              .initial = {},
              .phases = {},
              .abort_handler = {},
      })) {}

TaskDefinitionBuilder::~TaskDefinitionBuilder() = default;
TaskDefinitionBuilder::TaskDefinitionBuilder(TaskDefinitionBuilder&&) noexcept = default;
TaskDefinitionBuilder& TaskDefinitionBuilder::operator=(TaskDefinitionBuilder&&) noexcept = default;

TaskDefinitionBuilder& TaskDefinitionBuilder::initial(std::move_only_function<chord::Json(const chord::Json&)> fn) {
    impl_->initial = std::move(fn);
    return *this;
}

TaskDefinitionBuilder& TaskDefinitionBuilder::phase(std::string name, PhaseHandler handler) {
    impl_->phases.emplace(std::move(name), std::move(handler));
    return *this;
}

TaskDefinitionBuilder& TaskDefinitionBuilder::abort(AbortHandler handler) {
    impl_->abort_handler = std::move(handler);
    return *this;
}

support::Expected<TaskDefinition> TaskDefinitionBuilder::build() {
    if (impl_->kind.empty()) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "Task kind must not be empty"));
    }
    return TaskDefinition(std::move(impl_));
}

} // namespace cch::durable
