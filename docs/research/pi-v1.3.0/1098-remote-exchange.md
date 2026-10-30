# #1098 generic Remote exchange and Linux host

## Scope

This is the owning delivery record for #1098 under #1044 and [ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md). The named baseline is **pi-v1.3.0 / 71023aec1546d6d677a65669c5665c0927ce0dd0**, Linux x86-64 with glibc. It records the bounded generic Remote exchange and Linux host, not a whole-row parity receipt.

The delivered merge was `e426fe8d4a76c2a7893506976b1b1134175096d4`: `5ca172c7e4d6ee54a6134aab68464eb9e320a55a` added the host, `a42a2d519df40790ffb56a87c3f0d3f5c613e6e3` recorded initial evidence, `2bb01652305981179db7f496ae3d0b734d1d1e64` hardened lifecycle handling, and `791d7024ce295192498270b7eaf5ccb99670d52a` refreshed evidence. All four and the merge are ancestors of the integration revision examined for this record, `63223b51e78db567a3fe5909b65692e6c8b1f44c`. The merge is also an ancestor of #1072's later validated revision `6ae4ee6d899c4c33274e21fc658c1074e5c4a881`, itself an ancestor of the examined revision.

## Delivered behavior

The public [Remote interface](../../../src/codemode/include/cch/codemode/Remote.hpp) provides `CodemodeRemote`, `RemoteCodemodeSandbox` and `serve_codemode_remote`; the [implementation](../../../src/codemode/Remote.cpp) runs the guest on the Linux host and routes tool calls back to the caller through the exchange. The [public-caller tests](../../../tests/codemode/CodemodeRemoteTest.cpp) exercise both ends. The normal case returns `2` from caller tool `add`, records `hi`, and sees `[output, call:add]` followed by `[done]`, with result and null replies. The batching case retains 5,000 console items in six exchanges of 1,000, 1,000, 1,000, 1,000, 1,000 and one terminal message.

The host applies interrupt and memory caps, yielding distinct timeout and out-of-memory script errors. Caller abort cancels a waiting tool, returns an aborted result and replies stop. A deliberately slow remote delays exit by 600 ms after stop; the test checks settlement below 150 ms on non-sanitized builds. A separate close case checks an active run settles, a stop is received while the remote has not returned, and subsequent runs are rejected. A stop during output flush suppresses the terminal message. A gated late cleanup failure cannot replace an already settled success; failing, silent and unreachable remotes retain three distinct sandbox error messages. These cases separate settlement from remote return and late failure; they do not themselves constitute a measurement of the time of actual worker exit or a retained cleanup trace.

## Validation

The independently captured [audit-1053 Remote artifact](../../../fixtures/pi-v1.3.0/audit-1053/remote-exchange.json), registered in its [manifest](../../../fixtures/pi-v1.3.0/audit-1053/manifest.json), retains six frozen scenarios. Five concern this slice: caller tool/exchange, output batching, caps, abort/stop and transport failure taxonomy. The sixth is reordered deterministic Remote delivery and belongs to #1099. The first five provide named inputs and observations, but the C++ tests use their own assertions rather than a complete registered comparison receipt; for example, the normal C++ test does not assert every captured store-write field. The abort capture records stop and outcome, not an actual-exit timestamp.

Historical results from the [#1098 delivery comment](https://github.com/lanshengzhi/cpp-coding-harness/issues/1098#issuecomment-6382488986): **9/9 Remote**, **91/91 codemode**, **43/43 architecture**; Full Validation **3,690 passed, 1 skipped, 0 failed**, with incremental build and architecture gate passing. These are reported historical counts; no #1098 JUnit or actual-exit/cleanup trace was found among the retained delivery artifacts. The later [#1072 integration validation](https://github.com/lanshengzhi/cpp-coding-harness/issues/1072) reported **3,732 selected, 3,731 passed, 0 failed, 1 optional skip**, after an earlier unrelated worker timeout and an unfiltered rerun at two CTest jobs. It is independent historical integration evidence, not a new Remote comparison or terminal acceptance.

For this record, ancestry was checked with `git merge-base --is-ancestor`. The Remote implementation, public header, test, guest and sandbox sources, release-gate support and frozen Remote artifact have no diff from the merged revision. No pinned receipt input was changed, and the Remote selection was not rerun for this documentation-only delta. Current evidence verification and consolidation checks are recorded in the delivery summary; neither check promotes a capability.

## Capability status

Membership, implementation and verification remain separate. The existing ledger records the Remote implementation and upstream capture; this report supplies neither the complete pinned C++ comparison nor the whole-row receipt required to mark a capability verified. No ledger, routing, artifact or receipt status is changed. #1179 owns whole-row codemode reconciliation.

## Remaining boundary

#1099 remains open for deterministic Remote replay and reordered replies. #1171 remains open for bidirectional durable interrupted-journal continuation; this record makes no interchange claim. #1044 remains open. The retained Magpie-only AI path, XDG state root, strict no-exception boundary and ADR 0075 exclusions apply, including Cloudflare-specific deployment, the standalone pi-env SSH daemon, third-party pi TS/JS plugins and historical Pike durable migration/fallback. No new transport or deployment is delivered here.
