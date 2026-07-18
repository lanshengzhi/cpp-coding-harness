# Default session storage in the Agent Config Directory

Category: enhancement
Status: ready-for-agent

## Problem Statement

Agent Session histories currently default to a project-local path derived from the process working directory. That behavior writes sensitive harness state into workspaces, depends on workspace writability, and selects the wrong location when the CLI's explicit workspace differs from the process working directory. The SDK has no corresponding default at all: callers must choose either a create path or a resume path, and it cannot create an in-memory Agent Session.

The current path and identity rules are also split across adapters. The CLI invents an automatic file name separately from SessionFactory's Session ID, while the SDK represents mutually exclusive targets with independent optional fields. This makes the behavior harder for users to predict and harder for agents to navigate safely.

## Solution

Treat default persisted Agent Session history as user-level state associated with a workspace. CLI and SDK default creation will use a workspace-keyed directory under the Agent Config Directory, mirroring pi's session ownership and readable directory layout. One canonical physical workspace value will drive execution, session metadata, and default storage so symbolically linked aliases share one session directory.

The CLI will support pi-shaped session-directory overrides and an explicit non-persistent mode. The SDK will expose one aggregate-friendly variant for default persistence, explicit creation, resume, or in-memory operation, and will represent the absence of a session file with an optional path. Explicit paths remain valid outside the default root.

Path calculation will remain side-effect free. SessionFactory will remain the single assembly-policy seam, while a narrow Session Store capability will hide the physical difference between JSONL and in-memory operation. Persisted sessions will retain the project's current incremental durability semantics rather than adopting pi's delayed first flush.

## User Stories

1. As a CLI user, I want new sessions to be stored outside my workspace by default, so that running the harness does not modify project contents.
2. As a CLI user, I want default sessions grouped by workspace, so that histories from unrelated projects remain distinguishable.
3. As a CLI user, I want an explicit workspace to determine the default session directory, so that launching the harness from another directory does not misplace the transcript.
4. As a CLI user, I want symbolic-link aliases of the same physical workspace to share session storage, so that one project does not acquire duplicate history collections.
5. As a CLI user, I want readable workspace directory keys matching pi, so that I can inspect the Agent Config Directory without decoding opaque hashes.
6. As a CLI user, I want automatic session file names to contain a UTC timestamp and UUID, so that files are sortable and uniquely identifiable.
7. As a CLI user, I want the file name UUID to match the Session ID in the header and runtime state, so that I can correlate a file with diagnostics and frontend output.
8. As a CLI user, I want `--session-dir` to override automatic storage for one run, so that I can redirect a group of sessions without naming every file.
9. As a CLI user, I want an environment variable to override the automatic session directory, so that automation can configure storage without editing commands or settings.
10. As a CLI user, I want User Settings to provide a default session directory, so that I can keep a stable personal storage preference.
11. As a CLI user, I want explicit flags to take precedence over environment and User Settings, so that one-run intent is deterministic.
12. As a CLI user, I want relative session-directory overrides resolved against the final workspace, so that their meaning does not depend on my shell's launch directory.
13. As a CLI user, I want absolute paths and home-relative paths accepted, so that I can place sessions in a known user-controlled location.
14. As a CLI user, I want `--no-session` to create an Agent Session without filesystem persistence, so that disposable work leaves no transcript behind.
15. As a CLI user, I want explicit create and resume paths to remain usable outside the default root, so that I retain precise control over portable or temporary transcripts.
16. As a CLI user, I want to resume an explicitly named project-local file, so that a valid transcript remains usable without an automatic legacy-location fallback.
17. As a CLI user, I want missing or unwritable default storage to fail clearly, so that I never mistake an unpersisted interaction for a durable one.
18. As a CLI user, I want storage errors to identify the target and reason, so that I can choose `--session-dir` or `--no-session` deliberately.
19. As a security-conscious user, I want default-created directories and files private to my account, so that sensitive transcripts are not exposed through permissive defaults.
20. As a user of a pre-existing custom directory, I want the harness to avoid changing that directory's permissions, so that it does not take ownership of a location I manage.
21. As an SDK host, I want session creation with no explicit target to persist under the Agent Config Directory, so that the default matches the CLI and pi.
22. As an SDK host, I want a single session-target value, so that invalid combinations of create, resume, and in-memory requests cannot be expressed.
23. As an SDK host, I want to request an explicit new-session path, so that my application controls a durable transcript's exact location.
24. As an SDK host, I want to request an explicit resume path, so that my application can reopen a known Agent Session.
25. As an SDK host, I want to request an in-memory Agent Session explicitly, so that no temporary file or directory cleanup is required.
26. As an SDK host, I want an optional session path, so that in-memory operation is represented without an empty-path sentinel.
27. As an SDK host, I want CLI-only session-directory settings ignored by SDK defaults, so that machine-local CLI preferences cannot silently redirect my application's data.
28. As an SDK host, I want the Agent Config Directory override to affect default SDK persistence, so that my application can isolate all harness user state under one root.
29. As an SDK host, I want live history and Session ID to remain available for an in-memory Agent Session, so that lack of persistence does not remove normal runtime introspection.
30. As a user, I want failed session assembly to leave no session file, so that failed startup does not publish misleading durable state.
31. As a user, I want a successfully assembled persisted session to create its header before prompting, so that the session has a durable identity immediately.
32. As a user, I want completed user history persisted even if a later provider operation fails, so that failed interactions remain diagnosable.
33. As a maintainer, I want path computation to be free of filesystem side effects, so that policy can be tested without creating state.
34. As a maintainer, I want one canonical workspace value reused across execution, metadata, and storage, so that adapters cannot drift.
35. As a maintainer, I want one SessionFactory assembly pipeline for CLI and SDK, so that security and persistence decisions remain centralized.
36. As a maintainer, I want Runtime to depend on a narrow Session Store capability, so that persisted and in-memory sessions follow one prompt and event flow.
37. As a maintainer, I want concrete JSONL behavior hidden behind the storage seam, so that physical I/O does not leak into session orchestration.
38. As an agent implementing the feature, I want obsolete target fields and old-location compatibility logic removed, so that the active contract is the only code path to understand.
39. As an agent maintaining the project, I want current behavior and module routing updated with the implementation, so that future work reaches the authoritative seams quickly.
40. As a future session-discovery implementer, I want the workspace-key and directory policy available through one reusable seam, so that listing does not duplicate path logic.
41. As a CLI user, I want the same session-target choices in text, JSON, RPC, and REPL modes, so that persistence does not depend on presentation mode.
42. As an interactive user, I want `/session` to identify an in-memory Agent Session explicitly, so that an absent path is never displayed as an ambiguous empty value.

