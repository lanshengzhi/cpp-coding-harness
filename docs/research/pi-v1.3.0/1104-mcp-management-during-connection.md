# #1104 — manage MCP servers while connections are pending

## Scope

Batch B46 / F14 (management slice), under #1044 and ADR 0075. Named Baseline:
`pi-v1.3.0`, revision `71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64/glibc.
The sibling #1105 owns on-demand resources and tool binding; this delivery covers the
management surface only.

Frozen evidence: `fixtures/pi-v1.3.0/audit-1104/management-during-connection.json`
(four observations, manifest SHA-256
`a296b43eebec48c5c10512c4fa05e9c0e249bb3c5d74508bb5b671f7aa1d86d8`, generator
`capture-1104.mts`). Registered in `fixtures/pi-v1.3.0/baselines.json`.

## Delivered behavior

- **Session creation never blocks on a handshake.** `McpSessionManager::start()` queues each
  enabled server's connect as a detached task (pi `session_start`'s tracked
  `backgroundActions`) and returns; a late connection after `close()` or a disable is closed
  and dropped instead of registering tools on the released Agent (pi `startConnection`'s
  `isCurrent` drop). Pending servers publish a `connecting` connection snapshot, matching
  pi's immediate `{state: "connecting"}` publish.
- **The management panel is live during a connect.** `/mcp` opens `McpManagerView`
  immediately; the servers list, a server's menu, and enable/disable/exposure actions all
  answer while another server's handshake is in flight, and the panel re-presents on the
  manager's change notifications as the connects settle.
- **Non-TUI subcommands await the settle.** `login`, `logout` and `reconnect` run
  `waitForPendingConnections` before the pick (pi `registerCommand("mcp")`'s `await
  pending`), so a `needs-auth` OAuth server that only appears once its connect answered is
  picked correctly.
- **First prompt is bounded, later turns pick up external sign-ins.** `run_prompt` awaits
  `wait_for_direct_servers` (pi `before_agent_start`: `direct`-exposure servers only, pi's
  10 s `startupWaitMs`, one-shot latch, verbatim timeout notice) and then `on_turn_start`
  (pi `turn_start` → `reconnectSignedIn`): a server that entered `needs-auth` snapshots its
  stored token fingerprint and is reconnected on the next turn when that fingerprint changed
  — a sign-in performed in another process is picked up without a manual reconnect.
- **Loadout replay survives a late connect.** `apply_exposure` declares a name that the
  replayed loadout restored (pi `_isActivatedOnRegistration`) and `restore_loaded_tools`
  accepts a not-yet-registered name as pending, so reopening a session whose servers are
  still connecting restores the same offered set once registration lands.

## Comparisons

| Frozen scenario | C++ comparison |
| --- | --- |
| the /mcp manager lists connecting servers and completes a sign-in while another server connects | `tests/coding_agent/tui/McpManagerFlowTest.cpp` — `/mcp panel stays live while a server connect is pending and subcommands await the settle`; companion `tests/coding_agent/McpSessionManagerTest.cpp` — `sign-in completes for one server while another is still connecting` |
| a sign-in performed outside the session is picked up on the next turn | `tests/coding_agent/McpSessionManagerTest.cpp` — `on_turn_start reconnects a server signed in through another process` |
| closing the session while initialize is pending returns without waiting | `tests/coding_agent/McpSessionManagerTest.cpp` — `close during a pending connect returns promptly and drops the late connection` |

The Session-level discriminating case is
`tests/coding_agent/McpSessionIntegrationTest.cpp` — `agent session creation returns while a
gated MCP server connects and the manager stays manageable`: it drives the real
`create_agent_session_async` through `AssemblyOverrides::mcp_manager` over a gated connection
factory and rejects the blocking-creation design (creation returns with `pending_start_count()
== 1`, the snapshot reads `connecting`, `search_tools` is empty; after the gate opens the tool
is registered and offered to the model on the next prompt; disable hides it and enable
reconnects it).

## Validation

- Focused Validation: `ctest --preset vcpkg -L issue1104`: 12 selected, 12 passed.
- `[mcp]` shard: 220 cases / 4116 assertions (coding-agent shard) and 42 cases / 524
  assertions (interactive shard).
- AddressSanitizer + UBSan build (`vcpkg-asan-ubsan`): the 12 `issue1104` cases and the 5
  `issue1095` loadout cases pass with no sanitizer report; the interactive `[mcp]` selection
  passes 42/42. The sanitizer runs found and fixed one real defect: a
  `notifications/tools/list_changed` landing after session close registered tools on the
  released Agent; the listeners now bind the manager weakly and `register_tools`/`hide_tools`
  stop at `closed_`.
- Formatting: `scripts/format-check.sh origin/main` — clean. Warning gate: `cch_warning_gate`
  passed.
- Full Validation: `ctest --preset vcpkg -j2` — 3,987 selected, 3,984 passed, 3 failed, exit 8.
  The three failures are pre-existing and reproduce identically on the stashed baseline at this
  branch point: `Native TUI product run entry starts, renders, and restores its selected root`,
  `Native TUI queues batched submissions across deferred prompt dispatch`, and
  `Native TUI restores pending input before abort and accepts it after quiescence` (each fails
  on a `messages.size() == 2` message count). Both consolidation checks pass
  (`cch_pi_v130_consolidation_check`, `cch_pi_v130_consolidation_boundaries`), as does
  `cch_warning_gate`. One load-sensitive terminal `postdrain EIO` case failed once under `-j2`
  and passed serially and three times in a row when rerun.

## Capability status

Delivers `cch_coding_agent:mcp-management-ui` and
`cch_coding_agent:mcp-external-signin-pickup` as whole rows; the receipt is
`1104-management-during-connection` in `1058-deliveries.json`.

## Remaining boundary

- pi's non-TUI `/mcp` branch (`ctx.hasUI === false` → `ui.notify(formatStatus())`) has no
  Pike seam: print mode never dispatches slash commands (`pike --print "/mcp"` falls through
  to the model), so the status report is only reachable from a live host without a manager.
  The frozen observation "plain /mcp status waits for pending connections and then reports
  settled states" is therefore artifact-covered, not compared.
- `cch_coding_agent:mcp-reconnect-tools-changed` stays open: this delivery re-registers tools
  on a reconnect and on `tools/list_changed`, but its own frozen comparison set is #1105's.
- #1044 remains open.
