---
status: accepted
---

# Own the Capability Owner Package graph and Parity Architecture Gate

The repository has four authoritative Capability Owner Packages: `cch_ai`, `cch_agent_core`, `cch_tui`, and repository-private `cch_coding_agent`. One pi-neutral C++ Support Package, `cch_support`, provides only shared C++ values and mechanics and owns no Supported Capability. One strict, versioned Parity Architecture Manifest defines the legal Owner graph and evidence policy, and one fail-closed Parity Architecture Gate enforces that policy from configured build evidence. Owner Interfaces remain repository-internal; the only released product is the `cpp_harness` Runtime for native Linux x86-64 with glibc.

This decision records the destination frozen by architecture spec [#439](https://github.com/lanshengzhi/cpp-coding-harness/issues/439) for documentation ticket [#440](https://github.com/lanshengzhi/cpp-coding-harness/issues/440) without reopening it. User-visible capability behavior remains governed by [#396](https://github.com/lanshengzhi/cpp-coding-harness/issues/396), the planning authority remains parity map [#2](https://github.com/lanshengzhi/cpp-coding-harness/issues/2), and Semantic Parity remains pinned to pi commit `83114817c68f5413e4d7ba6d7003ddc511cd31d2`. muduo, Boost.Asio, and other implementations are research inputs, not product authorities. This ADR neither advances the Parity Baseline nor adds a Supported Capability.

## Capability Owner Packages and legal graph

| Package | Authoritative ownership | Legal direct Owner dependencies |
|---|---|---|
| `cch_ai` | Model, Provider, authentication, and model-stream behavior | none |
| `cch_agent_core` | Agent, agent harness, and Tool behavior | `cch_ai` |
| `cch_tui` | reusable terminal, input, rendering, and TUI Toolkit behavior | none |
| `cch_coding_agent` | repository-private Agent Session, Models Runtime, Native TUI application, CLI, and Runtime composition | `cch_agent_core`, `cch_ai`, `cch_tui` |

Every unlisted cross-Owner edge is forbidden. A cross-Owner edge targets the downstream Owner's one authoritative compiled static library, never one of that Owner's private implementation libraries. Same-Owner implementation targets may deepen an Owner, but remain private to it. Each production source compiles exactly once, every production target has exactly one declared role (`owner`, `implementation`, `support`, `composition`, or classified `external`), and the production graph is acyclic.

`cch_support` replaces `cch_util` without an alias or fallback. It may depend only on support targets and classified external targets. Its scope is limited to pi-neutral mechanics genuinely required across Owners, including `Error`/`Expected`, passive `JsonValue`, `AsyncResult`, and necessary move-only completion and Runtime mechanics. Product policy, process policy, serialization, redaction, Unicode handling, and output limiting remain with their Capability Owner unless actual cross-Owner reuse passes the deletion test. Final composition depends only on Owner libraries, support, and classified external imported targets.

## Owner Interfaces

An Owner Interface is a repository-internal header contract, not an installed SDK or ABI promise. Owner-local header roots provide one canonical `<cch/...>` angle-include spelling. Each Owner Interface header compiles independently and contains only standard-library types, legal downstream Owner types, and support types. Third-party types and headers, serialization DTOs, schema conversion, visitors, parsers, and other generic machinery stay private.

Umbrella headers, forwarding headers, compatibility aliases, alternate include spellings, and macro-generated include aliases are prohibited. Generated Owner Interface headers are also prohibited except for declared, configuration-independent pure values such as build identity.

## Supported Platform and toolchain

The Supported Platform is native Linux x86-64 with glibc only. Ubuntu 24.04 is the reproducible baseline and blocking release environment. A digest-pinned Arch Linux snapshot is a supported native-development validation environment; current Arch latest is scheduled advisory drift. WSL2 is development-only and never substitutes for native-Linux CI.

GCC 16.x is the sole build and release compiler. Clang 22.x is a blocking Linux conformance verifier and does not produce release artifacts. CI and releases pin exact GCC, CMake, and Ninja versions; the supported floor is CMake 4.4 or newer and Ninja 1.11 or newer, with the pinned vcpkg dependency path. GitHub Actions owns the blocking matrix, and release builds use validated IPO/LTO.

Other operating systems, architectures, and C libraries are unsupported. Cross-compilation, Unix Makefiles, and Unity Build are also unsupported. Unsupported platforms and cross-compilation fail at configure time rather than retaining best-effort product paths.

## Runtime-only release

`cmake --install` installs only the `cpp_harness` Runtime, required runtime resources, and required licenses/notices. It installs no Owner Interface headers, static libraries, SDK, CMake package, exported targets, components, compatibility files, or other development surface. Packaging format and dynamic-dependency treatment follow measured linkage evidence and are not product interfaces.

A clean staging-prefix smoke verifies Runtime resource relocation, dynamic dependency closure, licenses/notices, `--help`, `--version`, deterministic offline startup, Ubuntu baseline execution, pinned Arch execution, and the absence of development artifacts.

## Parity Architecture Manifest and Gate

The Parity Architecture Manifest is the single strict, closed, versioned policy authority. It records:

- the frozen baseline pin;
- the exact legal Owner DAG, Owner roots, and production roles;
- capability-evidence references; and
- the exact imported-target families permitted for each Owner and role.

It does not duplicate the target/source inventory. Unknown schema versions or fields, duplicate or conflicting declarations, and unclassified dependency families fail closed. A project wrapper never counts as an external target family. Each capability-evidence reference names its producer, input identities, configuration identity, content digest, and validity condition; changed inputs, configuration, manifest, baseline, or an older producer schema make the evidence stale. Parity Drift against another pi revision is advisory only after a complete valid audit; an incomplete or invalid audit fails. The validator uses Python 3.12 or newer and only the standard library.

Central CMake constructors declare each target's role, Owner, sources, direct dependencies, and defaults. Target dependencies are explicit and unconditional, so configurations cannot produce different project architectures. Configure validates those declarations and emits a resolved ownership index. After generation, the Gate requires the manifest, the fresh index, completed CMake File API replies, compile commands, and direct-include lexer output. Build, test, install, and release phases additionally require fresh GCC or Clang depfiles as active transitive-conformance evidence.

A preprocessing-directive lexer scans direct project-header includes in every branch, including disabled branches, and resolves them in their configured context. It also scans declared forced includes and PCH inputs; opaque PCH artifacts are forbidden. Depfiles prove only the active transitive closure and never authorize an illegal direct include.

Missing or stale evidence; unclassified files, roots, sources, targets, roles, or external families; ownership collisions; illegal target or include edges; unsupported include-affecting flags; generated-file gaps; environment leakage; path, include-root, or symlink escape; case aliases/collisions; absolute project paths; ambiguous resolution; and contradictory evidence all fail. Diagnostics have stable rule identifiers and deterministic human-readable and JSON forms.

Every supported configure, normal build, test, install, release, and CI entry point runs or depends on the latest applicable phase of this same Gate, including configurations with tests disabled. Structural constraints such as unique compilation, legal fan-out, bounded queue/waiter structures, and classified dependencies block immediately. Numeric allocation, size, fan-out, code-size, wall-time, and other performance thresholds become blocking only after repeatable same-environment measurements record a baseline, variance, selection rule, and update procedure.

## Considered options

- Keep the current target graph and source-text architecture checks: rejected because neither configured target relationships nor stale/contradictory evidence can fail closed.
- Let every historical package remain an Owner and share a global public include root: rejected because ownership becomes ambiguous, implementation splits become cross-package shortcuts, and `cch_util` can accumulate product policy.
- Generate a second target/source policy inventory: rejected because it can drift from CMake declarations; generated ownership is evidence, not a second authority.
- Validate only active compiler includes: rejected because disabled direct includes could hide an illegal Owner dependency; depfiles cannot authorize textual direct edges.
- Preserve a public or installed C++ SDK alongside the Runtime: rejected because the approved product is Runtime-only and repository-internal Owner Interfaces carry no external compatibility obligation.
- Keep Linux primary while retaining best-effort macOS, Windows, or cross-build paths: rejected because unverified output would appear supported and the release boundary would not be reproducible.

## Consequences

- Capability ownership, legal dependency direction, and support ownership are visible in the configured graph and enforced independently of optional test builds.
- Third-party and serialization machinery cannot leak through Owner Interfaces, and no private implementation target can become a cross-Owner shortcut.
- Adding a target, source, include root, imported family, or evidence producer requires explicit classification; omission is a failure rather than implicit permission.
- Parity Drift remains advisory and cannot silently advance the frozen product authority.
- The install tree is intentionally unsuitable for external C++ consumers.
- [ADR 0024](0024-record-and-explicitly-advance-the-pi-parity-baseline.md) remains authoritative for deliberate baseline advancement, and [ADR 0036](0036-own-the-scoped-pi-coding-agent-application-layer-capabilities-for-the-three-provider-paths.md) remains authoritative for the selected application behavior and removal of the SDK/JSON/RPC surfaces.

## Superseded clauses

Only conflicting topology, platform, release, and terminology clauses are superseded; unaffected capability semantics remain authoritative. The older records carry inline notices identifying the exact affected clauses:

- [ADR 0005](0005-keep-provider-and-product-messages-in-their-owning-modules.md) and [ADR 0015](0015-do-not-add-a-local-default-turn-limit.md): old physical `cch_agent`/`cch_harness`/`cch_coding_agent_runtime` package names (message ownership and no-default-turn-limit semantics remain);
- [ADR 0025](0025-own-a-modular-native-tui.md): public/installed interpretation, Linux/macOS support, best-effort non-TUI platform paths, and JSON/RPC/SDK frontend precedence;
- [ADR 0026](0026-separate-user-bash-from-model-bash-authorization.md): removed `--enable-bash`/JSON/RPC/SDK surfaces and Linux/macOS platform wording (User Bash semantics remain);
- [ADR 0029](0029-align-models-provider-and-authentication-ownership-with-pi.md): `util` package terminology and the high-level SDK/Models Runtime injection topology (the replacement Runtime operation shape is recorded by ADR 0040);
- [ADR 0034](0034-own-the-scoped-pi-agent-core-agent-and-agent-turn-capabilities.md): the direct `ModelRuntime::streamSimple` injection shape and `util::Expected` package terminology (replaced by ADR 0040 and `cch_support`; stream/error-channel semantics remain);
- [ADR 0035](0035-own-the-scoped-pi-tui-toolkit-capabilities-for-the-three-provider-paths.md): every “public” toolkit/module/header reference now means a repository-internal `cch_tui` Owner Interface, never an installed consumer surface; and
- [ADR 0038](0038-reproducible-build-toolchain-floor-gcc-16-cmake-4-single-vcpkg-path.md): the CMake 4.0 floor, loose GCC 16+ release rule, and best-effort platform range.

## References

- Architecture and Runtime spec [#439](https://github.com/lanshengzhi/cpp-coding-harness/issues/439), decisions 1–13, 34–35.
- Application behavior authority [#396](https://github.com/lanshengzhi/cpp-coding-harness/issues/396).
- pi C++ parity map [#2](https://github.com/lanshengzhi/cpp-coding-harness/issues/2).
- Frozen pi authority commit `83114817c68f5413e4d7ba6d7003ddc511cd31d2`.