## Implementation Decisions

- The accepted session-storage ADR is authoritative. The implementation is a hard switch because this is an experimental project: do not add migration, legacy scanning, fallback reads, deprecated fields, compatibility flags, or dual behavior.
- The Agent Config Directory path module remains the single source of user-level roots and gains the derived sessions root. It must not create directories while answering path queries.
- A session path policy module owns workspace-key encoding, automatic file identity, and automatic path composition. Its computations are pure values and do not inspect CLI flags, User Settings, or environment variables.
- The workspace is resolved once to a canonical physical directory before session-target resolution. The same value is used for the execution environment, Session Metadata, default path encoding, and explicit-workspace resume validation. Existing workspaces that are symbolic-link aliases therefore share one key.
- Workspace keys use pi's readable encoding: remove the leading root separator, replace path separators and drive separators with hyphens, and surround the result with double hyphens. Do not add a hash suffix or a legacy alternative.
- Default persisted paths have the conceptual layout `<Agent Config Directory>/sessions/<encoded workspace>/<UTC timestamp>_<Session ID>.jsonl`.
- New Session IDs use standard randomly generated UUIDs. One creation identity supplies the Session ID, the header timestamp, the automatic file name, and returned SDK metadata. The file-safe timestamp is the same UTC ISO-8601 instant with punctuation normalized as pi does.
- CLI target normalization supports default persisted, explicit new, explicit resume, and in-memory targets. `--no-session` is an explicit target rather than an empty path and is incompatible with explicit create or resume intent. The normalized target applies uniformly to text, JSON, RPC, one-shot, and REPL frontends.
- CLI automatic-directory precedence is `--session-dir`, then `CCH_CODING_AGENT_SESSION_DIR`, then User Settings `sessionDir`, then the Agent Config Directory default. An explicit create or resume path remains the more specific target and is not rewritten by automatic-directory policy.
- Absolute automatic-directory overrides remain absolute. A leading home marker is expanded. Relative overrides are resolved against the final canonical workspace, not the process working directory.
- User Settings add an optional `sessionDir` value using pi's vocabulary. It is a CLI session-storage preference, not a provider setting and not an SDK default input.
- SDK creation replaces independent create/resume optionals with one aggregate-friendly variant containing four value alternatives: default persisted creation, explicit persisted creation, explicit resume, and in-memory creation. Default construction selects default persisted creation.
- SDK-specific normalization does not consume `--session-dir`, `CCH_CODING_AGENT_SESSION_DIR`, or User Settings `sessionDir`. Its default persisted target derives only from the resolved workspace and Agent Config Directory. Explicit create, resume, and in-memory targets bypass that default.
- Public and internal session-path results become optional. Persisted create and resume return the actual file path; in-memory sessions return no path. Do not preserve an empty path as a sentinel. Frontend session information renders the in-memory state explicitly rather than formatting an empty path.
- In-memory sessions still have Session Metadata, a UUID Session ID, Live Session State, prompt processing, event delivery, and close behavior. They create no sessions root, workspace directory, or JSONL file.
- Runtime depends on one narrow Session Store capability rather than directly on a JSONL concrete type or a nullable store. The capability supports the persistence operations Runtime requires and returns an optional path. JSONL and in-memory implementations preserve one runtime control flow; the in-memory implementation does not duplicate Live Session State.
- Tree loading, resume parsing, and JSONL-specific navigation may remain on the existing concrete session machinery; the Runtime-facing capability must stay narrow instead of publishing all concrete methods.
- SessionFactory remains the sole assembly-policy seam. CLI and SDK adapt their product-specific inputs into its private target plan. Low-level stores never read environment variables, User Settings, or CLI configuration and never choose their own locations.
- Path resolution and identity generation are side-effect free. Physical directory and file creation occurs only at the publish stage after provider, workspace, trust, resources, tools, and execution capabilities have passed assembly prerequisites.
- Persisted session publication retains exclusive file creation and rejection of an existing explicit create target. Resume continues to open and append to the selected existing file.
- Storage failure is explicit. Default persistence must not retry in the workspace, switch to another root, or become in-memory. Errors include the attempted path and underlying reason and allow frontends to suggest the explicit directory or in-memory controls.
- On POSIX, harness-owned default sessions roots and workspace directories are created or tightened to owner-only (`0700`), and every newly created session file is owner-readable/writable (`0600`). Existing custom directories are not chmodded, but files created inside them remain private. If a default directory cannot be made private, creation fails. Other platforms use the closest protection supported by existing project facilities.
- Existing containment protections remain in force: exclusive creation, symlink defenses, private-file validation, redaction, and append-only JSONL behavior must not be weakened by the new default root or storage interface.
- Preserve current durability behavior. Failed assembly leaves no session file. Successful persisted assembly publishes a header, and completed user, assistant, and tool-result messages continue to append incrementally. Provider or subscriber failures do not roll back Live Session State or already durable history.
- The implementation updates current user documentation, SDK contract documentation, CLI help, settings documentation, module routing, build target membership if modules move, and examples that currently imply explicit session paths are mandatory.
- The implementation follows current pi source in the sibling checkout for supported session-directory semantics. Intentional differences are limited to canonical physical workspace identity, side-effect-free path computation, and the existing C++ incremental first-flush behavior recorded in the ADR.

