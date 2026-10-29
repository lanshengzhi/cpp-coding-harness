# Durable local environment and coding tools (#1089)

## Scope

Durable owns an injectable Linux file/process environment and an explicitly installed coding-tools extension. The ordinary application Session remains separate; the durable Owner does not depend on agent-core or the application.

## Delivered behavior

The public Harness executes read, write, edit and bash registrations against the injected environment. The local adapter offers file read, atomic replacement, append, stat and directory listing, shell or direct argv process execution with separate stdout/stderr, timeout and cancellation, and nonrecursive file or directory watch. Recursive watch returns a Validation error. Frozen observations are registered in `audit-1056/environment.json` and `audit-1089/environment.json`.

## Validation

Focused `issue1089`: 2 selected, 2 passed, 0 failed, 0 skipped. The Harness test compares all five frozen tool results and the final file, and separately asserts injected write failure and absence of filesystem fallback. The environment test checks missing paths, append/list/stat, argv and split output, distinct timeout and cancellation, file and directory watch, post-close callback stability and explicit recursive rejection. Architecture: 43 selected, 43 passed, 0 failed, 0 skipped. The configured `cch_durable` dependencies are AI, Chord and Support plus Boost headers and SQLite; no durable source includes agent-core or application paths. The production build parity gate passed. Full CTest selected 3836: 3833 passed, 2 failed, 1 skipped. Both failures were consolidation checks affected by the new evidence registry; after classifying and rebinding the evidence, their focused rerun selected 2 and passed 2. The skipped case was the Native TUI differential capture. Issue1101 revalidation on commit `517fb5eb610d58131f3682c68d90cfc1043b5118` selected 14, passed 14, failed 0 and skipped 0. Consolidation audit selected 39 Python cases and passed 39; consolidation check reports zero uncovered scenarios.

## Capability status

Membership, implementation and verification remain distinct. The focused tests compare the five-tool workflow with the frozen output and exercise an injected failing write, local file/process outcomes and nonrecursive watch. They do not verify the full upstream environment surface.

## Remaining boundary

- #1156: recursive watch, exclusions, overflow/error notifications and mode — delivered by #1156 (`1156-recursive-watch.md`).
- #1157: shell output window, skip and spill are not aligned. Output is retained without a bound and a long-running or noisy command can exhaust memory.
- #1158: bounded, paged and filtered listings and read offset/limit are not aligned.
- #1159: edit BOM/CRLF normalization, file mutation queue and diff details are not aligned.
- #1160: the final symlink check and atomic rename are not protected against a concurrent path replacement.
