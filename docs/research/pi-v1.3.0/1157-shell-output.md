# Durable shell output window (#1157)

## Scope

The local durable environment bounds its retained shell results by a caller-supplied byte and line window. Combined output, stdout and stderr retain separate tails and separate omission counts. The streaming callback receives all decoded chunks; it must manage its own retention.

## Delivered behavior

The result reports the omitted decoded UTF-8 byte count, newline count and whether the omitted prefix ended in a newline. Invalid byte sequences are decoded with U+FFFD, separately per stream. Pipes continue draining after the window fills. The default is 50,000 bytes and 2,000 lines for each retained result.

The adapter does not implement spill options or preserve omitted output in a file. Omitted result text cannot be recovered from the environment unless the caller records the streaming callback. This is an explicit remaining parity gap with frozen pi's optional `spill` operation, which preserves complete raw output in a temporary file when thresholds are crossed.

Frozen evidence `audit-1157` captures the Linux `NodeExecutionEnv` with a four-byte window: it sends all 16 bytes and no skip record. Its interface permits an environment to omit output, but this adapter does not. The bounded result and omission metadata are the user-approved robustness behavior, not a claim that frozen Node itself emits skips.

## Validation

Focused CTest: `Durable shell retains bounded tails and reports omitted output` selected 1, passed 1. The environment test covers output inside and outside the window, byte and line limits, separate streams, invalid UTF-8 and a 10 MiB command. It verifies retained length and exact omission totals. It does not measure process RSS, allocator capacity, callback allocations, or kernel pipe buffers; the implementation keeps three bounded strings plus one read chunk and decoder state.

Full CTest: selected 3,944 after registering `audit-1157`, passed 3,942, failed 2, skipped 1; the two failures were the pre-registration consolidation checks, which passed after rebuilding the ledger. The consolidation audit selected 42 Python cases and passed 42. The production parity build gate passed. `scripts/format-check.sh HEAD` reported clean. A rerun of the named-evidence lane fails only on its local `../pi` esbuild optional dependency, before the capture assertion, and is unrelated to this slice.

## Capability status

This slice bounds retained results; it does not claim the complete environment contract or spill support.

## Remaining boundary

Spill, callback backpressure and output pacing are not implemented. Output passed to callbacks may be retained without limit by the caller. Exit due to timeout or cancellation returns an error rather than a partial result.
