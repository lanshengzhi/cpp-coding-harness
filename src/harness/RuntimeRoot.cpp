#include "RuntimeRoot.hpp"

#include <boost/asio/post.hpp>

#include <atomic>
#include <exception>
#include <utility>

namespace cch::harness {

struct RuntimeRoot::State final {
    State(
        std::shared_ptr<boost::asio::io_context> loop,
        std::size_t worker_count,
        std::size_t max_admitted_operations,
        std::size_t max_admitted_bytes)
        : loop(std::move(loop)),
          work_guard(boost::asio::make_work_guard(*this->loop)),
          worker_count(worker_count),
          max_admitted_operations(max_admitted_operations),
          max_admitted_bytes(max_admitted_bytes) {
        workers.reserve(worker_count);
        for (std::size_t index = 0; index < worker_count; ++index) {
            workers.emplace_back([this](std::stop_token) {
                run_worker();
            });
        }
    }

    ~State() {
        shutdown();
    }

    State(const State&) = delete;
    State& operator=(const State&) = delete;

    [[nodiscard]] bool reserve_admission(std::size_t byte_charge) noexcept {
        std::lock_guard lock(admission_mutex);
        if (stopping.load(std::memory_order_acquire) || worker_count == 0 || admitted_operations >= max_admitted_operations ||
            byte_charge > max_admitted_bytes - admitted_bytes) {
            return false;
        }
        ++admitted_operations;
        admitted_bytes += byte_charge;
        return true;
    }

    void release_admission(std::size_t byte_charge) noexcept {
        std::lock_guard lock(admission_mutex);
        if (admitted_operations == 0 || admitted_bytes < byte_charge) {
            std::terminate();
        }
        --admitted_operations;
        admitted_bytes -= byte_charge;
    }

