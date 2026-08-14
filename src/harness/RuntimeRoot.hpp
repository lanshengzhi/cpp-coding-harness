#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace cch::harness {

class RuntimeTarget;

/// Private, coding-agent-composed Runtime root. One root owns the process
/// interaction loop and bounded worker capacity for a CLI invocation. Each
/// state-owning target receives its own ordered mailbox from make_target().
class RuntimeRoot final {
public:
    using Task = std::move_only_function<void() noexcept>;

    /// The root owns the loop by shared ownership: every target holds a
    /// reference to it, so the loop can never be destroyed while an admitted
    /// operation or cleanup still exists. The caller drives `*loop` (the
    /// interaction loop) for the full Runtime lifetime.
    RuntimeRoot(
        std::shared_ptr<boost::asio::io_context> loop,
        std::size_t worker_count,
        std::size_t max_admitted_operations,
        std::size_t max_admitted_bytes);
    RuntimeRoot(RuntimeRoot&&) = delete;
    RuntimeRoot& operator=(RuntimeRoot&&) = delete;
    ~RuntimeRoot();
    RuntimeRoot(const RuntimeRoot&) = delete;
    RuntimeRoot& operator=(const RuntimeRoot&) = delete;

    /// Stop admission, drain queued worker work, and join workers. The caller
    /// keeps pumping the shared loop afterwards so every admitted terminal
    /// reaches its target mailbox before the root and loop are destroyed.
    void close() noexcept;

    [[nodiscard]] std::shared_ptr<RuntimeTarget> make_target() const;
    [[nodiscard]] boost::asio::any_io_executor executor() const noexcept;

private:
    friend class RuntimeTarget;

    struct State;

    std::shared_ptr<State> state_;
};

/// One target's serialized result mailbox. This private capability keeps a
/// target's admitted terminal outcomes FIFO by admission sequence while all
/// physical work remains on the shared RuntimeRoot.
class RuntimeTarget final {
    struct State;

public:
    using Task = RuntimeRoot::Task;

    class Admission final {
    public:
        Admission(Admission&&) noexcept = default;
        Admission& operator=(Admission&&) noexcept = default;
        ~Admission();
        Admission(const Admission&) = delete;
        Admission& operator=(const Admission&) = delete;

        /// Queue blocking work. The caller owns the terminal completion path.
        [[nodiscard]] bool post_worker(Task task) const noexcept;
        /// Re-enter the shared Runtime loop before terminal process work begins.
        [[nodiscard]] bool post_loop(Task task) const noexcept;
        /// Return a terminal outcome through this target's ordered mailbox.
        void complete(Task task) && noexcept;

    private:
        friend class RuntimeTarget;
        Admission(
            std::shared_ptr<State> state,
            std::size_t sequence,
            std::size_t byte_charge) noexcept;

        std::shared_ptr<State> state_;
        std::size_t sequence_{0};
        std::size_t byte_charge_{0};
    };

    [[nodiscard]] std::optional<Admission> try_admit(std::size_t byte_charge) const noexcept;
    [[nodiscard]] boost::asio::any_io_executor executor() const noexcept;

    explicit RuntimeTarget(std::shared_ptr<State> state) noexcept;

private:
    friend class RuntimeRoot;

    std::shared_ptr<State> state_;
};

} // namespace cch::harness
