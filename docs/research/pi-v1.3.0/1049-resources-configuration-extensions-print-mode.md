# #1049 audit: resources, configuration, native extensions and print mode

## Baseline and scope

- Baseline: `pi-v1.3.0` at `71023aec1546d6d677a65669c5665c0927ce0dd0`.
- Platform: Linux x86-64, glibc.
- Authority: [ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md), [ADR 0066](../../adr/0066-pi-capability-scope-decisions-and-open-questions.md), umbrella [spec #1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044).
- This ticket audits the public Session/CLI boundary. It adds observation fixtures and findings only; it changes no production code, no shared evidence infrastructure, and no issue state.
- `packages`/`extensions` resource markers, JSON/RPC print mode, arbitrary TS/JS extension execution, and theme watching remain governed by ADR 0036 G1/G4/G5 and are excluded from this audit.
- Every observation below is `upstream-captured`; none is a `verified` C++ parity claim. The C++ column is source-and-test evidence, not an accepted comparison.

## Captured artifacts

All captures ran against a private checkout of the frozen revision with a controlled `HOME`, agent directory, temp workspace and faux model. The manifest records the generator, environment, artifact sizes/hashes and placeholder-normalized paths.

| Artifact | Scenarios | Bytes | SHA-256 |
|---|---:|---:|---|
| `fixtures/pi-v1.3.0/audit-1049/prompt-expansion.json` | 4 | 3682 | `d2fa573a4085fa34992b0d5c78b819e3e44c02b10999fbb638b45f4872473038` |
| `fixtures/pi-v1.3.0/audit-1049/resource-selection.json` | 6 | 5712 | `75da96144f6eae5d88ef23261835e5869d8082fa955dd42a72ff02e7fca9d1ab` |
| `fixtures/pi-v1.3.0/audit-1049/extension-run.json` | 2 | 5753 | `4c7b9e527f354178555c9a030c0b75972e4491b7f92998cf13e0ddb05c137b62` |
| `fixtures/pi-v1.3.0/audit-1049/configuration-schema.json` | 9 | 9579 | `a7191ccc7c8c9f5f45d90a951b6d6c661c3e1531a53d6feed9aa395a987adeb8` |
| `fixtures/pi-v1.3.0/audit-1049/print-mode.json` | 2 | 2093 | `0cc590201da5f12ea8c8d1347bba6221e76694455316759a9b3018dd52e4c043` |

Manifest SHA-256: `82b0ef09dc628dc2ae2107faa719998ccfaaf0fcd6ca4ad9ef78e96c06a022bf`.

The generator is `fixtures/pi-v1.3.0/audit-1049/capture-1049.mts`; the print-mode scenario is a real child process (`print-mode-child.mts`) so stdout, stderr and the process exit code are recorded rather than approximated. The artifact envelopes use `pike/pi-v1.3.0-audit-evidence/1`; the capture also asserts the registry schema, baseline revision, clean source tree, platform and family uniqueness before writing.

## Findings

### Prompt templates

Upstream parses command arguments before substitution. A quoted empty argument is a real positional value: `""` and `''` produce `[""]`, and later positionals do not shift. With the template `Review $2. Focus: ${1:-correctness}.`, both `/review "" src/index.ts` and `/review '' src/index.ts` expand to `Review src/index.ts. Focus: correctness.` The session-level path produces the same user text, a system message with an empty text `content` but a `sections` object, no remaining template command in the request, and an idle session after the response.

C++ diverges in `src/coding_agent/prompt/PromptTemplateExpander.cpp:parse_command_args`. It only appends a token when `current` is non-empty, so `""` disappears; the same input becomes `$1 = src/index.ts`, `$2` empty, and the default is not used. The existing test at `tests/coding_agent/PromptTemplateProcessingTest.cpp:85-90` encodes the shifted result (`[][ten]` for a `''` input) and calls it “matches pi positional and slice edge cases.” That test cannot detect the v1.3.0 behavior.

C++ covers numeric positionals, `$@`, `${N:-default}`, `${@:N}` and `${@:N:L}`. It does not cover upstream's `${@:-default}` / `${ARGUMENTS:-default}` collection-default forms. The parser fix should be limited to token retention plus those two forms; the existing no-recursive-expansion contract is correct.

### Resource selection

Upstream candidate order is `AGENTS.override.md`, `AGENTS.md`, `AGENTS.MD`, `CLAUDE.md`, `CLAUDE.MD`, first existing regular file per directory, ancestors root-to-leaf. The capture writes all candidates and observes `AGENTS.override.md` winning while `tree/AGENTS.md` and `tree/nested/AGENTS.md` both load.

C++ `src/coding_agent/ProjectResourceLoader.cpp:38` has only four names and starts at `AGENTS.md`; `AGENTS.override.md` is absent. `tests/coding_agent/ProjectResourceLoaderTest.cpp:1086` only asserts that `AGENTS.md` wins over `CLAUDE.md`, so the suite passes without the override candidate.

Nested worktree shadowing is implemented in C++. The capture uses a real linked worktree with a context file in both the main repository and the nested worktree; only the nested copy loads. A worktree context file symlinked to the main repository file also loads once. C++ tests at `tests/coding_agent/ProjectResourceLoaderTest.cpp:1181-1260` cover synthetic git metadata, including the shadow, no-shadow and same-filename-only cases.

Path modifiers were observed through upstream `resolvePath`/`isLocalPath`:

| Input | Upstream observation |
|---|---|
| `~/templates` | resolves under the controlled home |
| `~/templates` with tilde expansion disabled | resolves relative to the workspace |
| `templates` | workspace-relative |
| Unicode-space-padded path | normalized and trimmed |
| `@templates` with default options | `<workspace>/@templates` (leading `@` retained) |
| `@templates` with `stripAtPrefix` | `<workspace>/templates` |
| `file:///home/templates` | `<workspace>/home/templates`; `isLocal` true |
| `git:` / `npm:` prefixes | `isLocal` false and `resolved` null |

C++ `src/agent/harness/WorkspacePathResolution.cpp:resolve_to_cwd` implements tilde expansion, Unicode-space normalization, leading-`@` stripping and absolute/relative resolution, but deliberately does not convert `file:` URLs (ADR 0057). The upstream resource flags go through `resolveCliPaths`, which calls `resolvePath` without `stripAtPrefix`; C++ explicit resource paths use the resolving operation that strips `@`. That is a real boundary difference, not a captured C++ failure yet: record the file-URL exclusion under ADR 0057 and flag the `@` behavior for an owner decision before writing a parity test.

Explicit resource paths are supported for prompts, skills and themes: `--prompt-template`, `--skill` and `--theme` load named resources with zero diagnostics. The upstream plural flags cover the same families; the C++ surface is intentionally singular. `--extensions`/`--no-extensions` are absent under ADR 0036 G1. Package-source prefixes are not consumed by the C++ loader because the package marker is deleted under G4.

### Configuration

Settings schema and runtime are not the same contract upstream:

- `SettingsSchema` accepts the supported shape and unknown top-level keys, rejects `defaultThinkingLevel: "bogus"` and a negative compaction reserve.
- `SettingsManager` reads the supported values from global then project scope and reloads changed values.
- The runtime manager still consumes values that the schema rejects: `defaultThinkingLevel: "bogus"` comes back as-is, and a string `packages` value is spread into characters (`["n","o","t","-","a","r","r","a","y"]`).

C++ `SettingsManager::create`/`reload` (`src/coding_agent/SettingsManager.cpp:382,1067`) implements the model/thinking/session/shell/theme/compaction/retry subset, validates `defaultThinkingLevel` (`tests/coding_agent/SettingsManagerTest.cpp:195`), preserves unknown fields on surgical writes, and reloads both scopes (`:437`). It has no `packages`/`extensions` resource-source fields and no schema artifact. Do not copy the upstream runtime's unvalidated acceptance; use `SettingsSchema` as the validation reference where a C++ schema is needed. The capture exercises the generic `defaultProvider`/`defaultModel` consumer only; it does not restore retired provider/model settings, and no retired-provider key is added to the C++ settings surface.

Keybindings:

- Upstream catalog is 90 actions: 43 `app.*` and 47 `tui.*`.
- Legacy names migrate (`cursorUp` → `tui.editor.cursorUp`); invalid values are dropped; duplicate keys produce conflicts (`ctrl+q` → `["app.interrupt","app.clear"]`); BOM is tolerated; reload replaces the user map.
- The schema accepts an unknown action id with a valid key value. The manager retains unknown ids in the user map but does not install them.
- C++ reads only `<Agent Config Dir>/keybindings.json`, migrates known names, validates values, bounds diagnostics and exposes the 43-action app catalog (`tests/coding_agent/tui/KeybindingsManagerTest.cpp:281`). It diagnoses `unknown_action`/`unavailable_action` rather than retaining an unknown action. Decide whether that hardening is a deliberate divergence; the known-action, BOM, conflict and reload paths match the captured consumer behavior.

Themes:

- Upstream `THEME_TOKENS` has 56 entries: 51 required and optional `scrollbarTrack`, `scrollbarThumb`, `searchMatchBg`, `searchMatchText`, `thinkingMax`.
- `validateThemeJson` accepts a 56-token built-in, rejects an unknown color token, a missing required token (`text`) and a name containing `/`.
- Runtime `loadThemeFromPath` throws a `TypeError` containing `slot` for an unknown token, but accepts a missing required token (55 colors) and a slash name.

C++ `src/coding_agent/tui/ThemeTokens.inc` has 53 tokens; `scrollbarTrack`, `searchMatchBg` and `searchMatchText` are missing. `src/coding_agent/tui/Theme.cpp` requires required tokens and rejects slash names, so it is schema-aligned for those cases; it accepts unknown color tokens and ignores them. The existing test at `tests/coding_agent/tui/ThemeTest.cpp:346` says this is “like pi”, but that is only true of the runtime TypeBox schema, not v1.3.0 `validateThemeJson`; C++ has no distinct published schema. The runtime's missing-token/slash acceptance and its `TypeError` on unknown tokens should not be copied into the parser.

Reload consumers:

- `SettingsManager.reload` re-reads both scopes.
- Keybindings reload re-reads the file and replaces the user map.
- `/reload` re-reads themes, skills, prompts and context files.
- C++ `/reload` text lists keybindings, skills, prompts, themes and context files, and deliberately omits extensions (`src/coding_agent/prompt/BuiltinSlashCommands.cpp:29-32`); no covered command claims extension lifecycle support.
- Upstream theme watching is deferred; `/reload` is the re-read path.

System-prompt and command claims were checked directly. `src/coding_agent/prompt/SystemPromptBuilder.cpp:107-127` lists only pike docs, and `src/coding_agent/prompt/BuiltinSlashCommands.cpp:29-32` explicitly omits extensions from `/reload`; neither claims package/extension/lifecycle support. No affected system-prompt or command claim was found. The exact stale claim is the theme test's “accepts unknown schema members like pi” wording, which is not true of v1.3.0 `validateThemeJson`; it is tracked in the theme row, not as a command claim.

### Native extensions

The capture drives an inline extension through the frozen runner:

- `before_agent_start` is invoked while the session is busy (`isIdle: false`), with `messageType: "user"` for the initial prompt and `"custom"` for the extension-triggered message.
- The handler's returned system-prompt contribution is visible in the request (`hasSystemContribution` checks the injected marker), and its custom message is a `role: "custom"` entry.
- The first run retains the custom contribution across a tool call; the request contains the system prompt, the original user message, the custom message, the assistant tool call and the tool result.
- From idle, `pi.sendMessage(..., {triggerTurn: true})` starts another run; the custom contribution count grows from 1 to 2 in the triggered request and remains through the final assistant message. The session is idle after both runs.

C++ has the approved native Extension Tool Source foundation (`ExtensionToolRegistry`, `register_extension_tools`, `convert_extension_tool`, SessionFactory wiring) with tests in `tests/coding_agent/ExtensionToolSourceTest.cpp:181-319`. It has no general `before_agent_start`, custom-message trigger or extension-driven run seam. The only `before_agent_start` reference is the MCP server-section comment at `src/coding_agent/AgentSessionExecution.cpp:401`; that is not a lifecycle extension API. Lifecycle hooks are `no-decision` under ADR 0066, not a missing part of the approved tool seam; direct TS/JS execution stays excluded.

### Print mode

A real child process produced:

| Scenario | stdout | stderr | exit |
|---|---|---|---|
| length-truncated | `partial answer\n` | `Response was truncated: the model reached its output token limit.\n` | 1 |
| complete | `complete answer\n` | empty | 0 |

C++ `src/coding_agent/cli/PrintMode.cpp:print_final_assistant_text` handles only `Error` and `Aborted`, then prints every text block and returns true. It has no `Length` branch, so a length-truncated response would currently print the partial text and exit 0. The interactive assistant component has a different warning (`Response was truncated before completion.` at `src/coding_agent/tui/AssistantMessageComponent.cpp:520`), which is not the print-mode warning. `tests/cli/PrintModeTest.cpp` has complete/error/aborted/signal coverage but no `length` case. JSON/RPC print mode is excluded by ADR 0036 G1.

## Ledger summary

The complete operation/consumer/status record is `docs/research/pi-v1.3.0/1049-ledger.json` (20 rows). Status vocabulary is `supported|excluded|no-decision` for membership, `implemented|partial|missing|not-applicable` for implementation, and `verified|upstream-captured|unverified|not-applicable` for verification.

| Row | Membership | Implementation | Verification | Audit result |
|---|---|---|---|---|
| `prompt-quoted-empty` | supported | missing | upstream-captured | C++ drops `""`/`''`; existing test encodes the shifted result |
| `prompt-substitution-forms` | supported | partial | upstream-captured | Missing `${@:-default}` / `${ARGUMENTS:-default}` |
| `prompt-template-resources` | supported | implemented | upstream-captured | Discovery/explicit prompts covered |
| `context-file-candidate-order` | supported | partial | upstream-captured | Missing `AGENTS.override.md` |
| `context-worktree-shadowing` | supported | implemented | upstream-captured | Captured real linked-worktree shadow and symlink dedupe |
| `resource-path-modifiers` | supported | partial | upstream-captured | Tilde/Unicode/relative work; `file:` excluded; `@` boundary differs |
| `resource-explicit-sources` | supported | implemented | upstream-captured | `--skill`/`--prompt-template`/`--theme`; no `--extensions` |
| `settings-schema` | supported | partial | upstream-captured | Runtime subset exists; no published C++ schema |
| `settings-runtime` | supported | partial | upstream-captured | Model/thinking/reload work; resource-source fields not consumed |
| `settings-runtime-validation` | excluded | not-applicable | upstream-captured | Do not copy upstream manager's invalid-value acceptance |
| `keybindings-config` | supported | partial | upstream-captured | Known actions/migration/conflicts work; unknown-id policy differs |
| `keybindings-reload` | supported | implemented | upstream-captured | Reload transition lacks a dedicated C++ test |
| `theme-schema` | supported | partial | upstream-captured | C++ has no schema/runtime split; unknown tokens silently accepted |
| `theme-token-set` | supported | partial | upstream-captured | 53 vs 56 tokens |
| `extension-native-tool-source` | supported | implemented | unverified | Native tool seam tests only; no direct upstream capture |
| `extension-lifecycle-hooks` | no-decision | missing | upstream-captured | No `before_agent_start`/trigger seam; not an approved slice |
| `print-mode-text` | supported | implemented | upstream-captured | Complete/error/aborted/signal covered |
| `print-mode-length-truncated` | supported | missing | upstream-captured | No warning/exit-1 branch or test |
| `print-mode-json` | excluded | not-applicable | not-applicable | ADR 0036 G1 |
| `system-prompt-command-claims` | supported | implemented | unverified | No false package/extension claim; stale theme-test wording tracked |

## Consumers and tests affected

| Consumer seam | C++ implementation | Existing test |
|---|---|---|
| Prompt argument parsing/expansion | `src/coding_agent/prompt/PromptTemplateExpander.cpp` | `tests/coding_agent/PromptTemplateProcessingTest.cpp` |
| Resource discovery/context files | `src/coding_agent/ProjectResourceLoader.cpp` | `tests/coding_agent/ProjectResourceLoaderTest.cpp` |
| Local path modifiers | `src/agent/harness/WorkspacePathResolution.cpp` | Workspace-path tests |
| Settings schema/runtime/reload | `src/coding_agent/SettingsManager.cpp` | `tests/coding_agent/SettingsManagerTest.cpp` |
| Keybindings config/reload | `src/coding_agent/tui/KeybindingsManager.cpp` | `tests/coding_agent/tui/KeybindingsManagerTest.cpp` |
| Theme schema/tokens/reload | `src/coding_agent/tui/Theme.cpp`, `ThemeTokens.inc` | `tests/coding_agent/tui/ThemeTest.cpp` |
| Native extension tool source | `src/coding_agent/extensions/ExtensionToolRegistry.*` | `tests/coding_agent/ExtensionToolSourceTest.cpp` |
| Print-mode outcome | `src/coding_agent/cli/PrintMode.cpp` | `tests/cli/PrintModeTest.cpp` |

The capture seeds for the missing tests are the scenario names in the artifacts. In particular, use `prompt-quoted-empty` and `print-mode-length-truncated` as the two behavior tests that would fail on current C++; the resource and theme rows need the smaller candidate/token changes before their tests can pass.

## Included, excluded, unresolved

- Included in this ticket: the five capture artifacts, the capture helpers/child process, the human report and the ledger.
- Excluded by authority: `packages`/`extensions` resource markers (ADR 0036 G4), `--extensions`/`--no-extensions` (G1), JSON/RPC print mode (G1), third-party extension execution and theme watching (G5/ADR 0066).
- Unresolved: extension lifecycle hooks and custom-message-triggered runs; settings `prompts`/`themes`/`skills` path lists; the resource-flag `@` divergence. These are recorded as `no-decision`/blocked rows rather than implemented here.

## Validation

- Upstream tests: in the frozen checkout, `../../node_modules/.bin/vitest run test/prompt-templates.test.ts test/resource-loader.test.ts test/print-mode.test.ts test/keybindings.test.ts test/keybindings-migration.test.ts test/config-schemas.test.ts test/settings-manager.test.ts test/theme-controller.test.ts test/suite/agent-session-model-extension.test.ts test/suite/agent-session-prompt.test.ts` — 10 files, 273 tests passed. The frozen checkout remained clean at the baseline revision.
- Capture: `PI_CHECKOUT=<pi-checkout> PI_BASELINE=pi-v1.3.0 <pi-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1049/capture-1049.mts` — captured 5 artifacts, 23 scenarios.
- Evidence check: `python3 scripts/evidence/audit_evidence.py check --manifest fixtures/pi-v1.3.0/audit-1049/manifest.json` — passed; reported manifest SHA above.
- JSON/layout: all artifacts and the ledger parse; manifest path/hash and generator SHA checks pass.
- No shared registry file or boundary test was edited. The registry record above is the patch request for #1046.

## Recommended next steps

1. Implement the two clear C++ behavior gaps with captured regression tests: retain quoted-empty prompt arguments and add the print-mode `Length` stdout/stderr/exit branch.
2. Add `AGENTS.override.md` to `kContextFileCandidates` and the theme's three missing optional tokens; re-pin the theme unknown-token test wording to the `validateThemeJson` contract rather than the runtime TypeBox schema.
3. Add `${@:-default}`/`${ARGUMENTS:-default}` to the prompt expander after the parser fix; add the keybindings reload transition test.
4. Record the `@` resource-flag divergence and the settings resource-path lists for an owner ruling; do not add package/extension surfaces or a lifecycle API without a new approved slice.