    [[nodiscard]] bool post_worker(RuntimeRoot::Task task) noexcept {
        try {
            {
                std::lock_guard lock(worker_mutex);
                if (stopping.load(std::memory_order_acquire)) {
                    return false;
                }
                worker_tasks.push_back(std::move(task));
            }
            worker_ready.notify_one();
            return true;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool post_loop(RuntimeRoot::Task task) noexcept {
        try {
            boost::asio::post(*loop, std::move(task));
            return true;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] boost::asio::any_io_executor executor() const noexcept {
        return loop->get_executor();
    }

    void stop_admission_and_drain_workers() noexcept {
        {
            std::lock_guard lock(admission_mutex);
            if (stopping.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
        }
        worker_ready.notify_all();
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers.clear();
    }

    void shutdown() noexcept {
        stop_admission_and_drain_workers();
        work_guard.reset();
    }

private:
    void run_worker() noexcept {
        for (;;) {
            RuntimeRoot::Task task;
            {
                std::unique_lock lock(worker_mutex);
                worker_ready.wait(lock, [&] {
                    return stopping.load(std::memory_order_acquire) || !worker_tasks.empty();
                });
                if (worker_tasks.empty()) {
                    return;
                }
                task = std::move(worker_tasks.front());
                worker_tasks.pop_front();
            }
            task();
        }
    }

    std::shared_ptr<boost::asio::io_context> loop;

    std::mutex worker_mutex;
    std::condition_variable worker_ready;
    std::deque<RuntimeRoot::Task> worker_tasks;
    std::vector<std::jthread> workers;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard;

    std::mutex admission_mutex;
    std::size_t admitted_operations{0};
    std::size_t admitted_bytes{0};
    std::size_t worker_count{0};
    std::size_t max_admitted_operations{0};
    std::size_t max_admitted_bytes{0};
    std::atomic<bool> stopping{false};
};

struct RuntimeTarget::State final : std::enable_shared_from_this<RuntimeTarget::State> {
    explicit State(std::shared_ptr<RuntimeRoot::State> root) : root(std::move(root)) {}

    [[nodiscard]] std::optional<std::size_t> try_admit(std::size_t byte_charge) noexcept {
        if (!root->reserve_admission(byte_charge)) {
            return std::nullopt;
        }
        std::lock_guard lock(mailbox_mutex);
        return next_sequence++;
    }

    [[nodiscard]] bool post_worker(RuntimeRoot::Task task) noexcept {
        return root->post_worker(std::move(task));
    }

    [[nodiscard]] bool post_loop(RuntimeRoot::Task task) noexcept {
        return root->post_loop(std::move(task));
    }

    [[nodiscard]] boost::asio::any_io_executor executor() const noexcept {
        return root->executor();
    }

    void release_admission(std::size_t byte_charge) noexcept {
        root->release_admission(byte_charge);
    }

    void post_result(std::size_t sequence, RuntimeRoot::Task task) noexcept {
        try {
            bool schedule_drain = false;
            {
                std::lock_guard lock(mailbox_mutex);
                const auto [_, inserted] = completed_results.emplace(sequence, std::move(task));
                if (!inserted) {
                    std::terminate();
                }
                if (!mailbox_drain_scheduled) {
                    mailbox_drain_scheduled = true;
                    schedule_drain = true;
                }
            }
            if (schedule_drain && !root->post_loop([self = shared_from_this()]() noexcept {
                    self->drain_mailbox();
                })) {
                std::terminate();
            }
        } catch (...) {
            // An admitted operation needs a terminal mailbox slot. Capacity
            // failure cannot be repaired by executing work inline.
            std::terminate();
        }
    }

private:
    void drain_mailbox() noexcept {
        for (;;) {
            RuntimeRoot::Task task;
            {
                std::lock_guard lock(mailbox_mutex);
                const auto result = completed_results.find(next_delivery_sequence);
                if (result == completed_results.end()) {
                    mailbox_drain_scheduled = false;
                    return;
                }
                task = std::move(result->second);
                completed_results.erase(result);
                ++next_delivery_sequence;
            }
            task();
        }
    }

    std::shared_ptr<RuntimeRoot::State> root;
    std::mutex mailbox_mutex;
    std::map<std::size_t, RuntimeRoot::Task> completed_results;
    std::size_t next_sequence{0};
    std::size_t next_delivery_sequence{0};
    bool mailbox_drain_scheduled{false};
};

RuntimeTarget::Admission::Admission(
    std::shared_ptr<RuntimeTarget::State> state,
    std::size_t sequence,
    std::size_t byte_charge) noexcept
    : state_(std::move(state)), sequence_(sequence), byte_charge_(byte_charge) {}

RuntimeTarget::Admission::~Admission() {
    if (state_) {
        state_->release_admission(byte_charge_);
    }
}

bool RuntimeTarget::Admission::post_worker(Task task) const noexcept {
    return state_ && state_->post_worker(std::move(task));
}

bool RuntimeTarget::Admission::post_loop(Task task) const noexcept {
    return state_ && state_->post_loop(std::move(task));
}

void RuntimeTarget::Admission::complete(Task task) && noexcept {
    auto state = std::exchange(state_, {});
    if (!state) {
        std::terminate();
    }
    const auto sequence = sequence_;
    const auto byte_charge = byte_charge_;
    state->post_result(
        sequence,
        [state = std::move(state), task = std::move(task), byte_charge]() mutable noexcept {
            task();
            state->release_admission(byte_charge);
        });
}

RuntimeTarget::RuntimeTarget(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

std::optional<RuntimeTarget::Admission> RuntimeTarget::try_admit(std::size_t byte_charge) const noexcept {
    if (!state_) {
        return std::nullopt;
    }
    const auto sequence = state_->try_admit(byte_charge);
    if (!sequence) {
        return std::nullopt;
    }
    return Admission{state_, *sequence, byte_charge};
}

boost::asio::any_io_executor RuntimeTarget::executor() const noexcept {
    return state_->executor();
}

RuntimeRoot::RuntimeRoot(
    std::shared_ptr<boost::asio::io_context> loop,
    std::size_t worker_count,
    std::size_t max_admitted_operations,
    std::size_t max_admitted_bytes)
    : state_(std::make_shared<State>(
          std::move(loop), worker_count, max_admitted_operations, max_admitted_bytes)) {}

RuntimeRoot::~RuntimeRoot() = default;

void RuntimeRoot::close() noexcept {
    if (state_) {
        state_->stop_admission_and_drain_workers();
    }
}

std::shared_ptr<RuntimeTarget> RuntimeRoot::make_target() const {
    return std::make_shared<RuntimeTarget>(
        std::make_shared<RuntimeTarget::State>(state_));
}

boost::asio::any_io_executor RuntimeRoot::executor() const noexcept {
    return state_->executor();
}

} // namespace cch::harness
