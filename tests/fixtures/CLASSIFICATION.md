# Fixture classification

This inventory is the fixture side of the de-pi test audit (#625). The
classification is about the product contract, not where a fixture originally
came from.

## `spec`

These fixtures exercise behavior Pike keeps as its own product contract:

- `install-gate/CMakeLists.txt`
- `install-gate/NOTICE.txt`
- `install-gate/main.cpp`
- `keybindings/README.md`
- `keybindings/pi-864b35c.json` — the default keybindings remain a user-facing
  product default (ADR 0053), not a runtime pi-data import surface.
- `parity-gate/illegal-include/CMakeLists.txt`
- `parity-gate/illegal/CMakeLists.txt`
- `parity-gate/legal/CMakeLists.txt`
- `parity-gate/poison/*.cpp`
- `parity-gate/src/**/*`
- `prompts/README.md`
- `prompts/goldens/*.txt` — the golden records Pike's identity delta and prompt
  contract, rather than an imported pi session/config record.
- `themes/README.md`
- `themes/*.json`
- `themes/goldens/*.txt`

## `compat-pi`

There are no standalone `tests/fixtures` files in this category. The
compatibility corpus that will move behind `compat/pi` lives under the root
`fixtures/pi-*` tree and is classified at the consuming test case (for example,
`AgentCoreEvidenceTest` and `SessionSuiteGoldenTest`) so `ctest -L compat-pi`
selects the behavior that owns each corpus.

## `diverge`

There are no standalone fixture files whose behavior changes in this audit.
The current divergence marker is a test case, not fixture contents:

- `ProjectionStreamTest` — streaming tool-partial recovery is owned by
  migration #622.

The former #626 agent-config/project-resource differences are now Pike
product behavior and are classified as `spec`.

A future ticket that changes a fixture must move it here and record its owning
migration ticket in the test's CTest labels.
