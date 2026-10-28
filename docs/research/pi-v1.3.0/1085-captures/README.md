# Issue 1085 preliminary observations

The original registered `audit-1085` manifest is pinned at
`52aa87935fff0a1b8bdaca839320d5ae469283486c35b4a14d42e763903bb07e`.
Its recorded command omits the required output-directory argument. Running the
command shape with an empty probe directory failed with the error in
[registered-command-failure.txt](registered-command-failure.txt). Registry
verification checks integrity; it does not execute the capture command.

`harness-a.json` through `harness-c.json` are the original independent captures.
`harness-d.json` through `harness-f.json` used a corrected command before the
original registered bytes were restored. `harness-g.json` through
`harness-i.json` are independent runs of the separate replay script. For each
triplet, the expected observations matched after replacing only the generated
provider session UUID with `<session-id>` and sorting JSON object keys. These
are pi-only single-task observations; none establishes cross-runtime acceptance.
The replay family is registered as `audit-1085-replay` through issue #1169, which
allows `audit-<ticket>-<variant>` alongside the primary `audit-1085` family. The
original family and its pin are unchanged. Re-running the registered replay
command from the repository root succeeds; [replay-rerun.txt](replay-rerun.txt)
records the only differences from the pinned capture: the capture timestamp and
the generated provider session UUID.

[writer-red.txt](writer-red.txt) records the real frozen pi rejection of a
C++-written pending task before sidecar publication was implemented. The test
subsequently passed with pi actually executing it. It does not establish the
full issue: an uninterrupted task across both transitions, submission settlement,
document history and failure/recovery remain to be verified.

[reclaim-best-effort-red.txt](reclaim-best-effort-red.txt) and
[reclaim-best-effort-green.txt](reclaim-best-effort-green.txt) show that a
recovery-time sidecar reclamation failure used to fail the reopen even though
the terminal marker was already published; recovery now leaves the sidecar for
a later retry. The fixture probes whether directory permissions actually deny
`unlink` and skips when the process bypasses them.

[consolidation-variant-check.txt](consolidation-variant-check.txt) records the
consolidated ledger check after variant families became indexable. Because
`scripts/evidence/audit_evidence.py` and the consolidation tool are pinned
inputs of the `1101-structured-content` delivery, the delivery was revalidated
at `875099ba918384744a5e88cf1eff7e19cf7b1dbf`; [revalidate-1101.txt](revalidate-1101.txt)
records the 14 passing tests and the receipt now points at
`deliveries/1085-revalidation-1101.xml`.

[stale-identity-red.txt](stale-identity-red.txt) and
[stale-identity-green.txt](stale-identity-green.txt) cover an empty task
sidecar reclaimed during recovery: a later commit for the same task must create
the sidecar without failing against the removed identity. [replacement-red.txt](replacement-red.txt)
and [replacement-green.txt](replacement-green.txt) cover the follow-on window:
a regular file that reappears after reclamation must not be adopted and appended;
the commit fails closed and its bytes stay unchanged.

[focused-jsonl-27.txt](focused-jsonl-27.txt) is the full
`PI_CHECKOUT=/tmp/cch-1080-pi-frozen ctest --preset vcpkg --output-on-failure
-L jsonl` run at `d3feb4bbe0fcb42c121dbcbc9805f0b64435873a`: 27 selected, 27
passed, 0 failed, 0 skipped.
