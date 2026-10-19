---
status: accepted
---

# Align selected Owner packages and Linux capabilities with pi v1.3.0

The owner confirmed this decision after a grilling session covering baseline, package boundaries,
capability membership, storage interoperability and acceptance. Pike advances the selected non-AI
capabilities to frozen pi v1.3.0 and follows its package boundaries for this migration. Durable is
an independent runtime with the complete Linux capability target, rather than a new name for
Pike's existing task implementation. Acceptance of this target does not claim implementation or
verification is complete.

[Spec #1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044) carries the executable
scope and acceptance contract for this decision; [#1045](https://github.com/lanshengzhi/cpp-coding-harness/issues/1045)
records this authority in version control.

## Baseline and comparison contract

The target Named Baseline is **`pi-v1.3.0`**, exact revision
**`71023aec1546d6d677a65669c5665c0927ce0dd0`**. The local reference checkout is `../pi`;
the revision, not its moving working tree, determines comparison outcomes. Supported platform
remains Linux x86-64 with glibc and the repository's strict C++ toolchain.

- Selected capabilities preserve availability, defaults, caller responsibilities, errors, state
  transitions, event ordering and asynchronous outcomes. User-visible text, screens, input,
  cancellation and lifecycle behavior are part of that contract.
- C++ API shape, representation and implementation may differ. They cannot hide a missing
  capability or justify a different observable result. Exact wire and storage contracts require
  exact comparison of their contractual data, without demanding byte-identical terminal output
  or database files where the contract does not require it.
- Target membership, implementation status and verification status are three separate records.
  Membership places a capability in the selected target; implementation records whether the
  configured graph and observable behavior provide it; verification records whether independent
  frozen-pi evidence accepts the comparison. An accepted target may still have an implementation
  or verification gap; it cannot be reclassified Deferred to close the work.
- Outside the complete targets and explicit additions below, new application capabilities need
  an owner membership ruling. Unresolved membership remains No decision, not implicitly Supported
  or Deferred.
- This is one explicit package/baseline advancement. Future pi releases require another audit;
  the Owner graph does not automatically track upstream HEAD.

## Target package boundaries

These are migration targets, not a description of the currently configured graph.

| Package | Target responsibility |
| --- | --- |
| `cch_ai` | Retain the current Magpie discovery, catalog, authentication and inference contract; no overall pi-ai baseline advancement |
| `cch_agent_core` | Agent, agent loop, tool execution, nested calls and tool registry; move application session/history, compaction, concrete environment and built-in tool responsibilities out of core |
| `cch_coding_agent` | Application Agent Session, Models Runtime policy, concrete application tools, CLI, Native TUI and application integrations |
| `cch_tui` | Complete reusable Linux TUI capability target at v1.3.0 |
| `cch_durable` | Independent complete Linux durable runtime, including its own tools and integrations |
| `cch_mcp` | Reusable MCP protocol, client, transports and generic OAuth |
| `cch_codemode` | Reusable sandbox, declarations, script execution, cancellation, replay and generic Remote contracts |
| `cch_chord` | Durable-required Context, document/delta tracking, state observation and related semantics; not the whole upstream Chord product |
| `cch_support` | Pi-neutral C++ mechanics; no separate pi capability-alignment target |

Move application session/history, compaction, concrete filesystem/shell and application tool
implementations out of `src/agent/` to the application owner. Existing durable-specific stores,
journal and task machinery move to the durable owner and are assessed against its full target.
A filename, directory move or old passing test does not prove that the resulting responsibility
matches pi's current contract. Shared code and dependency edges must be assigned from actual
contracts and consumers rather than mechanically moving every historical `harness` file together.

The application retains MCP configuration persistence, session-tool binding, management UI and
codemode tool presentation. Durable owns its own integration with `cch_codemode`, without reaching
through `cch_coding_agent`. Upstream pi-durable does not depend on pi-agent-core; do not invent that
edge merely because Pike's durable code previously lived there. Determine the final acyclic edges
from actual use, and update the manifest, validator/tests and build declarations with each migration.

Product Architecture Contract gates continue to enforce ownership, frontend separation, private
implementation details and the strict no-exception policy. The new Owner packages are repository
libraries with independent callers, working examples and integration tests; an installed SDK,
stable ABI and cross-version C++ source compatibility are not promised.

## Complete durable target and storage

`cch_durable` targets the complete pi-durable v1.3.0 Linux capability surface: Harness,
Conversations, entries, Submissions, durable documents, Session transactions, versioned tasks,
scheduling, recovery, ownership and cancellation, hooks, extensions/registry, observations and
views, tools, compaction, execution environments, images, codemode and tool-search integrations.
Memory, JSONL and SQLite storage are included. Missing task graph, document or other target
capabilities are implementation gaps, not Deferred.

Durable-required dependency semantics are included in delivery. Chord's unrelated plugin/RPC
capabilities do not become targets solely because durable imports Chord. Node host responsibilities
receive native C++ Linux implementations. Cloudflare-specific host/deployment adapters are
excluded; this cannot excuse missing generic storage, transaction, recovery or execution semantics.

Durable and coding-agent remain distinct runtimes, as upstream does. Implementing durable does
not switch `pike --resume` or the application's ordinary Session onto durable storage. Durable
must have real independently callable behavior and examples, rather than test-only components.

New durable JSONL and SQLite storage must support **bidirectional opening and continued operation**
with frozen pi v1.3.0, preserving task, submission, document and recovery state. JSONL is pi-durable's
transactional main/sidecar format, not a relabeled coding-agent transcript. Verify alternating
writers and reopen/recovery scenarios, not just matching table names or record snapshots. Concurrent
cross-process writers are not implicitly promised beyond the upstream storage contract.

Pike is experimental: do not build old-Pike durable import, migration, automatic upgrade, dual-format
reads or fallback compatibility. New stores use the selected format. This decision does not
authorize deleting user data. Ordinary coding-agent session files remain a separate contract.

## Selected capability additions

- **TUI and consumers:** advance the complete Linux target of ADR 0067 to v1.3.0, including OSC 7501
  detection/override and program status reporting; fake/hardware cursor behavior and component
  marker handling; dynamic horizontal padding; selection reset; Herdr hyperlink/image support;
  and the overlay, wheel, shifted-key and fragmented terminal-color reply fixes.
- **MCP and application integration:** accept arbitrary JSON `structuredContent`, including null;
  propagate cancellation through discovery, registration, authorization and token requests; do not
  resend cancelled/timed-out calls after refresh; avoid token refresh during close; reject an
  authorization code whose issuer cannot be verified as required by v1.3.0. Include live usable
  management during connection, on-demand resource access and updated status presentation.
- **Codemode:** include the Linux execution, cancellation and replay contracts: Inline execution
  and interrupt budgets, deterministic `start()` and host result delivery, virtual time/seed,
  ordering/fingerprint, worker shutdown-request versus actual-exit semantics, admission/release
  tied to actual worker exit, schema constraint tags, console markers and updated output/error
  behavior. Include generic Remote execution/exchange and a working Linux host; only the
  Cloudflare-specific deployment is excluded. Native implementations need not copy Node/Bun internals.
- **Tool timing:** use a monotonic clock for tool `execute()` duration, excluding hooks; calls that
  never execute have no duration. Carry `durationMs` through results, events, persistence, resume
  and rendering. This is distinct from the excluded model-response timing.
- **Application:** include `--tools +name/-name`, restoration of active tools on resume and modifier
  precedence; cancellation state on `agent_settled`; render `outputPad`; OSC52 opt-in; settings,
  keybindings and theme schemas for the actually supported fields; and prompt arguments,
  resource/context paths, summary serialization and print-mode exit/warning fixes.
- **Extensions:** provide actual native C++ interfaces and behavior for included hooks, registry,
  tool wrapping, run triggering and prompt/busy-state semantics. Direct execution of third-party
  pi TS/JS plugins is excluded. Codemode's model-written JavaScript remains included.

## Retained product boundaries and explicit exclusions

Retain the Magpie-only model path of ADRs 0068 and 0071–0074, including the retired model-provider
login/logout, provider registry and `models.json` execution surfaces. Retain the XDG Pike state
root of ADR 0058. Do not restore those application behaviors merely because their upstream
implementation lives outside pi-ai.

`cch_ai` does not undergo overall alignment. Passive fields and serialization support needed by
included non-AI capabilities, such as tool duration, may be added. This does not authorize changes
to gateway inference or other AI behavior. Explicit AI exclusions include model-response timing,
cross-model thinking transformation, the final/partial tool-argument parsing fixes and additional
AI retry matching. Record exact affected claims and dependent cases in the inventory; do not use
AI ownership to exclude unrelated tool timing or whole durable subsystems. Classifier and image
model/provider execution are not added by the codemode/durable integrations; those integrations
must reflect the actual model capabilities available through the retained AI contract.

Other confirmed exclusions are:

- Agent-core `streamProxy`'s separate proxy-event protocol. Agent stream injection remains.
- The standalone pi-env SSH bootstrap/remote daemon, telemetry and evals packages. Durable's own
  environment contract/local Linux implementation and codemode's generic Remote contract remain included.
- Cloudflare-specific adapters, native macOS/Windows helpers and third-party TS/JS extension execution.
- Previously excluded application upload/share services and their `sessionShareDestination` setting,
  pi branding animation, installer/updater, Nix/npm distribution and third-party extension package
  management. Do not expose commands, settings or prompt claims without the corresponding capability.
- Installed SDK and stable ABI promises.

These are scope decisions, not permission for a selected capability to retain arbitrary historical
differences. Other capabilities without a membership decision remain No decision. For selected
capabilities, remove historical behavioral exemptions except the retained Magpie/XDG boundaries
and explicit exclusions in this decision. In particular Agent queue defaults/admission follow pi;
MCP HTTP/HTTPS, redirects and authentication follow pi rather than Pike's former TLS-only and
no-redirect policy. This does not relax TLS validation where upstream requires it or change the
Magpie connection contract.

## Migration, evidence and acceptance

1. Record the target and clause-level supersessions before implementation. First migrate package
   ownership with existing behavior, working consumers and focused validation; land subsequent
   behavior changes against the resulting owners. Temporary old behavior is a tracked migration
   obligation, not an accepted final divergence.
2. Inventory frozen exports, public operations, defaults, required consumers and relevant upstream
   tests. Every selected row needs an owner, implementation work and a discriminating comparison
   case; distinguish accepted membership, implementation and verification status.
3. Register/capture `pi-v1.3.0` evidence with provenance and integrity checks. Preserve historical
   bundle identities; do not relabel v1.0.4 observations or silently change the AI baseline default.
   Registry/bundle organization must serve the selected packages without creating an AI alignment claim.
4. Advance unresolved v1.0.4 TUI work to v1.3.0. Recheck upstream behavior and update acceptance
   criteria where it changed. An issue label change is not implementation evidence.
5. Compare the same scenarios in frozen pi and C++, including values, errors, state transitions,
   ordering and cancellation. Use real CLI/PTY and terminal-emulator evidence for relevant UI behavior.
   Durable additionally requires crash/reopen, atomicity/rollback, retry, ownership, submission
   deduplication and bidirectional storage continuation tests.
6. Require focused validation per dependent slice, architecture checks with graph changes, and
   Full Validation for implementation delivery. Old tests or existence checks alone cannot accept
   a new property. Missing evidence leaves the row incomplete; no self-captured golden may stand
   in for an independent upstream observation.

## Superseded and retained decisions

- **ADRs 0005, 0034, 0039 and 0070:** supersede the affected historical ownership placements for
  harness/session/tools and MCP within agent-core/coding-agent. Retain semantic contracts unless
  changed by this baseline, the gate mechanism and eviction of neutral infrastructure from AI.
- **ADR 0053:** supersede package-shape independence for this explicit v1.3.0 migration only;
  retain product-owned gates, frontend separation and deliberate future baseline audits.
- **ADR 0022:** supersede the extra Agent queue-capacity/default/rejection divergence for the
  selected target. Admission follows frozen pi behavior.
- **ADR 0054:** supersede TLS-only/no-redirect restrictions where they prevent selected MCP
  behavior; retain applicable executor/toolchain mechanics and unrelated transport guarantees.
- **ADRs 0024, 0060, 0065 and 0067:** replace selected capability target revisions with v1.3.0;
  retain frozen named evidence/provenance discipline and historical bundle identities. Do not
  advance the cch_ai target or claim that v1.3.0 evidence already exists.
- **ADR 0066:** supersede membership/exemption records that conflict with the complete durable,
  complete Linux TUI, explicit additions and removal of historical behavioral differences above.
  Preserve unrelated exclusions and No decision rows; no blanket full application/Chord claim.
- **ADRs 0038, 0040 and 0042:** retain strict toolchain, serialized lifecycle and no-exception
  requirements; representation choices cannot weaken selected observable behavior.
- **ADRs 0058 and 0068–0074:** retain XDG and Magpie decisions, with MCP generic ownership moving
  to `cch_mcp`. The no-old-Pike-migration decision here concerns durable storage, not an instruction
  to remove the existing pi configuration import command.

Implementation issues must cite this target and update the machine-readable graph with actual
code changes. This ADR alone does not alter the manifest, register captured evidence, create
tracker issues or establish that the new Owner packages already exist.
