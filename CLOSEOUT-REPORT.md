# Close-out report — #886 (spec #882: MCP + codemode full parity with pi v1.0.4)

**Branch:** `pike/mcp-codemode-882` (base tip `bfd3ae244`, an amend of `7c031e34f` — identical modulo
a formatting pass on four TUI/test files).
**Method:** full-row sweep of every MCP/codemode capability surface in ADR 0066 against the landed
branch, the frozen `pi-v1.0.4` bundle (`fixtures/pi-ai/v1.0.4/mcp-codemode/`), and the implementation
notes (`notes/882-mcp-codemode-parity/*.md`); the ACTIVATION-REPORT's Remaining-gaps sections were the
starting suspect list. Authority for disputed details was the pi checkout at `7c10bd43`.

## Verdict

**No unrecorded MCP/codemode behaviour difference remains.** Every residual found in the sweep is
either fixed in this close-out or recorded as a Deferred row in ADR 0066 with its exact pi pointer.

The capability surfaces enumerated and confirmed against landed reality: MCP transports
(stdio + streamable-http, TLS-only), the JSON-RPC protocol (notifications/cancelled, GET stream,
ping/roots, pagination), tool conversion (`mcp__<server>__<tool>` + hash suffix, result schema), the
three resource tools, the exposure policy and activation model (ensureDiscoveryActive,
defaultActive:false, verbatim warning, withdrawal-as-hidden), `mcp.json` global+project
persistence and write half, the `/mcp` panel + `pike mcp` CLI, OAuth (RFC 9728 discovery, DCR,
PKCE, CIMD, mcp-auth.json, auth.json migration, sign-in triggers), lifecycle states, the
`mcp_servers` prompt section; codemode (inline tool + grammar constraint emission, source.ts
@options, tools.* routing + nested calls, worker-thread execution, guest packaging, text|image
output items, settings keys).

## Fixed in this close-out (2 items, both small and focused)

1. **OAuth sign-in cancellation wording** (slash-wiring residual): the production driver now returns
   pi's info-level `Sign-in cancelled.` for the cancelled error (`extensions/mcp/index.ts:628`), so
   the landed /mcp flow's pi check matches; other failures keep the manager's pi-verbatim
   `Sign-in failed: <message>`. The `pike mcp login` CLI now renders pi `login`'s two failure lines
   (`was cancelled or not completed within N seconds.` / `failed: <message>`,
   `extensions/mcp/cli.ts`).
2. **Codemode script value shape** (ACTIVATION-REPORT residual 6): a script call now resolves to
   pi's whole CallToolResult object minus `_meta` under pi's output-schema guard — error results
   included (`isError` preserved, MCP error results inspectable instead of thrown), text otherwise,
   with pi's `Tool "<name>" failed` fallback (`execute.ts toScriptValue`, `tools.ts
   convertMcpResult`). Four separation cases added to `CodemodeRoutingTest`.

## Recorded as Deferred in ADR 0066 (10 pi-pointered rows, no other differences remain)

tool_search membership row · stdio stderr tail · external sign-in pickup · background-connect
nuance · multi-candidate `ui.select` · TUI sign-in timeout bound/classification · MCP result
conversion presentation (20 KB truncation + temp file, resource-link/binary rendering, renderer
details channel) · dynamic codemode description catalog (the consuming surface for the parsed
`codemode.mode`/`inlineBudget`) · `defaultTools` (no Pike counterpart) · `models.*` globals
(conditional on No-decision model rows).

The #865 absorbed follow-ups each carry their absorbing pointer in ADR 0066 (script→tool routing,
worker thread, guest packaging → #885; `/mcp` write half, login trigger, RFC 9728, resource
tools, GET stream, exposure policy → #884).

## Documentation landed

- `docs/research/spec-882-acceptance.md` — all ten user stories + Testing Decisions mapped to
  differential/bundle evidence; every story closes.
- ADR 0066 — slash-wiring delivery record, #885 codemode implementation record, #886 close-out
  revision, corrected settings-keys paragraph, refreshed package rows/Consequences.

## Gates (this tip, after the close-out edits)

- `ctest --preset vcpkg -L architecture`: **41/41** (incl. the -Werror warning gate and the parity
  gate; PARITY-4011 include evidence re-scanned after the close-out include-set changes).
- `bash scripts/format-check.sh 513290fc3`: **clean**.
- Focused: `codemode` 33/33, `mcp` 224/224, TUI/e2e/CLI MCP selections 16/16 and 47/47. Full suite
  was already green at the base tip (2904/2904, mcp|codemode|tui 1176/1176) and was not re-run per
  the close-out instruction.

## Commits (this close-out, oldest first)

| Commit | Message |
|---|---|
| `5c662b4fc` | `fix(mcp): align MCP sign-in outcomes with pi's messages (issue 886)` |
| `813eaa52d` | `fix(codemode): resolve script calls to pi's CallToolResult object (issue 886)` |
| `811e4cfdc` | `docs(adr): sweep ADR 0066 to the landed full-parity reality (issue 886)` |
| `4e23dbe2e` | `docs(research): land the spec 882 acceptance table (issue 886)` |
| (this file) | `docs: append the 886 close-out report (issue 886)` |