## Testing Decisions

- Tests assert externally observable contracts rather than private helper structure. The primary policy seam is session creation through SessionFactory as reached from the public SDK and the CLI adapter; lower-level tests are reserved for path values, file permissions, and storage fault injection that cannot be observed reliably at a higher seam.
- Public SDK tests cover all four session targets: default persisted creation, explicit new creation, explicit resume, and in-memory creation. They verify the optional path contract, Session ID availability, workspace metadata, prompt behavior, close behavior, and absence of filesystem state for in-memory sessions. Built-in session-information coverage verifies that an in-memory session is presented explicitly rather than as an empty path.
- SDK default-persistence tests isolate the Agent Config Directory through an environment guard, create a session in a temporary workspace, and verify that its returned path is under the expected encoded-workspace directory.
- SDK isolation tests set CLI-only session-directory environment and User Settings values and verify that SDK default persistence still uses the Agent Config Directory. A separate Agent Config Directory override test verifies that the SDK default follows that root.
- CLI parsing tests cover `--session-dir`, `--no-session`, invalid target combinations, and help text. They assert normalized intent rather than internal parser library details.
- CLI smoke tests use the fake provider and temporary directories to cover the full precedence chain: explicit directory over environment, environment over User Settings, User Settings over the Agent Config Directory default, and default behavior when no override exists.
- CLI smoke tests launch from a directory different from an explicit workspace and verify that the workspace determines the automatic location. Relative session-directory tests verify resolution against the workspace.
- CLI smoke tests verify that `--no-session` completes a prompt without creating the Agent Config Directory sessions root, a workspace-local sessions directory, or a transcript file. Coverage includes text, JSON, RPC, and REPL paths at the cheapest seam that protects each frontend's target propagation.
- Explicit-path CLI and SDK tests verify that create and resume targets outside the default root remain usable and are not rewritten by directory overrides.
- A no-legacy-fallback test places sessions under the old project-local default, performs automatic creation, and verifies that the old directory is neither scanned nor reused. Explicitly resuming the same valid file remains covered as the general explicit-path behavior.
- Path policy tests use table-driven POSIX and Windows-shaped inputs to protect pi's readable encoding, drive and separator handling, double-hyphen framing, absolute output, and lack of a hash suffix. These tests call a pure seam and verify that no directories are created.
- Workspace identity tests create a real directory and symbolic-link alias where supported, then verify that session creation through either path returns the same encoded workspace directory and canonical workspace metadata.
- Identity tests verify UUID shape, equality between returned Session ID and JSONL header ID, equality between the ID and filename suffix, and derivation from one UTC timestamp. Tests avoid asserting a wall-clock instant beyond format and internal equality.
- Agent Config Directory tests extend the existing derived-path coverage for the sessions root and preserve behavior when the directory override or home directory is unavailable.
- User Settings tests cover a valid `sessionDir`, absent values, malformed types, unknown keys, and retention of existing provider, trust, and resource parsing behavior.
- Session lifecycle tests preserve the established prepare/publish guarantee: any failure before publish leaves no session file. A publish-stage path or permission failure returns a session error containing the target and does not create a fallback transcript.
- Existing incremental-persistence tests remain authoritative. Add or retain a provider-failure case proving that a completed user message is durable and Live Session State remains available after the later failure.
- Storage capability contract tests run equivalent append/path expectations against persisted and in-memory implementations where behavior is shared. JSONL serialization and tree mechanics remain covered by their existing focused tests rather than duplicated through Runtime tests.
- POSIX permission tests verify `0700` for default-created directories and `0600` for files. A custom-directory test starts with broader permissions, verifies they are unchanged, and verifies the new file is `0600`. Platform-specific checks are conditional rather than pretending POSIX modes exist everywhere.
- Security regression tests retain symlink-parent rejection, exclusive file creation, append-only operation, and private-file checks. The new sessions root must not weaken these established behaviors.
- Architecture tests verify that the SDK target remains a passive value variant, Runtime depends on a narrow storage seam, concrete implementation details stay out of unrelated public headers, SessionFactory remains the only Runtime construction seam, and CMake dependency direction remains valid.
- Documentation checks verify CLI help, SDK examples, settings vocabulary, relative links, tracker state, and module-routing accuracy.
- No live provider, API key, or network tests are required. All end-to-end coverage uses the fake provider and temporary filesystem state.
- Prior art includes the existing Agent Config Directory path tests, User Settings loader tests, CLI parse and smoke tests, SDK session creation tests, SessionLifecycle prepare/publish and persistence-failure tests, JSONL store security tests, and public-boundary architecture tests.

