---
status: accepted
---

# Retire workspace containment and align filesystem path resolution with pi resolveToCwd

ADR 0026 introduced fail-closed workspace containment as an intentional security divergence from
pi, and ADR 0034's #619 addendum later moved only the write/edit path scope to pi `resolveToCwd`,
leaving three resolution scopes — addressed-path, read (workspace plus the #629
`AuthorizedSkillRoots` allowlist), and write — behind one filesystem seam. Issue #696 measured the
resulting fracture: an agent can write or edit a file under `/tmp` but cannot read it back; system
headers, external repositories, and OS logs are unreadable; skills carrying absolute paths fail at
the tool-call boundary; and the codebase carries the split as cross-layer debt (three resolution
entry points, the specialized `read_text_file_for_write`, and skill-root plumbing through Session
assembly).

The owner has decided (#696): **Pike's filesystem path resolution uniformly aligns with upstream
pi's `resolveToCwd` contract.** One resolution operation serves every filesystem operation — read,
write, edit, metadata, listing, removal, and directory creation — and the shell working directory.
This ADR supersedes ADR 0026's "containment" divergence and ADR 0034's split resolution scope.

## The uniform resolution contract

Every tool path resolves under pi's `resolveToCwd` contract, as enumerated here:

- Absolute paths are honored as-is across the host filesystem after lexical normalization.
- Relative paths resolve against the workspace working directory; `..` segments are resolved by
  lexical normalization rather than rejected.
- Home-relative paths (`~`, `~/...`) expand against `$HOME`.
- Leading `@` prefixes (CLI and prompt conventions) are stripped.
- Unicode whitespace variants are normalized to standard spaces.
- pi's `file://` URL conversion is not mirrored: URLs are not paths on this seam. This is the one
  deliberate narrowing of the upstream function, inherited unchanged from ADR 0034's #619 addendum;
  #696's "100% alignment" enumeration does not list it.

There are no blocklists, no path sandboxing, no skill-root allowlists, no backward-compatibility
shims, and no fallback resolution paths. Filesystem access authority is exactly that of the host
user process; inaccessible paths surface standard OS-level errors (`NotFound`, `PermissionDenied`)
rather than artificial containment errors. The policies that survive are orthogonal to path
authority and remain in force: secret redaction (as narrowed for User Bash values by ADR 0028),
bounded output, the bash environment filter, the no-follow symlink policy and atomic writes, and
the documented "not a sandbox" boundary. The Bash authorization split itself — User Bash as a
direct-user capability independent of the model-requested Bash Tool — is ADR 0026's core decision
and is untouched.

## Considered options

- **Keep containment on read and metadata operations with the skill-root allowlist**: rejected —
  the split produces the contradictory experience #696 catalogues (write succeeds where read
  fails), blocks ordinary agent work (system headers, external checkouts, temp outputs), and the
  allowlist is plumbing debt across the runtime, Session assembly, and test seams. #619 already
  adjudicated this question for the write scope in favor of full pi alignment.
- **Adopt a Kimi-style sensitive-file blocklist**: rejected — #696 decides against it
  (the investigation lives in the still-open #630): parity with pi is the chosen design, and a
  pattern blocklist would be a second, weaker policy source
  that neither pi parity nor OS permissions require.
- **Keep a constrained escape hatch (e.g., a temp-dir allowance)**: rejected — any enumerated
  exception list re-creates the fractured experience at a smaller scale and preserves the
  multi-scope machinery this decision exists to delete. ADR 0053's clean-end-state principle
  forbids dual resolution paths as a steady state.

## Consequences

- `WorkspaceFileSystem` consolidates on one path-resolution operation matching `resolveToCwd`;
  `resolve_addressed_path`, `resolve_read_path`, `resolve_write_path`, `read_text_file_for_write`,
  `AuthorizedSkillRoots`, and the directory-descriptor containment machinery
  (`open_authorized_skill_parent`, `authorizing_skill_root`, `inside_lexically`) are deleted. The
  `edit` tool reads through the ordinary `readTextFile` operation. Implementation lands under #696.
- `fileInfo`, `listDir`, `exists`, `createDir`, and `remove` operate on the uniformly resolved
  path without workspace-parent constraints, and `AsyncLocalShell` working-directory overrides
  lose the workspace containment restriction. The bash environment filter and Bash authorization
  boundaries are unchanged.
- Skills and prompt mentions carrying absolute, home-relative, or `@`-prefixed paths resolve
  without an authorization list; the #629 revocation contract for skill roots is moot because the
  list itself no longer exists.
- Security review of file-touching changes shifts from scope preservation to the retained
  policies: `CODING_STANDARDS.md` §10.4 now requires routing through the one resolution operation
  and flags any file-tool path that bypasses it or duplicates its checks elsewhere.
- `docs/agents/architecture.md` §Security and containment records the end state: filesystem
  access aligns with host user process permissions using `resolveToCwd`.

## References

- Issue [#696](https://github.com/lanshengzhi/cpp-coding-harness/issues/696) (problem statement,
  implementation decisions) and [#697](https://github.com/lanshengzhi/cpp-coding-harness/issues/697)
  (this record).
- Issue #619 (write/edit realignment to `resolveToCwd`, closed), #629 (skill-root allowlist,
  closed; retired here), #630 (Kimi-style blocklist investigation, open; decided against in #696).
- pi `resolveToCwd`: `packages/coding-agent/src/core/tools/path-utils.ts`
  (`resolvePath(filePath, cwd, { normalizeUnicodeSpaces: true, stripAtPrefix: true })`).
- ADR [0026](0026-separate-user-bash-from-model-bash-authorization.md) (containment divergence,
  superseded in that clause only), ADR [0028](0028-pass-user-bash-text-values-through-raw-like-pi.md)
  (redaction narrowing, retained), ADR [0034](0034-own-the-scoped-pi-agent-core-agent-and-agent-turn-capabilities.md)
  (split-scope addendum, superseded), ADR [0053](0053-replace-pi-parity-authority-with-the-product-architecture-contract.md)
  (clean end state, no compatibility shims).
