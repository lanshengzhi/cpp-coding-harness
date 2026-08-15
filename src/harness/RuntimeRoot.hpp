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

/// Which admission budget an operation draws from (ADR 0040 §Admission,
/// overload, and fairness). Ordinary bulk work uses the shared admitted
/// budget; control work uses the reserved lane so it cannot be rejected
/// behind ordinary bulk work.
enum class AdmissionLane { Ordinary, Reserved };

/// Scenario-measured Runtime admission and fairness limits (ADR 0040
/// §Admission, overload, and fairness; `docs/runtime-capacities.md`). Ordinary
/// bulk work (filesystem, Shell, model streaming) draws from the admitted
/// budget; persistence, credential, terminal-completion, and Close control
/// work draws from the reserved budget so ordinary traffic can never reject
/// required progress. `mailbox_drain_batch` bounds how many terminal results
/// one drain delivers before the mailbox requeues itself at the loop tail,
/// so a busy target cannot monopolize the loop.
struct RuntimeLimits {
    /// Worker threads executing admitted blocking work off the loop.
    std::size_t worker_count{2};
    /// Ordinary admitted operations in flight (task-count bound).
    std::size_t max_admitted_operations{32};
    /// Ordinary admitted operations' conservative byte-charge bound.
    std::size_t max_admitted_bytes{1024 * 1024};
    /// Reserved control operations in flight (persistence, credential,
    /// terminal completion, Close) — never rejected by ordinary bulk work.
    std::size_t max_reserved_operations{8};
    /// Reserved control operations' conservative byte-charge bound.
    std::size_t max_reserved_bytes{256 * 1024};
    /// Terminal results delivered per mailbox drain before the drain
    /// requeues itself at the loop tail (bounded batch, ADR 0040).
    std::size_t mailbox_drain_batch{16};
};

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
        RuntimeLimits limits);
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
            std::size_t byte_charge,
            AdmissionLane lane) noexcept;

        std::shared_ptr<State> state_;
        std::size_t sequence_{0};
        std::size_t byte_charge_{0};
        AdmissionLane lane_{AdmissionLane::Ordinary};
    };

    /// Admit ordinary bulk work from the shared ordinary budget; nullopt is
    /// a typed capacity rejection (the caller maps it to domain `Busy`).
    [[nodiscard]] std::optional<Admission> try_admit(std::size_t byte_charge) const noexcept;
    /// Admit control work from the reserved budget (persistence, credential,
    /// terminal completion, Close); nullopt is a typed capacity rejection.
    /// Reserved admission cannot be rejected behind ordinary bulk work.
    [[nodiscard]] std::optional<Admission> try_admit_reserved(std::size_t byte_charge) const noexcept;
    [[nodiscard]] boost::asio::any_io_executor executor() const noexcept;

    explicit RuntimeTarget(std::shared_ptr<State> state) noexcept;

private:
    friend class RuntimeRoot;

    std::shared_ptr<State> state_;
};

} // namespace cch::harness