## Out of Scope

- Automatic migration, copying, fallback reads, or discovery of sessions under the old project-local default.
- Compatibility aliases for the old SDK create/resume optional fields or empty-path sentinels.
- Session listing for one workspace or all workspaces.
- Lookup, resume, or fork by Session ID.
- Interactive session selection, renaming, deletion, trash integration, export, or sharing.
- Changing the current explicit `--session` create and `--resume` append product semantics beyond integrating them with the new target model.
- Adopting pi's delayed first flush or discarding sessions that have no assistant response.
- Changing the JSONL schema, Session Topology, compaction, branching, leaf semantics, or resume context reconstruction.
- Project-local settings support or allowing a project to choose the SDK's default session location.
- Adding collision-resistant hashes to readable workspace keys.
- Changing provider resolution, project trust policy, resource loading, tool execution, event delivery, or prompt semantics except where session-target values must flow through existing assembly.
- Session cleanup or retention policy for workspaces that no longer exist.
- ABI compatibility, migration tooling, or support for binaries built against the previous experimental SDK contract.

## Further Notes

- This spec implements the accepted decision in [ADR 0003](../../docs/adr/0003-store-default-sessions-in-agent-config-directory.md) and the resolved [pi-parity decision 02](../pi-cpp-parity/issues/02-decide-session-storage-location.md).
- The previously accepted Agent Config Directory and shared SessionFactory decisions remain authoritative and complementary.
- The test seams were already confirmed during the preceding grilling session: SessionFactory is the shared assembly seam, CLI and SDK retain distinct adapter contracts, and physical persistence sits behind one narrow Session Store capability.
- The local pi source authority is the sibling `../pi` checkout. Implementers must compare the current coding-agent config, session manager, SDK, CLI, and session documentation there before changing parity-sensitive behavior.
- After this spec, use `/to-tickets` to split implementation into dependency-ordered work. Do not implement directly from the wayfinder decision ticket.
