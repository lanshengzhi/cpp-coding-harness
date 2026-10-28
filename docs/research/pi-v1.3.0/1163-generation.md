## Scope

Issue #1163 compares durable generation against pi-v1.3.0 commit `71023aec1546d6d677a65669c5665c0927ce0dd0`. This record does not promote a capability ledger row or replace previously registered captures.

## Frozen evidence

The adjacent `1163-generation/` directory preserves probe source and twelve raw public Harness watch captures. The probes ran in an isolated copy of the frozen checkout, from `packages/durable`, using its Vitest configuration and existing faux provider. The copy's workspace symlinks resolve to the read-only frozen checkout; no upstream sources were changed. Run `CCH_1163_CAPTURE=/tmp/new-unique-file ../../node_modules/.bin/vitest run test/issue1163-probe.test.ts` for the normal probe and select `-t aborted`, `-t retry`, or `-t tool` for the lifecycle probe after copying the respective test into `packages/durable/test/`. Capture creation uses exclusive file creation.

Each probe replayed every watch operation into the delivered view and compared the final replay with fresh hydration. Normal was repeated three times: each produced three frames, with `generation: {attempt: 1}` visible before stream invocation and final removal, assistant entry and usage in the final frame. The other three scenarios each ran three times: provider-aborted produced three frames and `unanswered/model_error`, retry five frames and a successful settlement, tool nine frames and a successful settlement. Within each scenario the captures match after excluding dynamic timestamps, generated faux API identifiers, retry times and tool duration. The faux provider recomputes usage despite the values supplied by the probe. Actual input/output/cacheRead/cacheWrite/totalTokens were respectively 2/2/0/2/6, 2/1/2/2/7 and 45/6/33/46/130 for aborted, retry and tool.

The frozen source also specifies conversion of an already committed partial in the request and abort transactions, and usage on every assistant append including overflow recovery. Those particular boundaries were inspected, not independently exercised by the frozen probes above.

## Validation and limits

Before integration with #1164, the issue1163 focused selection had 7 selected, 7 passed, 0 failed and 0 skipped. The first run had 7 selected, 0 passed, 5 failed and 2 timed out; a missing intermediate Chord path in partial conversion was fixed, as was a separate invalid partial test setup. Passing reruns do not establish the root cause of every original failure.

Suppressing only the request generation set made the stream-time attempt assertion fail (1 selected, 0 passed, 1 failed). Suppressing only the assistant ledger write while retaining its append made the retry test fail its four ledger counter assertions (1 selected, 0 passed, 1 failed). These mutations do not prove every arithmetic case or transaction boundary. The restored baseline tests include three nonempty live document watch frames replayed against each delivered revision and final hydration, and a commit listener requiring one final assistant entry and usage document change in the same publication. A fault-injection case asserts that the rejected batch addresses the live document and no stream starts.

The current C++ tool round and observation seam are separately tracked work: these tests do not establish that the integrated #1088 conversation-watch scenario passes. Live partial streaming throttling and deferred polling remain outside this evidence. Full validation and integrated #1164 results are reported separately after they run.
