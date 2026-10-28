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
The replay candidate is unregistered because the registry requires `audit-<ticket>`
and one family per ticket. Supporting multiple captures for a ticket would need
an independent registry/tool change, not a fabricated ticket number.

[writer-red.txt](writer-red.txt) records the real frozen pi rejection of a
C++-written pending task before sidecar publication was implemented. The test
subsequently passed with pi actually executing it. It does not establish the
full issue: an uninterrupted task across both transitions, submission settlement,
document history and failure/recovery remain to be verified.
