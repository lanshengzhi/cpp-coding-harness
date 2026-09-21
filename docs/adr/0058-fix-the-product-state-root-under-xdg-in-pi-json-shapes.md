---
status: accepted
---

# Fix the product state root under XDG in pi's JSON shapes

Pike's user-level state needs one predictable location that nothing can reroute, in a shape the
repository already speaks. Issue [#754](https://github.com/lanshengzhi/cpp-coding-harness/issues/754)
follows a rejected TOML direction (#753) whose cost was a hand-rolled 584-line parser, a writer
upgrade for nested settings, a null encoding, and comment loss on every rewrite — while the
already-pinned Glaze dependency ships `glz::read_toml`/`glz::write_toml` unused, and while every
state file is machine-written and therefore gains nothing from TOML's human-friendly surface.

## Decisions

- **Fixed Agent Config Directory**: `$XDG_CONFIG_HOME/pike/agent`, falling back to
  `$HOME/.config/pike/agent` when `XDG_CONFIG_HOME` is unset; an unresolvable home yields empty
  paths. No environment variable relocates the root: `PIKE_CONFIG_DIR`, `PIKE_CODING_AGENT_DIR`,
  and `PIKE_CODING_AGENT_SESSION_DIR` are never read. `--session-dir` and `settings.sessionDir`
  remain session-storage overrides only. pi's own tree is never consulted (ADR 0053).
- **pi's file vocabulary, in JSON**: `auth.json`, `models.json`, `settings.json`, `trust.json`,
  `keybindings.json`, plus `sessions/` (JSONL transcripts), `skills/`, `prompts/`, and `themes/`
  (pi's JSON theme documents). The directory is `0700`; the state files are `0600`.
- **One state format**: the tree carries no TOML reader or writer. The bundled default model
  catalog is one embedded JSON document (`src/ai/DefaultModelsJson.hpp`) parsed with the existing
  support JSON reader, replacing the C++ catalog translation units. `thinkingLevelMap` expresses
  "explicitly unsupported" as JSON `null`.
- **No compatibility machinery**: no legacy-location reads, no extension-driven choice between two
  formats, and no fallback retry. A parse failure is reported rather than reinterpreted.
- **The catalog stays data**: tuning a default model edits the embedded document rather than C++
  catalog code; `input` and `compat` remain C++ provider behavior rather than catalog fields.
- **Session affinity injection**: request dispatch injects `x-opencode-session: <session_id>` for
  OpenCode Go when a session id is present and the header is absent.
- **`pike import`** copies a pi tree into the fixed root without format conversion; the copy stays
  readable because the state files remain JSON.

## Retires

- ADR 0002's `getAgentDir`-style override clause (its single-path-module and `settings.json`
  vocabulary decisions stand).
- ADR 0030's shared-pi-directory and live-credential-interop clauses, already contradicted in
  practice by ADR 0053 and by the fixed root.
- ADR 0031's shared-file path clause (its two-scope merge, surgical write, and field-subset
  contract stands).
- Issue #753's TOML decisions: the `models.toml`/`auth.toml` names, the empty-string null encoding,
  and the in-tree TOML parser. #753's provider-onboarding subject carries over to #754.

## Considered options

- **TOML through the pinned Glaze dependency**: rejected because every state file is machine-written;
  the only human-authored, comment-bearing document is the bundled catalog, which does not justify a
  second repository-wide format and a null-encoding rule for everything else.
- **The hand-rolled in-tree TOML reader**: rejected under `CODING_STANDARDS.md` §16.2 — an
  already-pinned dependency provided the operation — and because it dragged a writer upgrade and
  comment loss behind it.
- **Keeping the JSON files behind the existing environment overrides**: rejected because the
  overrides served test isolation only; isolation moves to `HOME` for subprocess tests and to the
  existing `ModelRuntimeOptions.agent_dir` injection in-process.
- **pi's `agent/` layout with `.toml` file names**: rejected because a half-pi, half-pike vocabulary
  costs every reader a second look and buys nothing the state files use.

## Consequences

- Documents state one root and one file set; `CONTEXT.md`, `docs/usage.md`, and `docs/keybindings.md`
  lose their resolution-order paragraphs.
- Test isolation uses `HOME` (subprocess) and `agent_dir` injection (in-process); the override test
  cases disappear with the overrides.
- The default catalog stays data-driven while the repository keeps exactly one serialization format.
- An existing installation is not read: users re-enter credentials and settings, point
  `pike import --from <old directory>` at it, or move the files by hand.
