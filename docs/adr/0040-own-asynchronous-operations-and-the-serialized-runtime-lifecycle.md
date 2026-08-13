---
status: accepted
---

# Own asynchronous operations and the serialized Runtime lifecycle

Fallible asynchronous Owner operations use one lazy, move-only, single-consumption `cch::support::AsyncResult<T, E = Error>` contract. One concrete, coding-agent-owned Runtime root supplies the private event loop, bounded worker pool, and bounded FIFO mailboxes needed by real state-owning objects. Agent, Models Runtime, and Agent Session state changes occur only in their owning serialized execution domains. Admission, cancellation, callback lifetime, Session Event Commitment, Session Close, and final application Close are explicit and quiescent.

This decision records the destination frozen by architecture spec [#439](https://github.com/lanshengzhi/cpp-coding-harness/issues/439) for documentation ticket [#440](https://github.com/lanshengzhi/cpp-coding-harness/issues/440) without reopening it. Observable behavior remains governed by [#396](https://github.com/lanshengzhi/cpp-coding-harness/issues/396), the planning authority remains parity map [#2](https://github.com/lanshengzhi/cpp-coding-harness/issues/2), and Semantic Parity remains pinned to pi commit `83114817c68f5413e4d7ba6d7003ddc511cd31d2`. The event loop and worker implementation are private representation choices; replacing them is not parity drift when ordering, cancellation, errors, overload, persistence, and Close behavior remain unchanged.

## `AsyncResult`

`AsyncResult<T, E>` represents ready and pending completion with one operation shape:

- It is lazy, move-only, and consumed exactly once, either by move-only callback `start` or move-only `co_await`.
- Its producer may signal exactly one terminal `std::expected<T, E>`, delivered at most once. After established abandonment, the first terminal value is accepted and discarded safely.
- A ready value completes inline with no support allocation or coroutine suspension. Pending state may allocate.
- The pending operation owns everything it needs after initiation returns unless an exceptional borrowed-lifetime contract is explicit at the declaration.
- Before initiation, the Capability Owner converts ordinary throwable setup failures into the operation's expected error channel. Producer initiation and terminal completion are then `noexcept` contracts.
- Duplicate completion, duplicate or moved-from consumption, reused consumption, and controlled completion-callback contract violations call `std::terminate` in Debug and Release. They are invariant violations, not ordinary errors.
- The coroutine Owner establishes quiescence before destroying a coroutine frame. Concurrent `resume()` and `destroy()` are forbidden; a late first completion after established abandonment never resumes a dead frame.
- The initial owning operation erasure is `std::move_only_function`. A custom box or small-buffer optimization requires measured allocation, object-size, code-size, and compile-fan-out evidence first.

`AsyncResult` chooses no executor, scheduler, timeout, retry, stream, or generic cancellation/stopped outcome. Cancellation is supplied explicitly with `std::stop_token`, and the Capability Owner resolves cancellation/completion races into the operation's honest domain outcome.

## Operation ownership and cancellation

Bounded in-memory queries, environment reads, pure path/model calculations, retry classification, queue accounting, passive event construction, and similar small value operations remain synchronous. Model calls, OAuth, credential operations, filesystem work, Session persistence, Shell/process execution, timers, and cleanup are asynchronous. No interface pretends all work is asynchronous, and no potentially blocking work enters the Runtime loop disguised as a synchronous call.

Cancellation is cooperative. Queued filesystem work may be removed; started short syscalls finish and their result may be ignored; large work checks cancellation between chunks; partial side effects are reported rather than described as rolled back. The Runtime introduces neither `pthread_cancel` nor an initial io_uring requirement.

Session Abort and Session Close request cancellation of active cancellable user work. They do not cancel or discard admitted Session Event Commitments or required persistence work. Operation ownership decides the winning terminal outcome for each race, preserving pi's normal `aborted` lifecycle where it applies.

## Runtime ownership and serialized domains

Each CLI invocation creates one concrete Runtime root owned by `cch_coding_agent`. It is neither a singleton nor a virtual interface. It survives New Session, Session Fork, Session switching, and current Agent Session replacement. At most one current Agent Session accepts prompts. Replacing it first closes and quiesces the old Session, then installs the new one; Session Close does not stop shared Runtime machinery.

The root owns one private event loop, one bounded worker pool, and only the mailboxes required by actual state-owning runtime objects. It does not create an executor hierarchy, event bus, reactive stream framework, priority scheduler, work stealing, service locator, generic lifecycle framework, or configurable queue-policy hierarchy. Timer state remains local to the operation that needs it.

Agent, Models Runtime, and Agent Session state change only in the serialized domain of the object that owns that state. Cross-Owner events and worker results enter the target object's bounded FIFO mailbox; workers never mutate serialized Owner state directly. This requires neither one event loop nor one thread per Capability Owner Package.

## Admission, overload, and fairness

Mailboxes, worker queues, reserved lanes, and waiter sets have hard task-count and conservative byte-charge bounds. Event-loop submission never blocks and never falls back to inline worker execution, including when worker count is zero. Ordinary overload returns typed `Busy`; new user input is rejected before admission rather than accepted and lost.

Persistence, credential, terminal-completion, and Close control work have reserved admission. Required work may wait asynchronously only after admission to a bounded waiter set; a full waiter set returns `Busy`. Close uses pre-reserved capacity and never depends on ordinary queue capacity.

Admitted model-stream, Agent-lifecycle, Tool Update, and Session-lifecycle events use lossless asynchronous backpressure. Replaceable render/progress snapshots coalesce. Diagnostics aggregate within a bound and report omitted counts. Process stdout and stderr always drain even after retained output reaches its truncation bound.

Mailbox drains process bounded batches and requeue themselves at the tail when work remains, so sustained model/render traffic cannot starve timers, cancellation, process draining, or Close. Timer callbacks advance short state and send heavy work to workers. Worker counts, capacities, byte charges, batch sizes, waiter bounds, and timeout values become policy only after representative same-environment workloads record repeated samples, variance, a selection rule, the chosen value, and a regression property.

## Behavior mechanisms and callback lifetime

Mechanisms follow the behavior's semantics:

- passive state uses aggregate-friendly `struct`; closed alternatives use `std::variant`;
- local non-escaping operations use direct lambdas or constrained templates;
- stored or queued single operations use `std::move_only_function`;
- `std::function` requires a documented independent-copy contract;
- closed Runtime-owned behavior families may use private variants; and
- open, identity-bearing, stateful, multi-operation physical capabilities use narrow private virtual interfaces.

New `boost::bind` and `std::bind` are forbidden. `bind_front` or `bind_back` is used only when clearer than a lambda. There is no support-owned multi-operation capability box, custom operation-table framework, or speculative SBO. A future type-erasure mechanism requires a concrete prototype that beats a narrow private virtual interface on ownership, Interface depth, allocation, object/code size, compile fan-out, sanitizer safety, and maintenance.

The specific selected seams are:

- an Agent Tool is a passive descriptor, prompt metadata, and concurrency value plus one move-only execute operation accepting an invocation, stop token, and optional Tool Update sink and returning `AsyncResult`;
- Agent owns one AI-owned move-only `ModelStream` operation by value; `ai::Models` implements it and its closure deliberately shares Models Runtime lifetime;
- coding-agent Models Runtime remains the concrete owner of `models.json`, authentication storage, Provider composition, availability, refresh, and selection rather than becoming a virtual-for-fake seam;
- Provider, CredentialStore, Terminal, AsyncExecutionEnv, connected WebSocket, and genuinely connected/stateful HTTP transport remain narrow private virtual physical capabilities;
- stateless HTTP/auth/process/clipboard/timer helpers remain concrete until a second implementation demonstrates a capability seam;
- SessionStore is a private concrete facade over the closed JSONL and in-memory alternatives;
- the private heterogeneous TUI Component tree retains virtual dispatch; and
- broad positional application callbacks become one closed action value and one move-only sink.

Stored and asynchronous callbacks capture owned values by move, deliberate shared ownership, or weak ownership plus closed/generation validation. Escaping local-reference and implicit escaping-`this` captures are forbidden. Callable ownership never makes a captured raw pointer or reference safe. Internal continuations whose scheduling failure is a fatal Runtime invariant have `noexcept` signatures.

Subscription delivery uses a stable snapshot plus an active check before each invocation. Cancellation before a subscriber's turn suppresses that event for it; a subscriber added during delivery starts with the next event; duplicate same-event delivery is impossible. Late callbacks after Close become benign drops through subscription or mailbox state. Weak-observer exceptions are caught, boundedly diagnosed, and deactivate the observer.

## Session Event Commitment and Close

Agent-to-Session lifecycle commitment is one strong, awaited connection. For each admitted event it:

1. advances Live Session State;
2. notifies weak observers from the committed state;
3. submits required encoding/writing off the event-loop thread;
4. receives the persistence result in the Agent Session's serialized mailbox; and
5. completes in FIFO order before the next sequential commitment completes.

Sequential awaited commitment supplies the order; no generic commit queue is added. A persistence failure does not roll back newer Live Session State. It records a stable error, rejects new prompts, and remains part of the Close obligation. Model-stream sinks and Agent-to-Session commitment are strong, backpressured, awaited connections. Session-to-TUI, status, and diagnostic subscriptions are weak and may perform only bounded value work or mailbox sends. Any awaited host work must be a separately named strong capability.

Session Close is idempotent. It stops admission, requests cancellation of active cancellable user work, and waits for every admitted prompt, compaction, process, persistence operation, Session Event Commitment, and admitted callback to reach a terminal outcome before releasing Session resources. A Close request made from an admitted callback returns without waiting on that callback; the asynchronous Close operation becomes terminal only after that callback and all other admitted work quiesce. Close does not abandon admitted history and it does not stop the shared Runtime.

Final application Close stops application admission, closes and quiesces the current Agent Session by the same rule, drains admitted Runtime work, then stops the event loop and worker pool and joins owned resources. Close progress uses reserved capacity. No resource is destroyed while an admitted operation or callback can still use it.

This does not add a write-ahead log, per-entry `fsync`, rollback transaction, or durability stronger than frozen pi behavior. The known reentrant-subscription, compaction-triggered Close, unsubscribe-during-run, and throwing process-output regressions must be covered before the Runtime refactor is complete.

## Considered options

- Expose Boost.Asio awaitables, executors, and cancellation machinery through Owner Interfaces: rejected because implementation machinery would determine cross-Owner contracts and make a Runtime replacement architectural drift.
- Use unrelated ready, callback, coroutine, and blocking operation surfaces: rejected because ownership, error, cancellation, and Close semantics could diverge.
- Put blocking work and Owner state mutation on the same event-loop path: rejected because a full pipe, filesystem call, or persistence operation could freeze input and rendering.
- Add one executor/event loop per Owner or a general scheduler/event bus: rejected because package ownership does not imply execution ownership and the current product needs only one Runtime root.
- Leave queues unbounded, block producers, execute worker work inline, or silently drop ordinary input/events: rejected because overload would become hidden memory growth, deadlock, latency, or lost pi-observable work.
- Use virtual interfaces for every helper or a general capability box for testability: rejected because seams follow real identity and operation families, not tests or speculative extension needs.
- Release resources immediately on Close: rejected because admitted callbacks, process draining, persistence, or coroutine completion could race destruction.

## Consequences

- Ready and pending Owner operations share one allocation-conscious, move-only contract with fatal misuse semantics.
- Blocking/heavy work cannot mutate Owner state directly; all resulting state transitions re-enter the owning serialized domain.
- Overload is explicit, bounded, and distinguishable from domain failure, while reserved work and Close continue to make progress.
- Live Session State may be newer than durable history after persistence failure, but the Runtime never claims rollback and prevents a later prompt from compounding the failure.
- Session replacement reuses one Runtime root without allowing multiple promptable Sessions.
- Concrete scheduler, event-loop, worker, and transport choices remain private.
- [ADR 0011](0011-close-agent-sessions-in-two-phases.md), [ADR 0017](0017-keep-event-subscribers-as-weak-observers.md), [ADR 0020](0020-make-cancellation-a-supported-end-to-end-capability.md), [ADR 0021](0021-let-the-agent-module-own-the-stateful-agent.md), [ADR 0022](0022-bound-agent-input-queues-explicitly.md), and [ADR 0027](0027-keep-prompt-cancellation-with-the-admission-owner.md) remain authoritative for their semantic ownership, cancellation, weak-observer, queue-admission, and two-phase-close decisions. This ADR deepens them with one Runtime and operation shape.

## Superseded clauses

Only conflicting operation shape and injection clauses are superseded; established lifecycle and observable semantics remain authoritative. The older records carry inline notices identifying the exact affected clauses:

- [ADR 0016](0016-require-explicit-cpp-tool-parallel-safety.md): adapter `concurrency()` as the Tool representation (the exclusive-default and scheduling semantics remain passive Tool descriptor state);
- [ADR 0018](0018-make-agent-policy-hooks-awaitable.md): Boost.Asio awaitable signatures, ready-awaitable adapters, and their shape-specific test obligations as the Owner Interface operation shape; hook ordering, move-only ownership, failure boundaries, and weak-observer separation remain;
- [ADR 0020](0020-make-cancellation-a-supported-end-to-end-capability.md): the alternative public narrow cancellation value and exposed Boost.Asio bridge (`std::stop_token`, prompt-scoped ownership, and normal aborted-lifecycle semantics remain);
- [ADR 0021](0021-let-the-agent-module-own-the-stateful-agent.md): obsolete SDK-presentation and external-public-surface terminology (Agent state ownership and the Agent/Session responsibility split remain);
- [ADR 0023](0023-make-sdk-prompt-asynchronous-by-default.md): the authoritative SDK prompt, `prompt_blocking()`, and SDK-specific test obligations; the SDK was removed by ADR 0036 and the nonblocking/quiescent lifecycle is now owned here;
- [ADR 0029](0029-align-models-provider-and-authentication-ownership-with-pi.md): SDK-injected/shared virtual Models Runtime and single-threaded-executor topology; Provider, Model, authentication, refresh, and stream outcomes remain;
- [ADR 0030](0030-share-pi-agent-config-directory-and-credential-store.md): SDK `agentDir` override and injected Models Runtime precedence/assembly; shared-file and live-auth semantics remain;
- [ADR 0032](0032-own-oauth-lifecycle-and-frontend-interaction-division-with-pi.md): SessionFactory/SDK Models Runtime injection and the sole public virtual seam; auth lifecycle and presentation ownership remain; and
- [ADR 0034](0034-own-the-scoped-pi-agent-core-agent-and-agent-turn-capabilities.md): direct Agent dependency on `ModelRuntime::streamSimple`; the AI-owned `ModelStream` preserves its option, event, retry, cancellation, and terminal semantics.

## References

- Architecture and Runtime spec [#439](https://github.com/lanshengzhi/cpp-coding-harness/issues/439), decisions 14–29 and 35.
- Application behavior authority [#396](https://github.com/lanshengzhi/cpp-coding-harness/issues/396).
- pi C++ parity map [#2](https://github.com/lanshengzhi/cpp-coding-harness/issues/2).
- Frozen pi authority commit `83114817c68f5413e4d7ba6d7003ddc511cd31d2`.
