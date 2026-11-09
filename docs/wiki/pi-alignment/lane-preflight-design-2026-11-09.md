# Read-only lane preflight: probe and design (2026-11-09)

## Probe and boundary

On `main` at `bae3a2b58345287d560048b4d938e2affe6a8a79`, the main checkout had pre-existing modified and untracked source, test, research and wiki files. `scripts/dev/lane-lock.sh status` reported free. A local process sample did not establish ownership of that work; the available T3 active-thread listing did not establish the state of every local session. Neither a free lock nor absence from a process sample establishes that the checkout or shared build is unused ([lane scheduling and concurrency rules](ops-lanes.md)). No code preflight was implemented, no lane acquired, and no build, tests, or evidence operation was run for this probe.

At the time of this initial probe, the locally installed `implement` skill called for TDD, a full test suite, review and a task-scoped commit. The user's then-current instructions prohibited builds, tests, pushing and external messages. Ownership uncertainty and that validation restriction prevented implementation in the main checkout. Subsequent isolated validation is recorded in the [usage document](lane-preflight-usage-2026-11-09.md).

## Existing coverage

- `git status --short`, `git branch --show-current`, and `git rev-parse HEAD` report a point-in-time checkout state. Status can contain sensitive filenames; default output should summarize counts, not print paths or file contents.
- [`scripts/dev/lane-lock.sh status`](../../../scripts/dev/lane-lock.sh) reports free or a holder, age and staleness. It neither proves ownership nor excludes other activity. The holder metadata should not be echoed blindly: a corrupted or manually written lock is untrusted.
- [Validation](../../agents/validation.md#implementation) names `ctest --preset vcpkg -N` with a specific selection to inspect matches, and warns that incomplete build trees register only a subset. Counts establish neither coverage nor a passing test.
- [Build readiness](build-readiness-2026-11-05.md#4-pi_checkout-sibling-with-working-esbuild) identifies the pi-v1.3.0 frozen revision and notes that `PI_CHECKOUT` may select an upstream checkout; [`fixtures/pi-ai/README.md`](../../../fixtures/pi-ai/README.md) describes its default. A path existing alone does not establish frozen identity or cleanliness. Other baselines require their own explicitly specified revision.

## Proposed small interface

A standalone read-only script should require an explicit lane identifier and expected frozen revision, with an optional explicit checkout path and an optional complete CTest selection plus expected count. It should:

1. Verify the current Git root, branch and HEAD; summarize staged, unstaged and untracked status without filenames. Treat command errors as unknown, and a dirty tree as requiring owner review, not as proof of another lane.
2. Read the lock safely without acquiring, releasing, clearing or taking over it. Distinguish absent, readable matching identity, readable foreign identity and malformed/unreadable state; validate lane identifiers using the lock tool's whitelist. Never infer that a matching string proves the caller owns a lock, and never report a free lock as authorization to start.
3. Check the explicitly selected upstream checkout's Git root, full HEAD against the caller-supplied frozen revision, and tracked and untracked cleanliness. Report missing inputs, errors and mismatches separately; do not fetch, reset, clean, install, or change the checkout. A clean revision is a prerequisite, not proof that a capture is correct.
4. Check presence of tools required by the requested mode using executable lookup, without printing environment values. If selection and expected count are explicitly supplied, use the existing CTest listing mechanism only with an already configured tree, report the observed count and a subset-registration warning, and do not run tests. Otherwise skip selection entirely. Reject ambiguous or incomplete selection parameters.
5. Emit a compact status with pass / attention / unknown and a nonzero result for failures or unknown required inputs; never emit secret values or raw untrusted metadata. These are snapshots subject to races, not a lock or a lane authorization decision.

The design aims to avoid creating output files, modifying Git index metadata, or touching evidence. Git receives a restricted environment and optional locks are disabled, but repository configuration and filesystem behavior are not sandboxed; no unconditional no-write guarantee follows. Paths, error messages and CTest test names are potentially sensitive. See the usage document for the conservative status boundaries.

## Status and environment boundary (2026-11-09)

Inherited Git variables, including trace destinations and injected configuration, are excluded. Global and system configuration are disabled; repository configuration remains in scope. Configured filters, includes and trace2 settings make status unknown, as do hidden index flags and submodule entries. Status ignores ignored files, and its multiple Git calls are subject to races. These restrictions do not contain a hostile repository, executable or filesystem. Deep or malformed lock JSON is unknown without exposing its contents.

## Selection design adjustment (2026-11-09)

The no-write requirement takes precedence over the proposed CTest listing. In a disposable tree, CTest 4.4.5 `-N --show-only=json-v1` created `Testing/Temporary/LastTest.log` and executed a `file(WRITE ...)` in `CTestTestfile.cmake`. A `bwrap` probe with the filesystem read-only and network namespace isolated blocked that write, but CTest exited 8 because it also could not write its log. This does not establish a portable, complete sandbox that both permits listing and prevents other side effects. Redirecting the log alone does not constrain commands in the test definitions.

Until such a mechanism is established, supplying the regex and expected count produces **unknown**, not a count or a pass, and does not invoke CTest. Omission skips the check. A separately run CTest listing is an operator action outside this script's read-only contract; an observed count still cannot establish complete registration or passing tests. Any future design must preserve CTest's selection semantics and establish containment before reporting a count.
